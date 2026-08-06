#include "duckdb/storage/compression/dict_fsst/compression.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/settings.hpp"
#include "fsst.h"
#include "duckdb/common/fsst.hpp"

#include <absl/strings/match.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace duckdb {
namespace dict_fsst {
namespace {

constexpr idx_t ENCODE_THRESHOLD = 4096;
constexpr idx_t ROW_HEADROOM = 4 * 1024;
constexpr idx_t ENCODE_HEADROOM = 16 * 1024;
constexpr idx_t DICT_STABLE_ROWS = 4096;
constexpr idx_t CLEAVE_GAP = 2 * 1024;
constexpr idx_t MODE_TIE_TOL = 2 * 1024;
//! What one prefix group costs in metadata: its prefix-lengths entry plus the amortized prefix-id width growth,
//! rounded up. Charged inside the DP so a group must SAVE more than it costs to form at all -- which both kills
//! net-negative junk groups and is what lets CleavedUpperBound charge anchored groups at a flat constant.
constexpr idx_t GROUP_COST = 2;

inline idx_t GuardedLcp(const unsigned char *a, idx_t la, const unsigned char *b, idx_t lb) {
	idx_t l = absl::FindLongestCommonPrefix(absl::string_view(reinterpret_cast<const char *>(a), la),
	                                        absl::string_view(reinterpret_cast<const char *>(b), lb))
	              .size();
	//! An FSST escape is 255 followed by its literal, and that literal may itself be 255, so a single
	//! test cannot tell a marker from a payload. Backing off the whole trailing run of 255s cannot land
	//! between a marker and its literal either way.
	while (l != 0 && a[l - 1] == 255) {
		l--;
	}
	return l;
}

constexpr uint32_t CLEAVE_NONE = 0xFFFFFFFFu;

//! Entry accessors over one cleave order: identity (row order, FSST_PLUS) or a permutation (sorted order,
//! DICT_FSST_PLUS). The one place the order indirection lives.
template <bool IDENTITY>
struct CleaveView {
	const vector<string_t> &encoded;
	const uint32_t *order;

	uint32_t O(uint32_t k) const {
		if constexpr (IDENTITY) {
			return k;
		} else {
			return order[k];
		}
	}
	const unsigned char *EP(uint32_t k) const {
		return reinterpret_cast<const unsigned char *>(encoded[O(k)].GetData());
	}
	uint32_t EL(uint32_t k) const {
		return NumericCast<uint32_t>(encoded[O(k)].GetSize());
	}
};

//! LCP + Cartesian tree + optimal-savings DP, shared by the emitting and the measuring walks. lcp[0, lcp_valid) is
//! trusted as already computed for this order: the identity caller persists it across cleaves (row order only ever
//! appends), the sorted caller passes 0 (the merge shifts positions). Groups are priced: a subtree is taken whole
//! only when the shared prefix saves at least GROUP_COST more than its children manage separately, so a group that
//! cannot pay for its own metadata never forms.
template <bool IDENTITY>
inline uint32_t CleaveDP(const CleaveView<IDENTITY> &v, uint32_t m, CleaveScratch &scratch, vector<uint32_t> &lcp,
                         idx_t lcp_valid) {
	lcp.resize(m);
	for (uint32_t i = NumericCast<uint32_t>(lcp_valid); i < m; i++) {
		lcp[i] = NumericCast<uint32_t>(GuardedLcp(v.EP(i), v.EL(i), v.EP(i + 1), v.EL(i + 1)));
	}
	auto &lc = scratch.lc;
	auto &rc = scratch.rc;
	lc.resize(m);
	rc.assign(m, CLEAVE_NONE);
	auto &mono = scratch.mono;
	mono.clear();
	mono.reserve(m);
	uint32_t root = 0;
	for (uint32_t i = 0; i < m; i++) {
		uint32_t last = CLEAVE_NONE;
		while (!mono.empty() && lcp[mono.back()] > lcp[i]) {
			last = mono.back();
			mono.pop_back();
		}
		lc[i] = last;
		if (mono.empty()) {
			root = i;
		} else {
			rc[mono.back()] = i;
		}
		mono.push_back(i);
	}
	auto &value = scratch.value;
	auto &sz = scratch.sz;
	auto &take_whole = scratch.take_whole;
	value.resize(m);
	sz.resize(m);
	take_whole.assign(m, 0);
	using DFrame = CleaveScratch::DFrame;
	auto &dstk = scratch.dstk;
	dstk.clear();
	dstk.push_back({root, false});
	while (!dstk.empty()) {
		const DFrame f = dstk.back();
		dstk.pop_back();
		const uint32_t i = f.node;
		if (!f.done) {
			dstk.push_back({i, true});
			if (lc[i] != CLEAVE_NONE) {
				dstk.push_back({lc[i], false});
			}
			if (rc[i] != CLEAVE_NONE) {
				dstk.push_back({rc[i], false});
			}
		} else {
			const idx_t vl = (lc[i] != CLEAVE_NONE) ? value[lc[i]] : 0;
			const idx_t vr = (rc[i] != CLEAVE_NONE) ? value[rc[i]] : 0;
			const uint32_t szl = (lc[i] != CLEAVE_NONE) ? sz[lc[i]] : 0;
			const uint32_t szr = (rc[i] != CLEAVE_NONE) ? sz[rc[i]] : 0;
			sz[i] = 1 + szl + szr;
			const idx_t whole = idx_t(sz[i]) * idx_t(lcp[i]);
			if (whole >= vl + vr + GROUP_COST) {
				value[i] = whole - GROUP_COST;
				take_whole[i] = 1;
			} else {
				value[i] = vl + vr;
			}
		}
	}
	return root;
}

//! One traversal for both consumers of the DP: entry ranges reach `plain(k)` one entry at a time or
//! `group(a, b, plen)` as a whole take_whole range. The emitting caller materializes in the callbacks and passes
//! its group budget; the measuring caller counts, with no budget (it reports what the DP wants, and RefreshCleave
//! raises the budget to cover exactly that before anything emits -- which is what keeps the clamp here a backstop
//! rather than load-bearing).
template <typename PLAIN, typename GROUP>
inline void CleaveWalk(CleaveScratch &scratch, const vector<uint32_t> &lcp, uint32_t m, uint32_t root,
                       idx_t prefix_cap, PLAIN &&plain, GROUP &&group) {
	auto &lc = scratch.lc;
	auto &rc = scratch.rc;
	auto &take_whole = scratch.take_whole;
	idx_t groups = 0;
	using EFrame = CleaveScratch::EFrame;
	auto &estk = scratch.estk;
	estk.clear();
	estk.push_back({0u, m, root});
	while (!estk.empty()) {
		const EFrame f = estk.back();
		estk.pop_back();
		const uint32_t a = f.a;
		const uint32_t b = f.b;
		if (a == b) {
			plain(a);
			continue;
		}
		const uint32_t mid = f.node;
		if (!take_whole[mid]) {
			estk.push_back({mid + 1, b, rc[mid]});
			estk.push_back({a, mid, lc[mid]});
			continue;
		}
		if (lcp[mid] == 0 || groups >= prefix_cap) {
			for (uint32_t k = a; k <= b; k++) {
				plain(k);
			}
			continue;
		}
		groups++;
		group(a, b, lcp[mid]);
	}
}

template <bool IDENTITY>
inline void CleaveEncoded(CleavedDictionary &dict, const vector<string_t> &encoded, idx_t n, const uint32_t *order,
                          CleaveScratch &scratch, vector<uint32_t> &lcp, idx_t lcp_valid, idx_t prefix_cap) {
	const CleaveView<IDENTITY> v {encoded, order};
	dict.entries.reserve(n);

	auto emit_plain = [&](uint32_t k) {
		PlusEntry e;
		e.prefix_id = 0xFFFFFFFFu; // temporary "no prefix" marker, fixed to the sentinel below
		e.suffix = v.EP(k);
		e.suffix_len = v.EL(k);
		e.original_index = v.O(k);
		dict.suffix_bytes += e.suffix_len;
		dict.max_suffix_len = MaxValue<uint32_t>(dict.max_suffix_len, e.suffix_len);
		dict.entries.push_back(e);
	};
	auto emit_group = [&](uint32_t a, uint32_t b, uint32_t plen) {
		const uint32_t prefix_id = NumericCast<uint32_t>(dict.prefixes.size());
		dict.prefixes.push_back({v.EP(a), plen});
		dict.prefix_bytes += plen;
		dict.max_prefix_len = MaxValue<uint32_t>(dict.max_prefix_len, plen);
		for (uint32_t k = a; k <= b; k++) {
			PlusEntry e;
			e.prefix_id = prefix_id;
			e.suffix = v.EP(k) + plen;
			e.suffix_len = v.EL(k) - plen;
			e.original_index = v.O(k);
			dict.suffix_bytes += e.suffix_len;
			dict.max_suffix_len = MaxValue<uint32_t>(dict.max_suffix_len, e.suffix_len);
			dict.entries.push_back(e);
		}
	};

	if (n >= 2) {
		const uint32_t m = NumericCast<uint32_t>(n - 1);
		const uint32_t root = CleaveDP(v, m, scratch, lcp, lcp_valid);
		CleaveWalk(scratch, lcp, m, root, prefix_cap, emit_plain, emit_group);
	} else if (n == 1) {
		emit_plain(0);
	}

	const uint32_t sentinel = dict.NoPrefixSentinel();
	for (auto &e : dict.entries) {
		if (e.prefix_id == 0xFFFFFFFFu) {
			e.prefix_id = sentinel;
		}
	}
}

//! The measuring twin of CleaveEncoded: same DP, same walk, counting callbacks, nothing materialized.
template <bool IDENTITY>
inline void CleaveMeasureImpl(CleaveStats &out, const vector<string_t> &encoded, idx_t n, const uint32_t *order,
                              CleaveScratch &scratch, vector<uint32_t> &lcp, idx_t lcp_valid) {
	const CleaveView<IDENTITY> v {encoded, order};

	auto count_plain = [&](uint32_t k) {
		const uint32_t l = v.EL(k);
		out.suffix_bytes += l;
		out.max_suffix_len = MaxValue<uint32_t>(out.max_suffix_len, l);
	};
	auto count_group = [&](uint32_t a, uint32_t b, uint32_t plen) {
		out.pc++;
		out.prefix_bytes += plen;
		out.max_prefix_len = MaxValue<uint32_t>(out.max_prefix_len, plen);
		for (uint32_t k = a; k <= b; k++) {
			const uint32_t sl = v.EL(k) - plen;
			out.suffix_bytes += sl;
			out.max_suffix_len = MaxValue<uint32_t>(out.max_suffix_len, sl);
		}
	};

	if (n >= 2) {
		const uint32_t m = NumericCast<uint32_t>(n - 1);
		const uint32_t root = CleaveDP(v, m, scratch, lcp, lcp_valid);
		CleaveWalk(scratch, lcp, m, root, NumericLimits<idx_t>::Maximum(), count_plain, count_group);
	} else if (n == 1) {
		count_plain(0);
	}
}

inline idx_t CleavedSizeFromStats(const CleaveStats &s, idx_t entry_n, idx_t tuple_count,
                                  bitpacking_width_t indices_width, idx_t symbol_table_size) {
	const idx_t dict_count = entry_n + 1;
	auto l = DictFSSTPlusLayout::Compute(tuple_count, dict_count, s.pc, indices_width,
	                                     BitpackingPrimitives::MinimumBitWidth(s.max_prefix_len),
	                                     BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(s.pc)),
	                                     BitpackingPrimitives::MinimumBitWidth(s.max_suffix_len), symbol_table_size,
	                                     s.prefix_bytes, s.suffix_bytes);
	return l.total;
}

struct NativeLayout {
	idx_t entry_n;
	idx_t dict_count;
	idx_t dict_bytes;
	idx_t symtab_size;
	bitpacking_width_t sl_width;
	bitpacking_width_t di_width;
	idx_t dict_dest;
	idx_t symtab_dest;
	idx_t sl_dest;
	idx_t di_dest;
	idx_t total;
};

NativeLayout ComputeNativeLayout(const Dictionary &dict, idx_t tuple_count, DictFSSTMode mode) {
	const bool encoded = mode != DictFSSTMode::DICTIONARY;
	const bool has_selection = mode != DictFSSTMode::FSST_ONLY;
	NativeLayout l;
	l.entry_n = encoded ? dict.encoded.size() : dict.raw.size();
	l.dict_count = l.entry_n + 1;
	l.dict_bytes = encoded ? dict.flat_encoded : dict.raw_bytes;
	l.symtab_size = encoded ? dict.symbol_table_size : 0;
	l.sl_width = BitpackingPrimitives::MinimumBitWidth(encoded ? dict.max_enc_len : dict.max_raw_len);
	l.di_width = has_selection ? BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(l.entry_n)) : 0;
	l.dict_dest = AlignValue<idx_t>(DictFSSTCompression::DICTIONARY_HEADER_SIZE);
	l.symtab_dest = AlignValue<idx_t>(l.dict_dest + l.dict_bytes);
	l.sl_dest = AlignValue<idx_t>(l.symtab_dest + l.symtab_size);
	l.di_dest = AlignValue<idx_t>(l.sl_dest + BitpackingPrimitives::GetRequiredSize(l.dict_count, l.sl_width));
	idx_t di_space = has_selection ? BitpackingPrimitives::GetRequiredSize(tuple_count, l.di_width) : 0;
	l.total = l.di_dest + di_space;
	return l;
}

idx_t WriteCleavedSegment(DictFSSTCompressionState &state, DictFSSTMode mode, const CleavedDictionary &dict,
                          const vector<uint32_t> &sel_new, const unsigned char *symbol_table, idx_t symbol_table_size) {
	idx_t entry_n = dict.entries.size();
	idx_t dict_count = entry_n + 1;
	idx_t tuple_count = state.tuple_count;
	idx_t prefix_count = dict.prefixes.size();

	const bool has_selection = mode != DictFSSTMode::FSST_PLUS;
	bitpacking_width_t indices_width =
	    has_selection ? BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n)) : 0;
	auto prefix_lengths_width = dict.PrefixLengthsWidth();
	auto prefix_id_width = dict.PrefixIdWidth();
	auto suffix_lengths_width = dict.SuffixLengthsWidth();

	auto layout = DictFSSTPlusLayout::Compute(tuple_count, dict_count, prefix_count, indices_width,
	                                          prefix_lengths_width, prefix_id_width, suffix_lengths_width,
	                                          symbol_table_size, dict.prefix_bytes, dict.suffix_bytes);
	D_ASSERT(layout.total <= state.info.GetBlockSize());

	auto base_ptr = state.handle.GetDataMutable();
	auto header = reinterpret_cast<dict_fsst_compression_header_t *>(base_ptr);
	header->dict_size = 0;
	header->dict_count = NumericCast<uint32_t>(dict_count);
	header->mode = mode;
	header->string_lengths_width = 0;
	header->dictionary_indices_width = indices_width;
	header->symbol_table_size = NumericCast<uint32_t>(symbol_table_size);

	auto ext = reinterpret_cast<dict_fsst_plus_header_t *>(base_ptr +
	                                                       AlignValue<idx_t>(sizeof(dict_fsst_compression_header_t)));
	ext->prefix_count = NumericCast<uint32_t>(prefix_count);
	ext->prefix_bytes_size = NumericCast<uint32_t>(dict.prefix_bytes);
	ext->suffix_bytes_size = NumericCast<uint32_t>(dict.suffix_bytes);
	ext->prefix_id_width = prefix_id_width;
	ext->prefix_lengths_width = prefix_lengths_width;
	ext->suffix_lengths_width = suffix_lengths_width;
	ext->padding = 0;

	if (has_selection && indices_width > 0 && tuple_count > 0) {
		BitpackingPrimitives::PackBuffer<sel_t, false>(base_ptr + layout.selection_dest,
		                                               const_cast<sel_t *>(sel_new.data()), tuple_count, indices_width);
	}
	if (symbol_table_size > 0) {
		memcpy(base_ptr + layout.symtab_dest, symbol_table, symbol_table_size);
	}
	if (prefix_count > 0) {
		auto &pl = state.serialize_scratch.pl;
		pl.resize(prefix_count);
		for (idx_t i = 0; i < prefix_count; i++) {
			pl[i] = dict.prefixes[i].len;
		}
		if (prefix_lengths_width > 0) {
			BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.prefix_lengths_dest, pl.data(),
			                                                  prefix_count, prefix_lengths_width);
		}
		idx_t poff = 0;
		for (idx_t i = 0; i < prefix_count; i++) {
			memcpy(base_ptr + layout.prefix_bytes_dest + poff, dict.prefixes[i].data, dict.prefixes[i].len);
			poff += dict.prefixes[i].len;
		}
	}
	{
		auto &pid = state.serialize_scratch.pid;
		auto &sl = state.serialize_scratch.sl;
		pid.resize(dict_count);
		sl.resize(dict_count);
		pid[0] = dict.NoPrefixSentinel();
		sl[0] = 0;
		for (idx_t j = 0; j < entry_n; j++) {
			pid[j + 1] = dict.entries[j].prefix_id;
			sl[j + 1] = dict.entries[j].suffix_len;
		}
		if (prefix_id_width > 0) {
			BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.prefix_ids_dest, pid.data(), dict_count,
			                                                  prefix_id_width);
		}
		if (suffix_lengths_width > 0) {
			BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.suffix_lengths_dest, sl.data(),
			                                                  dict_count, suffix_lengths_width);
		}
		idx_t soff = 0;
		for (idx_t j = 0; j < entry_n; j++) {
			memcpy(base_ptr + layout.suffix_bytes_dest + soff, dict.entries[j].suffix, dict.entries[j].suffix_len);
			soff += dict.entries[j].suffix_len;
		}
	}
	return layout.total;
}

void BuildSelNew(DictFSSTCompressionState &state, const CleavedDictionary &dict, idx_t entry_n) {
	auto &list_to_new = state.serialize_scratch.list_to_new;
	list_to_new.assign(entry_n, 0);
	for (uint32_t j = 0; j < dict.entries.size(); j++) {
		list_to_new[dict.entries[j].original_index] = j + 1;
	}
	idx_t tuple_count = state.tuple_count;
	auto &sel = state.serialize_scratch.sel;
	sel.resize(tuple_count);
	const auto &dictionary_indices = state.dictionary_indices;
	for (idx_t r = 0; r < tuple_count; r++) {
		auto di = dictionary_indices[r];
		sel[r] = di == 0 ? 0 : list_to_new[di - 1];
	}
}

} // namespace

Dictionary::~Dictionary() {
	if (encoder) {
		auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
		duckdb_fsst_destroy(fsst_encoder);
	}
}

void Dictionary::Clear() {
	raw_heap.Destroy();
	encoded_heap.Destroy();
	raw.clear();
	encoded.clear();
	sorted_order.clear();
	row_lcp.clear();
	row_lcp_valid = 0;
	dedup.clear();
	raw_bytes = 0;
	flat_encoded = 0;
	max_raw_len = 0;
	max_enc_len = 0;
	if (encoder) {
		duckdb_fsst_destroy(reinterpret_cast<duckdb_fsst_encoder_t *>(encoder));
		encoder = nullptr;
	}
	symbol_table_size = DConstants::INVALID_INDEX;
}

Dictionary::AddResult Dictionary::Add(const string_t &s) {
	uint32_t entry_index = NumericCast<uint32_t>(raw.size());
	auto res = dedup.try_emplace(s, entry_index);
	if (!res.second) {
		return {res.first->second, false};
	}
	auto &entry = const_cast<string_t &>(res.first->first) = raw_heap.AddBlob(s);
	raw.push_back(entry);
	auto raw_len = UnsafeNumericCast<uint32_t>(entry.GetSize());
	raw_bytes += raw_len;
	if (raw_len > max_raw_len) {
		max_raw_len = raw_len;
	}
	if (EncodedReady()) {
		EncodeOne(entry);
	}
	return {entry_index, true};
}

void Dictionary::PopLastEntry() {
	dedup.erase(raw.back());
	raw_bytes -= UnsafeNumericCast<idx_t>(raw.back().GetSize());
	raw.pop_back();
	if (EncodedReady()) {
		flat_encoded -= UnsafeNumericCast<idx_t>(encoded.back().GetSize());
		encoded.pop_back();
		row_lcp_valid = MinValue<idx_t>(row_lcp_valid, encoded.empty() ? 0 : encoded.size() - 1);
		if (sorted_order.size() > encoded.size()) {
			sorted_order.clear();
		}
	}
}

void Dictionary::EncodeAll() {
	idx_t n = raw.size();
	D_ASSERT(n > 0 && !EncodedReady());
	auto &sizes = encode_scratch.sizes;
	auto &ptrs = encode_scratch.ptrs;
	sizes.resize(n);
	ptrs.resize(n);
	idx_t total = 0;
	for (idx_t i = 0; i < n; i++) {
		sizes[i] = raw[i].GetSize();
		ptrs[i] = reinterpret_cast<unsigned char *>(const_cast<char *>(raw[i].GetData()));
		total += sizes[i];
	}
	encoder = reinterpret_cast<void *>(duckdb_fsst_create(n, sizes.data(), ptrs.data(), 0));
	auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
	if (!symbol_table) {
		symbol_table = make_unsafe_uniq_array_uninitialized<unsigned char>(sizeof(duckdb_fsst_decoder_t));
	}
	symbol_table_size = duckdb_fsst_export(fsst_encoder, symbol_table.get());

	size_t out_cap = 7 + 2 * total;
	if (out_cap > encode_buffer_size) {
		encode_buffer = make_unsafe_uniq_array_uninitialized<unsigned char>(out_cap);
		encode_buffer_size = out_cap;
	}
	auto &out_sizes = encode_scratch.out_sizes;
	auto &out_ptrs = encode_scratch.out_ptrs;
	out_sizes.assign(n, 0);
	out_ptrs.assign(n, nullptr);
	auto res = duckdb_fsst_compress(fsst_encoder, n, sizes.data(), ptrs.data(), encode_buffer_size, encode_buffer.get(),
	                                out_sizes.data(), out_ptrs.data());
	if (res != n) {
		throw FatalException("dict_fsst plus: failed to FSST-encode the dictionary");
	}
	StoreEncoded(out_sizes.data(), out_ptrs.data(), n);
}

void Dictionary::EncodeOne(const string_t &raw) {
	D_ASSERT(EncodedReady());
	size_t in_size = raw.GetSize();
	unsigned char *in_ptr = reinterpret_cast<unsigned char *>(const_cast<char *>(raw.GetData()));
	size_t out_cap = 7 + 2 * in_size;
	if (out_cap > encode_buffer_size) {
		encode_buffer = make_unsafe_uniq_array_uninitialized<unsigned char>(out_cap);
		encode_buffer_size = out_cap;
	}
	size_t out_size = 0;
	unsigned char *out_ptr = nullptr;
	auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
	auto res = duckdb_fsst_compress(fsst_encoder, 1, &in_size, &in_ptr, encode_buffer_size, encode_buffer.get(),
	                                &out_size, &out_ptr);
	if (res != 1) {
		throw FatalException("dict_fsst plus: failed to FSST-encode a dictionary entry");
	}
	StoreEncoded(&out_size, &out_ptr, 1);
}

void Dictionary::StoreEncoded(const size_t *out_sizes, unsigned char *const *out_ptrs, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		auto enc_len = UnsafeNumericCast<uint32_t>(out_sizes[i]);
		encoded.push_back(encoded_heap.AddBlob(string_t(reinterpret_cast<const char *>(out_ptrs[i]), enc_len)));
		flat_encoded += enc_len;
		if (enc_len > max_enc_len) {
			max_enc_len = enc_len;
		}
	}
}

void Dictionary::SyncSortedOrder() {
	idx_t sorted = sorted_order.size();
	idx_t n = encoded.size();
	if (sorted == n) {
		return;
	}
	auto less = [&](uint32_t a, uint32_t b) {
		return encoded[a] < encoded[b];
	};
	sorted_order.reserve(n);
	for (uint32_t i = NumericCast<uint32_t>(sorted); i < n; i++) {
		sorted_order.push_back(i);
	}
	std::sort(sorted_order.begin() + sorted, sorted_order.end(), less);
	merge_result.resize(n);
	std::merge(sorted_order.begin(), sorted_order.begin() + sorted, sorted_order.begin() + sorted, sorted_order.end(),
	           merge_result.begin(), less);
	sorted_order.swap(merge_result);
}

void Dictionary::Cleave(CleavedDictionary &out, const uint32_t *order, idx_t prefix_cap) {
	idx_t n = encoded.size();
	out.Reset();
	out.symbol_table_size = symbol_table_size;
	if (order) {
		CleaveEncoded<false>(out, encoded, n, order, cleave_scratch, cleave_scratch.lcp, 0, prefix_cap);
	} else {
		CleaveEncoded<true>(out, encoded, n, nullptr, cleave_scratch, row_lcp, row_lcp_valid, prefix_cap);
		row_lcp_valid = n >= 2 ? n - 1 : 0;
	}
}

void Dictionary::CleaveMeasure(const uint32_t *order, CleaveStats &out) {
	idx_t n = encoded.size();
	out = CleaveStats();
	if (order) {
		CleaveMeasureImpl<false>(out, encoded, n, order, cleave_scratch, cleave_scratch.lcp, 0);
	} else {
		CleaveMeasureImpl<true>(out, encoded, n, nullptr, cleave_scratch, row_lcp, row_lcp_valid);
		row_lcp_valid = n >= 2 ? n - 1 : 0;
	}
}

DictFSSTCompressionState::DictFSSTCompressionState(ColumnDataCheckpointData &checkpoint_data_p,
                                                   unique_ptr<DictFSSTAnalyzeState> &&analyze_p)
    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_DICT_FSST), stats_writer(GetType()),
      analyze(std::move(analyze_p)) {
	const auto &mode = Settings::Get<ForceDictFsstModeSetting>(checkpoint_data_p.GetDatabase());
	if (mode == "DEFAULT") {
		forced_mode = DictFSSTMode::COUNT;
		allow_plus = IsSereneDBStorageVersion(checkpoint_data_p.GetStorageVersion());
	} else if (mode == "AUTO") {
		forced_mode = DictFSSTMode::COUNT;
		allow_plus = true;
	} else if (mode == "AUTO_NATIVE") {
		forced_mode = DictFSSTMode::COUNT;
		allow_plus = false;
	} else {
		forced_mode = EnumUtil::FromString<DictFSSTMode>(mode);
		allow_plus = IsPlusMode(forced_mode);
	}
	committed = allow_plus ? CutCommit::UNDECIDED : CutCommit::PLAIN;
	CreateEmptySegment();
}

void DictFSSTCompressionState::CreateEmptySegment() {
	CreateAndPinNewSegment();
	D_ASSERT(dictionary_indices.empty());
	tuple_count = 0;
	dict.symbol_table_size = DConstants::INVALID_INDEX;
}

void DictFSSTCompressionState::Flush(bool final, bool use_cached_cleave) {
	if (!tuple_count) {
		return;
	}
	current_segment->count = tuple_count;
	auto segment_size = FinalizeSegment(use_cached_cleave);
	FlushCurrentSegment(stats_writer, segment_size);
	total_tuple_count += tuple_count;

	ResetSegment();
	tuple_count = 0;
	if (!final) {
		CreateAndPinNewSegment();
	}
}

void DictFSSTCompressionState::ResetSegment() {
	dict.Clear();
	dictionary_indices.clear();
	cl_dict_bytes = 0;
	cl_prefix_count = 0;
	cut_stats = CleaveStats();
	flat_at_cleave = 0;
	sel_at_cleave = 0;
	enc_width_at_cleave = 0;
	null_count = 0;
	rows_since_new = 0;
	committed = allow_plus ? CutCommit::UNDECIDED : CutCommit::PLAIN;
	fit_rows = 0;
	fit_raw_count = 0;
	fit_commit = committed;
	cut_mode = DictFSSTMode::COUNT;
	cut_dict.Reset();
	cut_dict_row.Reset();
}

void DictFSSTCompressionState::RowModeBroken() {
	if (committed == CutCommit::PLUS_ROW) {
		committed = CutCommit::UNDECIDED;
	}
}

bool DictFSSTCompressionState::AddValue(const string_t &s, bool is_null) {
	if (is_null) {
		dictionary_indices.push_back(0);
		null_count++;
		rows_since_new++;
		stats_writer.SetHasNull();
		tuple_count++;
		RowModeBroken();
		return false;
	}
	auto result = dict.Add(s);
	dictionary_indices.push_back(result.index + 1);
	rows_since_new = result.was_new ? 0 : rows_since_new + 1;
	stats_writer.Update(s);
	tuple_count++;
	if (!result.was_new) {
		RowModeBroken();
	}
	return result.was_new;
}

void DictFSSTCompressionState::PopRow(bool was_new) {
	bool was_null = dictionary_indices.back() == 0;
	dictionary_indices.pop_back();
	tuple_count--;
	if (was_null) {
		null_count--;
	} else if (was_new) {
		dict.PopLastEntry();
	}
}

idx_t DictFSSTCompressionState::RefreshCleave() {
	idx_t n = dict.encoded.size();
	const bool all_unique = null_count == 0 && n == tuple_count;
	if (committed == CutCommit::PLUS_ROW && !all_unique) {
		committed = CutCommit::UNDECIDED;
	}
	const bool do_sorted = committed == CutCommit::UNDECIDED || committed == CutCommit::PLUS_SORTED;
	const bool do_row = all_unique && (committed == CutCommit::UNDECIDED || committed == CutCommit::PLUS_ROW);
	auto indices_width = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(n));
	idx_t sorted_size = DConstants::INVALID_INDEX;
	idx_t row_size = DConstants::INVALID_INDEX;
	CleaveStats s_sorted;
	CleaveStats s_row;
	if (do_sorted) {
		dict.SyncSortedOrder();
		dict.CleaveMeasure(dict.sorted_order.data(), s_sorted);
		sorted_size = CleavedSizeFromStats(s_sorted, n, tuple_count, indices_width, dict.symbol_table_size);
	}
	if (do_row) {
		dict.CleaveMeasure(nullptr, s_row);
		row_size = CleavedSizeFromStats(s_row, n, tuple_count, 0, dict.symbol_table_size);
	}
	idx_t chosen_size;
	if (forced_mode == DictFSSTMode::DICT_FSST_PLUS || committed == CutCommit::PLUS_SORTED) {
		cut_mode = DictFSSTMode::DICT_FSST_PLUS;
		chosen_size = sorted_size;
	} else if ((forced_mode == DictFSSTMode::FSST_PLUS || committed == CutCommit::PLUS_ROW) &&
	           row_size != DConstants::INVALID_INDEX) {
		cut_mode = DictFSSTMode::FSST_PLUS;
		chosen_size = row_size;
	} else {
		chosen_size = ChooseMode(sorted_size, row_size, cut_mode);
	}
	cut_stats = cut_mode == DictFSSTMode::FSST_PLUS ? s_row : s_sorted;
	cl_dict_bytes = cut_stats.prefix_bytes + cut_stats.suffix_bytes;
	cl_prefix_count = cut_stats.pc;
	//! The flush emission runs at this budget: at least a doubling above what either candidate's DP wants, so the
	//! emit-side clamp cannot bind and the written layout is exactly the measured one. It is also the count
	//! CleavedUpperBound prices the prefix-id width at until the next cleave.
	const idx_t want = MaxValue(s_sorted.pc, s_row.pc);
	prefix_cap = MinValue(MaxValue<idx_t>(MIN_PREFIX_CAP, 2 * want + 1), MaxValue<idx_t>(n / 2, 1));
	flat_at_cleave = dict.flat_encoded;
	sel_at_cleave = CurSelBytes();
	enc_width_at_cleave = BitpackingPrimitives::MinimumBitWidth(dict.max_enc_len);
	return chosen_size;
}

idx_t DictFSSTCompressionState::CachedCutSize() const {
	const idx_t n = dict.encoded.size();
	if (cut_mode == DictFSSTMode::DICT_FSST_PLUS) {
		auto indices_width = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(n));
		return CleavedSizeFromStats(cut_stats, n, tuple_count, indices_width, dict.symbol_table_size);
	}
	if (cut_mode == DictFSSTMode::FSST_PLUS) {
		return CleavedSizeFromStats(cut_stats, n, tuple_count, 0, dict.symbol_table_size);
	}
	return NativeSize(cut_mode);
}

idx_t DictFSSTCompressionState::NativeSize(DictFSSTMode mode) const {
	return ComputeNativeLayout(dict, tuple_count, mode).total;
}

idx_t DictFSSTCompressionState::CurSelBytes() const {
	auto di_width = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(dict.encoded.size()));
	return BitpackingPrimitives::GetRequiredSize(tuple_count, di_width);
}

idx_t DictFSSTCompressionState::CleavedUpperBound() const {
	const idx_t entry_n = dict.encoded.size();
	const idx_t dict_count = entry_n + 1;
	//! The prefix-id width is priced at the budget the emission is held to. The DP's group count cannot be
	//! predicted between cleaves (one entry can restructure the LCP tree anywhere), but every write is measured
	//! before it happens, so a pathological jump past the budget costs a rewind, never an overflow.
	const idx_t pc_ub = MinValue(MaxValue<idx_t>(prefix_cap, 1), MaxValue<idx_t>(entry_n / 2, 1));
	const auto pid_w = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(pc_ub));
	const auto len_w = BitpackingPrimitives::MinimumBitWidth(dict.max_enc_len);
	const idx_t meta_ub = BitpackingPrimitives::GetRequiredSize(dict_count, pid_w) +
	                      BitpackingPrimitives::GetRequiredSize(dict_count, len_w);
	//! Dictionary terms. The anchored layout stays feasible as entries arrive: for sorted x < e < y,
	//! lcp(x, y) = min(lcp(x, e), lcp(e, y)), so a newcomer inside a group's range shares that group's prefix and
	//! the old grouping absorbs it (the row cleave only ever appends). The meta-priced DP nets at least as much as
	//! any feasible grouping, so the anchor's groups are charged GROUP_COST each in place of a separate
	//! prefix-lengths term -- the only per-group field, which GROUP_COST covers by construction.
	const idx_t dict_ub = cl_dict_bytes + GROUP_COST * cl_prefix_count + (dict.flat_encoded - flat_at_cleave);
	const bool all_unique = null_count == 0 && entry_n == tuple_count;
	const idx_t sel_ub = all_unique && committed == CutCommit::PLUS_ROW ? 0 : CurSelBytes();
	return DictFSSTCompression::PLUS_HEADER_SIZE + dict.symbol_table_size + sel_ub + dict_ub + meta_ub + 6 * 7;
}

DictFSSTMode DictFSSTCompressionState::EffectiveForcedNativeMode() const {
	if (forced_mode == DictFSSTMode::FSST_ONLY && (null_count != 0 || dict.raw.size() != tuple_count)) {
		return DictFSSTMode::DICT_FSST;
	}
	return forced_mode;
}

idx_t DictFSSTCompressionState::PlainOnlySize(DictFSSTMode &out_mode) const {
	if (IsNativeMode(forced_mode)) {
		out_mode = EffectiveForcedNativeMode();
		return NativeSize(out_mode);
	}
	idx_t entry_n = dict.encoded.size();
	bool all_unique = null_count == 0 && entry_n == tuple_count;
	idx_t s_plain = NativeSize(DictFSSTMode::DICT_FSST);
	idx_t s_fsst_only = all_unique ? NativeSize(DictFSSTMode::FSST_ONLY) : DConstants::INVALID_INDEX;
	out_mode = s_fsst_only <= s_plain ? DictFSSTMode::FSST_ONLY : DictFSSTMode::DICT_FSST;
	return MinValue<idx_t>(s_plain, s_fsst_only);
}

idx_t DictFSSTCompressionState::ChooseMode(idx_t sorted_plus_size, idx_t row_plus_size, DictFSSTMode &out_mode) const {
	DictFSSTMode plain_mode;
	idx_t plain_size = PlainOnlySize(plain_mode);
	idx_t best = MinValue<idx_t>(plain_size, MinValue<idx_t>(sorted_plus_size, row_plus_size));
	if (plain_size <= best + MODE_TIE_TOL) {
		out_mode = plain_mode;
		return plain_size;
	}
	if (row_plus_size <= best + MODE_TIE_TOL) {
		out_mode = DictFSSTMode::FSST_PLUS;
		return row_plus_size;
	}
	out_mode = DictFSSTMode::DICT_FSST_PLUS;
	return sorted_plus_size;
}

idx_t DictFSSTCompressionState::WriteNative(DictFSSTMode mode) {
	const bool encoded = mode != DictFSSTMode::DICTIONARY;
	const bool has_selection = mode != DictFSSTMode::FSST_ONLY;
	const vector<string_t> &src = encoded ? dict.encoded : dict.raw;
	auto layout = ComputeNativeLayout(dict, tuple_count, mode);
	D_ASSERT(layout.total <= info.GetBlockSize());

	auto base_ptr = handle.GetDataMutable();
	auto header = reinterpret_cast<dict_fsst_compression_header_t *>(base_ptr);
	header->mode = mode;
	header->dict_size = NumericCast<uint32_t>(layout.dict_bytes);
	header->dict_count = NumericCast<uint32_t>(layout.dict_count);
	header->string_lengths_width = layout.sl_width;
	header->dictionary_indices_width = layout.di_width;
	header->symbol_table_size = NumericCast<uint32_t>(layout.symtab_size);

	auto &sl = serialize_scratch.sl;
	sl.resize(layout.dict_count);
	sl[0] = 0;
	idx_t off = 0;
	for (idx_t j = 0; j < layout.entry_n; j++) {
		auto l = UnsafeNumericCast<uint32_t>(src[j].GetSize());
		memcpy(base_ptr + layout.dict_dest + off, src[j].GetData(), l);
		sl[j + 1] = l;
		off += l;
	}
	if (encoded) {
		memcpy(base_ptr + layout.symtab_dest, dict.symbol_table.get(), dict.symbol_table_size);
	}
	BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.sl_dest, sl.data(), layout.dict_count,
	                                                  layout.sl_width);
	if (has_selection) {
		BitpackingPrimitives::PackBuffer<sel_t, false>(base_ptr + layout.di_dest, dictionary_indices.data(),
		                                               tuple_count, layout.di_width);
	}
	return layout.total;
}

idx_t DictFSSTCompressionState::FinalizeSegment(bool use_cached) {
	idx_t entry_n = dict.raw.size();
	if (IsNativeMode(forced_mode)) {
		if (entry_n == 0) {
			return WriteNative(DictFSSTMode::DICTIONARY);
		}
		DictFSSTMode m = EffectiveForcedNativeMode();
		if (m != DictFSSTMode::DICTIONARY && !dict.EncodedReady()) {
			dict.EncodeAll();
		}
		return WriteNative(m);
	}
	if (entry_n == 0 || !dict.EncodedReady()) {
		return WriteNative(DictFSSTMode::DICTIONARY);
	}
	auto symbol_table = dict.symbol_table.get();

	if (committed == CutCommit::PLAIN && forced_mode == DictFSSTMode::COUNT) {
		DictFSSTMode plain_mode;
		PlainOnlySize(plain_mode);
		return WriteNative(plain_mode);
	}

	//! Forced plus modes go through the same measure-then-materialize path as auto: RefreshCleave honours
	//! forced_mode when picking cut_mode, and this is what keeps every written layout one a measuring pass saw.
	if (!use_cached || forced_mode != DictFSSTMode::COUNT) {
		RefreshCleave();
	}
	if (cut_mode == DictFSSTMode::DICT_FSST_PLUS) {
		dict.SyncSortedOrder();
		dict.Cleave(cut_dict, dict.sorted_order.data(), prefix_cap);
		BuildSelNew(*this, cut_dict, entry_n);
		return WriteCleavedSegment(*this, cut_mode, cut_dict, serialize_scratch.sel, symbol_table,
		                           dict.symbol_table_size);
	}
	if (cut_mode == DictFSSTMode::FSST_PLUS) {
		dict.Cleave(cut_dict_row, nullptr, prefix_cap);
		return WriteCleavedSegment(*this, cut_mode, cut_dict_row, {}, symbol_table, dict.symbol_table_size);
	}
	return WriteNative(cut_mode);
}

void DictFSSTCompressionState::FlushRewind() {
	D_ASSERT(fit_rows < tuple_count && fit_raw_count <= dict.raw.size());
	idx_t moved_count = tuple_count - fit_rows;
	auto *const null_marker = reinterpret_cast<char *>(~uintptr_t(0));
	vector<string_t> moved;
	moved.reserve(moved_count);
	for (idx_t k = 0; k < moved_count; k++) {
		idx_t di = dictionary_indices[fit_rows + k];
		if (di != 0) {
			moved.push_back(dict.raw[di - 1]);
		} else {
			string_t null_row(string_t::INLINE_LENGTH + 1);
			null_row.SetPointer(null_marker);
			moved.push_back(null_row);
		}
	}
	for (idx_t r = fit_rows; r < tuple_count; r++) {
		if (dictionary_indices[r] == 0) {
			null_count--;
		}
	}
	dictionary_indices.resize(fit_rows);
	while (dict.raw.size() > fit_raw_count) {
		dict.PopLastEntry();
	}
	tuple_count = fit_rows;
	//! fit_rows certifies a size for the CANDIDATE it was measured under, so restore that candidate with it. The
	//! rewind rebuilds the exact state the certificate was taken in -- including, when the flip row is among the moved
	//! rows, all-unique itself -- but the commit in force may have been poisoned since (the flip kills the row
	//! candidate), and flushing under it writes a layout the certificate never priced: sorted needs a selection buffer
	//! the row cut did not pay for. Restoring fit_commit makes the flush write back exactly what was measured, rather
	//! than relying on the re-measurement to rediscover it.
	committed = fit_commit;
	StringHeap saved;
	saved.Move(dict.raw_heap);
	Flush(false);
	for (idx_t k = 0; k < moved_count; k++) {
		AddValue(moved[k], moved[k].GetData() == null_marker);
	}
}

bool DictFSSTCompressionState::AddScanRow(UnifiedVectorFormat &vf, const string_t *strings, idx_t j) {
	auto jdx = vf.sel->get_index(j);
	bool is_null = !vf.validity.RowIsValid(jdx);
	return AddValue(is_null ? string_t() : strings[jdx], is_null);
}

void DictFSSTCompressionState::MaybeEncodeOrCutSmall(UnifiedVectorFormat &vf, const string_t *strings, idx_t i,
                                                     bool was_new, idx_t block_size) {
	if (forced_mode != DictFSSTMode::DICTIONARY && dict.raw_bytes >= ENCODE_THRESHOLD) {
		if (NativeSize(DictFSSTMode::DICTIONARY) + ENCODE_HEADROOM >= block_size ||
		    rows_since_new >= DICT_STABLE_ROWS) {
			dict.EncodeAll();
			//! Only certify a state the cleave just proved fits. This path fires when the native size is already
			//! within ENCODE_HEADROOM of the block, so the cleave can come back over it, and an uncertified fit_rows
			//! is a rewind target nothing ever priced -- the same defect FlushRewind's restore guards against.
			if (RefreshCleave() + dict.max_enc_len + ROW_HEADROOM < block_size) {
				fit_rows = tuple_count;
				fit_raw_count = dict.raw.size();
				fit_commit = committed;
			}
		}
		return;
	}
	//! A never-encoded segment can be half a million rows wide at a few bits of selection each, and one NEW entry
	//! that crosses a bitpacking width boundary re-prices every one of those rows at once -- the same
	//! whole-segment re-price NearBlock watches for on the encoded path, and far larger than ROW_HEADROOM. The
	//! byte trigger below only sees it after the fact, so handle the jump exactly like CutPlain's overshoot:
	//! exclude the bumping row, flush the pre-bump segment (which passed the trigger and so fits), re-add.
	if (was_new && tuple_count > 1) {
		const idx_t entry_n = dict.raw.size();
		const bool sel_width_bumped = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n)) !=
		                              BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n - 1));
		if (sel_width_bumped && NativeSize(DictFSSTMode::DICTIONARY) + dict.max_raw_len + ROW_HEADROOM >= block_size) {
			PopRow(true);
			Flush(false);
			AddScanRow(vf, strings, i);
			return;
		}
	}
	if (NativeSize(DictFSSTMode::DICTIONARY) + dict.max_raw_len + ROW_HEADROOM >= block_size) {
		Flush(false);
	}
}

bool DictFSSTCompressionState::NearBlock(bool was_new) const {
	idx_t entry_n = dict.encoded.size();
	bool sel_width_bumped = was_new && entry_n >= 2 &&
	                        BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n)) !=
	                            BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n - 1));
	//! The string-lengths field is dict_count wide entries, so one longer entry re-prices all of them.
	bool enc_width_bumped = BitpackingPrimitives::MinimumBitWidth(dict.max_enc_len) != enc_width_at_cleave;
	//! Exactly one row can end all-unique: the first that adds no entry (duplicate or null), leaving one more tuple
	//! than entries. That loses FSST_ONLY, which costs a selection buffer for every row already in the segment.
	bool unique_broken = !was_new && entry_n + 1 == tuple_count;
	idx_t grown = (dict.flat_encoded - flat_at_cleave) + (CurSelBytes() - sel_at_cleave);
	return grown >= CLEAVE_GAP || sel_width_bumped || enc_width_bumped || unique_broken;
}

void DictFSSTCompressionState::CutPlain(UnifiedVectorFormat &vf, const string_t *strings, idx_t i, bool was_new,
                                        idx_t margin, idx_t block_size) {
	DictFSSTMode plain_mode;
	idx_t plain_c = PlainOnlySize(plain_mode);
	if (plain_c + margin < block_size) {
		flat_at_cleave = dict.flat_encoded;
		sel_at_cleave = CurSelBytes();
		enc_width_at_cleave = BitpackingPrimitives::MinimumBitWidth(dict.max_enc_len);
		return;
	}
	if (plain_c <= block_size || tuple_count == 1) {
		Flush(false);
		return;
	}
	PopRow(was_new);
	Flush(false);
	AddScanRow(vf, strings, i);
}

void DictFSSTCompressionState::CutCleaved(UnifiedVectorFormat &vf, const string_t *strings, idx_t i, bool was_new,
                                          idx_t margin, idx_t block_size) {
	//! A PLUS_ROW segment that loses all-unique revives the sorted candidate, and the dict-bytes
	//! baseline under CleavedUpperBound was anchored to the row candidate -- row savings do not
	//! provably bound sorted savings, so the bound cannot be trusted for the revived candidate.
	//! Cleave for real instead: RefreshCleave resets `committed` to UNDECIDED and re-anchors the
	//! baseline from the freshly chosen candidate. NearBlock fires on exactly this flip
	//! (unique_broken), so this is reached on the row that breaks uniqueness.
	const bool row_commit_broken =
	    committed == CutCommit::PLUS_ROW && (null_count != 0 || dict.encoded.size() != tuple_count);
	if (!row_commit_broken && CleavedUpperBound() + margin < block_size) {
		return;
	}
	idx_t true_c = RefreshCleave();
	if (committed == CutCommit::UNDECIDED) {
		if (!IsPlusMode(cut_mode)) {
			committed = CutCommit::PLAIN;
		} else {
			committed = cut_mode == DictFSSTMode::DICT_FSST_PLUS ? CutCommit::PLUS_SORTED : CutCommit::PLUS_ROW;
		}
	}
	if (true_c + margin < block_size) {
		fit_rows = tuple_count;
		fit_raw_count = dict.raw.size();
		fit_commit = committed;
		return;
	}
	if (true_c <= block_size || tuple_count == 1) {
		Flush(false, true);
		return;
	}
	PopRow(was_new);
	idx_t excl_c = was_new ? RefreshCleave() : CachedCutSize();
	if (excl_c <= block_size) {
		Flush(false, true);
		AddScanRow(vf, strings, i);
	} else {
		AddScanRow(vf, strings, i);
		FlushRewind();
	}
}

void DictFSSTCompressionState::Compress(const Vector &scan_vector) {
	UnifiedVectorFormat vector_format;
	scan_vector.ToUnifiedFormat(vector_format);
	auto strings = UnifiedVectorFormat::GetData<string_t>(vector_format);
	const auto count = scan_vector.size();
	const idx_t block_size = info.GetBlockSize();

	for (idx_t i = 0; i < count; i++) {
		const bool was_new = AddScanRow(vector_format, strings, i);

		if (!dict.EncodedReady()) {
			MaybeEncodeOrCutSmall(vector_format, strings, i, was_new, block_size);
			continue;
		}
		if (!NearBlock(was_new)) {
			continue;
		}
		idx_t margin = dict.max_enc_len + ROW_HEADROOM;
		if (committed == CutCommit::PLAIN) {
			CutPlain(vector_format, strings, i, was_new, margin, block_size);
		} else {
			CutCleaved(vector_format, strings, i, was_new, margin, block_size);
		}
	}
}

void DictFSSTCompressionState::FinalizeCompress() {
	//! The final segment is the one write no cut guarded, so measure it for real before flushing. If it no longer
	//! fits -- the between-cleave estimate is a cadence heuristic, not a guarantee -- rewind to the last certified
	//! state and move the excess on, exactly like an ordinary cut.
	const idx_t block_size = info.GetBlockSize();
	while (tuple_count && dict.EncodedReady() && !IsNativeMode(forced_mode) && committed != CutCommit::PLAIN &&
	       fit_rows && fit_rows < tuple_count && RefreshCleave() > block_size) {
		FlushRewind();
	}
	Flush(true);
}

} // namespace dict_fsst
} // namespace duckdb
