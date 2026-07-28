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

#if defined(__MVS__) && !defined(alloca)
#define alloca __builtin_alloca
#endif

namespace duckdb {
namespace dict_fsst {
namespace {

// The whole segment is accumulated in memory: a deduplicated entry list (raw values, then their FSST encoding)
// plus a row->entry selection. A segment is cut against the size of the mode it will be written as; for the
// cleaved modes (DICT_FSST_PLUS / FSST_PLUS) that is the prefix-factored size, so enough rows are packed that the
// block fills UNDER the cleaved encoding (the flat encoding of those rows is deliberately allowed to exceed the
// block). At flush the entries are cleaved once (reusing the single FSST symbol table, no re-encode) and written.

// Below this many raw dictionary bytes FSST tends to expand, so a small segment is kept as a plain DICTIONARY.
constexpr idx_t ENCODE_THRESHOLD = 4096;
// Slack left below the block on top of the adaptive (one-entry) part of the cut margin: covers the growth between two
// gap-spaced cleaves (CLEAVE_GAP) plus metadata-width bumps, so the segment usually still fits WITH the current
// row (the common flush-with path) rather than needing the exclude-row-i fallback.
constexpr idx_t ROW_HEADROOM = 4 * 1024;
// The raw dictionary is FSST-encoded once it reaches within this much of a block. Encoding is decoupled from the
// block-fill cut on purpose: it happens while there is still this much room, so (a) the symbol table (which encoding
// adds) always fits and (b) the table is built from a large, representative dictionary sample. After encoding, the
// cleaved cut fills the rest of the block, so this headroom does NOT become final slack.
constexpr idx_t ENCODE_HEADROOM = 16 * 1024;
// If the raw dictionary has not gained a new entry in this many rows it is treated as complete and FSST-encoded even
// though the segment is not near a block yet -- so a low-cardinality (selection-dominated) column still gets encoded
// within a single row group instead of falling back to a raw DICTIONARY.
constexpr idx_t DICT_STABLE_ROWS = 4096;
// Near the block a real cleave runs at most every this many bytes of growth (keeps cleaves sparse -> fast), EXCEPT a
// row that widens the selection bitpacking (entry count crossed a power of two) always forces one, so the exclude-
// row-i fallback undoes that whole-selection jump.
constexpr idx_t CLEAVE_GAP = 2 * 1024;
// When a plain (read-faster) layout is within this many bytes of the sorted-plus size, prefer it. Kept small so the
// mode chosen by the cut and by finalize cannot differ by enough to overflow the block (< the cut margin).
constexpr idx_t MODE_TIE_TOL = 2 * 1024;

//! Longest common byte prefix of two FSST-encoded spans, never ending inside an FSST escape (255). Unbounded.
inline idx_t GuardedLcp(const unsigned char *a, idx_t la, const unsigned char *b, idx_t lb) {
	// Reuse abseil's word-at-a-time longest-common-prefix (SIMD-friendly, tail handled by one overlapping load).
	idx_t l = absl::FindLongestCommonPrefix(absl::string_view(reinterpret_cast<const char *>(a), la),
	                                        absl::string_view(reinterpret_cast<const char *>(b), lb))
	              .size();
	if (l != 0 && a[l - 1] == 255) {
		l--; // never split an FSST escape
	}
	return l;
}

//! Cleave the `n` ALREADY-FSST-ENCODED entries into shared-prefix chunks -- no FSST re-encoding; the spans are the
//! bytes dict_fsst already produced. `order` is the entry permutation (sorted, for DICT_FSST_PLUS); a null `order` is
//! the identity/row order (FSST_PLUS), which the IDENTITY specialization compiles down with no materialized vector.
//!
//! Optimal flat prefix factoring via the adjacent-LCP Cartesian tree. A chunk shares its minimum adjacent LCP as a
//! prefix (stored once) -- valid for any order, since "consecutive entries share >= P" transitively means the whole
//! run shares its first P bytes. The size-minimizing partition maximizes total savings sum (count-1)*prefix_len, and
//! is computed by "take the whole range at its min-LCP, or split at the min and recurse". O(n) via the leftmost-min
//! Cartesian tree of the LCP array, built with a monotonic stack (post-order DP + pre-order emit): window-free (chunks
//! of any size) and with no prefix-length cap.
template <bool IDENTITY>
inline void CleaveEncoded(CleavedDictionary &dict, const vector<string_t> &encoded, idx_t n, const uint32_t *order,
                          CleaveScratch &scratch) {
	auto O = [&](uint32_t k) -> uint32_t {
		if constexpr (IDENTITY) {
			return k;
		} else {
			return order[k];
		}
	};
	// Read entry k's encoded span straight from its string_t -- valid because `encoded` does not change within a
	// cleave, so there are no denormalized pointer/length arrays to keep in sync. GetData resolves the inline-vs-heap
	// storage (short entries live inside the string_t).
	auto EP = [&](uint32_t k) -> const unsigned char * {
		return reinterpret_cast<const unsigned char *>(encoded[O(k)].GetData());
	};
	auto EL = [&](uint32_t k) -> uint32_t {
		return encoded[O(k)].GetSize();
	};
	dict.entries.reserve(n);

	auto emit_plain = [&](uint32_t k) {
		PlusEntry e;
		e.prefix_id = 0xFFFFFFFFu; // temporary "no prefix" marker, fixed to the sentinel below
		e.suffix = EP(k);
		e.suffix_len = EL(k);
		e.original_index = O(k);
		dict.suffix_bytes += e.suffix_len;
		dict.max_suffix_len = MaxValue<uint32_t>(dict.max_suffix_len, e.suffix_len);
		dict.entries.push_back(e);
	};

	if (n >= 2) {
		const uint32_t m = NumericCast<uint32_t>(n - 1); // adjacent-LCP count (n fits uint32_t: entries << 4B)
		static constexpr uint32_t NONE = 0xFFFFFFFFu;
		auto &lcp = scratch.lcp;
		lcp.resize(m); // every index is unconditionally written by the loop below
		for (uint32_t i = 0; i < m; i++) {
			lcp[i] = NumericCast<uint32_t>(GuardedLcp(EP(i), EL(i), EP(i + 1), EL(i + 1)));
		}
		// Leftmost-min Cartesian tree of lcp[0..m-1] via a monotonic stack: node i is an ancestor of node j iff
		// (lcp[i], i) < (lcp[j], j), so the root of any range is its leftmost minimum. Popping only strictly-greater
		// tops keeps an equal earlier index as the ancestor -- matching the old leftmost range-min exactly.
		auto &lc = scratch.lc;
		auto &rc = scratch.rc;
		lc.resize(m);              // every index is unconditionally written below (lc[i] = ...) each iteration
		rc.assign(m, NONE);        // only conditionally written -- unset slots must stay NONE ("no right child")
		auto &mono = scratch.mono; // right spine of lcp indices
		mono.clear();
		mono.reserve(m);
		uint32_t root = 0;
		for (uint32_t i = 0; i < m; i++) {
			uint32_t last = NONE;
			while (!mono.empty() && lcp[mono.back()] > lcp[i]) {
				last = mono.back();
				mono.pop_back();
			}
			lc[i] = last; // the popped chain becomes i's left subtree
			if (mono.empty()) {
				root = i;
			} else {
				rc[mono.back()] = i; // i is the right child of the surviving top
			}
			mono.push_back(i);
		}

		// Post-order DP. sz[i] = lcp-nodes in i's subtree == (entries in i's range) - 1; value[i] = max prefix savings;
		// take_whole[i] = share the whole range's min LCP rather than split at it.
		auto &value = scratch.value;
		auto &sz = scratch.sz;
		auto &take_whole = scratch.take_whole;
		value.resize(m); // every index is unconditionally written below (one of the two branches always sets it)
		sz.resize(m);    // every index is unconditionally written below
		take_whole.assign(m, 0); // only conditionally written -- the else branch leaves "false"
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
				if (lc[i] != NONE) {
					dstk.push_back({lc[i], false});
				}
				if (rc[i] != NONE) {
					dstk.push_back({rc[i], false});
				}
			} else {
				const idx_t vl = (lc[i] != NONE) ? value[lc[i]] : 0;
				const idx_t vr = (rc[i] != NONE) ? value[rc[i]] : 0;
				const uint32_t szl = (lc[i] != NONE) ? sz[lc[i]] : 0;
				const uint32_t szr = (rc[i] != NONE) ? sz[rc[i]] : 0;
				sz[i] = 1 + szl + szr;
				const idx_t whole = idx_t(sz[i]) * idx_t(lcp[i]); // (count-1) * shared prefix length
				if (whole >= vl + vr) {
					value[i] = whole;
					take_whole[i] = 1;
				} else {
					value[i] = vl + vr;
				}
			}
		}

		// Emit chunks left-to-right (entries stay in `order`), following the recorded decisions. Each frame carries the
		// entry range [a,b] and its governing split index `node` (the leftmost-min lcp of [a,b-1]); a==b is a lone
		// entry.
		using EFrame = CleaveScratch::EFrame;
		auto &estk = scratch.estk;
		estk.clear();
		estk.push_back({0u, m, root}); // m == n - 1 == last entry index
		while (!estk.empty()) {
			const EFrame f = estk.back();
			estk.pop_back();
			const uint32_t a = f.a;
			const uint32_t b = f.b;
			if (a == b) {
				emit_plain(a);
				continue;
			}
			const uint32_t mid = f.node; // a <= mid < b
			if (!take_whole[mid]) {
				estk.push_back({mid + 1, b, rc[mid]}); // right pushed first -> left processed first
				estk.push_back({a, mid, lc[mid]});
				continue;
			}
			if (lcp[mid] == 0) {
				for (uint32_t k = a; k <= b; k++) { // whole range but no common prefix
					emit_plain(k);
				}
				continue;
			}
			const uint32_t plen = lcp[mid];
			const uint32_t prefix_id = NumericCast<uint32_t>(dict.prefixes.size());
			dict.prefixes.push_back({EP(a), plen});
			dict.prefix_bytes += plen;
			dict.max_prefix_len = MaxValue<uint32_t>(dict.max_prefix_len, plen);
			for (uint32_t k = a; k <= b; k++) {
				PlusEntry e;
				e.prefix_id = prefix_id;
				e.suffix = EP(k) + plen;
				e.suffix_len = EL(k) - plen;
				e.original_index = O(k);
				dict.suffix_bytes += e.suffix_len;
				dict.max_suffix_len = MaxValue<uint32_t>(dict.max_suffix_len, e.suffix_len);
				dict.entries.push_back(e);
			}
		}
	} else if (n == 1) {
		emit_plain(0);
	}

	// Fix up the "no prefix" marker now that prefix_count is known.
	const uint32_t sentinel = dict.NoPrefixSentinel();
	for (auto &e : dict.entries) {
		if (e.prefix_id == 0xFFFFFFFFu) {
			e.prefix_id = sentinel;
		}
	}
}

//! Total serialized size (bytes) of a cleaved dictionary INCLUDING the selection buffer for `tuple_count` rows at
//! `indices_width` (pass indices_width == 0 for FSST_PLUS, which has no selection buffer).
inline idx_t CleavedDictionarySize(const CleavedDictionary &dict, idx_t tuple_count, bitpacking_width_t indices_width) {
	idx_t dict_count = dict.entries.size() + 1;
	auto l = DictFSSTPlusLayout::Compute(tuple_count, dict_count, dict.prefixes.size(), indices_width,
	                                     dict.PrefixLengthsWidth(), dict.PrefixIdWidth(), dict.SuffixLengthsWidth(),
	                                     dict.symbol_table_size, dict.prefix_bytes, dict.suffix_bytes);
	return l.total;
}

//! Byte offsets of a native (duckdb-compatible) layout -- header + dictionary bytes + [symbol table] + string
//! lengths + [selection] -- computed once and shared by NativeSize (query) and WriteNative (write) so they cannot
//! drift.
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
	idx_t total; //! == di_dest when the mode has no selection (FSST_ONLY)
};

//! DICTIONARY packs the raw entries (no symbol table); DICT_FSST / FSST_ONLY pack the FSST-encoded entries (with
//! symbol table); FSST_ONLY drops the selection (row order).
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

//! Write a cleaved segment (DICT_FSST_PLUS / FSST_PLUS). `sel_new` is the per-row selection (1-based cleave position,
//! 0=NULL); empty for FSST_PLUS, which has no selection buffer. `symbol_table` is dict_fsst's already-serialized table.
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
		pl.resize(prefix_count); // every index unconditionally written by the loop below
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
		// Index 0 is the null slot (sentinel prefix id, zero suffix), so real entry j lands at index j + 1 and the
		// 1-based selection value indexes these arrays directly -- the same layout as native's dictionary.
		auto &pid = state.serialize_scratch.pid;
		auto &sl = state.serialize_scratch.sl;
		pid.resize(dict_count); // index 0 set explicitly below, the rest by the loop below
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

//! Per-row selection (1-based cleave position; 0 = NULL), written into state.serialize_scratch.sel (reused across
//! flushes). `dictionary_indices[r]` is the pre-cleave dict index (1-based; 0 = NULL); entries[j].original_index is
//! that same pre-cleave list index (0-based) whatever the cleave reordering, so this remap is uniform for the
//! within-run and global-sort cleaves alike.
void BuildSelNew(DictFSSTCompressionState &state, const CleavedDictionary &dict, idx_t entry_n) {
	auto &list_to_new = state.serialize_scratch.list_to_new;
	list_to_new.assign(entry_n, 0); // written through a permutation (original_index), not loop order -- zero the rest
	for (uint32_t j = 0; j < dict.entries.size(); j++) {
		list_to_new[dict.entries[j].original_index] = j + 1;
	}
	idx_t tuple_count = state.tuple_count;
	auto &sel = state.serialize_scratch.sel;
	sel.resize(tuple_count); // every index unconditionally written by the loop below
	const auto &dictionary_indices = state.dictionary_indices;
	for (idx_t r = 0; r < tuple_count; r++) {
		auto di = dictionary_indices[r];
		sel[r] = di == 0 ? 0 : list_to_new[di - 1];
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Dictionary
//===--------------------------------------------------------------------===//
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
		// If the popped entry was already merged into the sorted order, drop it (rebuilt on the next cleave).
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
	sizes.resize(n); // every index unconditionally written by the loop below
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
	// out_sizes/out_ptrs are duckdb_fsst_compress's output arrays -- zero/null-filled (not just resized) since only
	// the library, not a loop here, writes them, and the fill must not depend on its success contract.
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

void Dictionary::Cleave(CleavedDictionary &out, const uint32_t *order) {
	idx_t n = encoded.size();
	out.Reset();
	out.symbol_table_size = symbol_table_size;
	if (order) {
		CleaveEncoded<false>(out, encoded, n, order, cleave_scratch); // sorted order (DICT_FSST_PLUS)
	} else {
		CleaveEncoded<true>(out, encoded, n, nullptr, cleave_scratch); // identity/row order (FSST_PLUS)
	}
}

//===--------------------------------------------------------------------===//
// DictFSSTCompressionState -- lifecycle
//===--------------------------------------------------------------------===//
DictFSSTCompressionState::DictFSSTCompressionState(ColumnDataCheckpointData &checkpoint_data_p,
                                                   unique_ptr<DictFSSTAnalyzeState> &&analyze_p)
    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_DICT_FSST), stats_writer(GetType()),
      analyze(std::move(analyze_p)) {
	// force_dict_fsst_mode selects which dict_fsst mode(s) this segment may use. "default" follows storage ownership
	// (owned -> auto across all modes; native duckdb -> auto across the native modes only, so the file round-trips with
	// vanilla duckdb). "auto"/"auto_native" force those two policies regardless of ownership; the five mode names pin a
	// single written mode. Every case runs through the one accumulate-and-cut path; allow_plus is just "may the cut
	// cleave to a plus mode" -- false (auto_native / a pinned native mode) means it never does, so only native modes
	// are written.
	const auto &mode = Settings::Get<ForceDictFsstModeSetting>(checkpoint_data_p.GetDatabase());
	if (mode == "DEFAULT") {
		forced_mode = DictFSSTMode::COUNT;
		allow_plus =
		    StorageManager::TargetAtLeastVersion(StorageVersion::SERENEDB_V1, checkpoint_data_p.GetStorageVersion());
	} else if (mode == "AUTO") {
		forced_mode = DictFSSTMode::COUNT;
		allow_plus = true;
	} else if (mode == "AUTO_NATIVE") {
		forced_mode = DictFSSTMode::COUNT;
		allow_plus = false;
	} else {
		// OnSet normalizes the setting to upper-case == the enum member names, so the generated FromString maps it
		// directly -- the single DictFSSTMode <-> string mapping. OnSet has already validated `mode` is one of the
		// five mode names here (the DEFAULT/AUTO/AUTO_NATIVE policies are handled above).
		forced_mode = EnumUtil::FromString<DictFSSTMode>(mode);
		allow_plus = IsPlusMode(forced_mode);
	}
	// Native-only policies never cleave; auto starts undecided (ResetSegment mirrors this per segment).
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
	entry_at_cleave = 0;
	flat_at_cleave = 0;
	sel_at_cleave = 0;
	null_count = 0;
	rows_since_new = 0;
	// Native-only policies never cleave (auto_native / a pinned native mode); auto starts undecided.
	committed = allow_plus ? CutCommit::UNDECIDED : CutCommit::PLAIN;
	fit_rows = 0;
	fit_raw_count = 0;
	cut_mode = DictFSSTMode::COUNT;
	// drop dangling spans into the (about-to-be-destroyed) encoded heap; keeps capacity
	cut_dict.Reset();
	cut_dict_row.Reset();
}

//===--------------------------------------------------------------------===//
// DictFSSTCompressionState -- add / undo a row
//===--------------------------------------------------------------------===//
bool DictFSSTCompressionState::AddValue(const string_t &s, bool is_null) {
	if (is_null) {
		dictionary_indices.push_back(0); // 0 == NULL
		null_count++;
		rows_since_new++;
		stats_writer.SetHasNull();
		tuple_count++;
		return false;
	}
	auto result = dict.Add(s);
	dictionary_indices.push_back(result.index + 1); // 1-based
	rows_since_new = result.was_new ? 0 : rows_since_new + 1;
	stats_writer.Update(s);
	tuple_count++;
	return result.was_new;
}

void DictFSSTCompressionState::PopRow(bool was_new) {
	// Undo the last AddValue (to flush the segment WITHOUT that row and start the next one with it). Stats are left
	// as a harmless superset: the flushed segment's min/max only widen, and re-adding the row updates the new
	// segment's stats.
	bool was_null = dictionary_indices.back() == 0;
	dictionary_indices.pop_back();
	tuple_count--;
	if (was_null) {
		null_count--;
	} else if (was_new) {
		dict.PopLastEntry();
	}
}

//===--------------------------------------------------------------------===//
// DictFSSTCompressionState -- cut / size
//===--------------------------------------------------------------------===//
idx_t DictFSSTCompressionState::RefreshCleave() {
	idx_t n = dict.encoded.size();
	const bool all_unique = null_count == 0 && n == tuple_count;
	// FSST_PLUS needs all-unique/no-null (row i maps to entry i): if a repeat/null appeared after committing to it,
	// drop the commitment and re-decide.
	if (committed == CutCommit::PLUS_ROW && !all_unique) {
		committed = CutCommit::UNDECIDED;
	}
	// Which candidates to cleave: once the segment commits to a cleaved mode, only that one (skip the loser's cleave
	// and, for FSST_PLUS, the sort). Uncommitted weighs both -- the sorted DICT_FSST_PLUS candidate, plus the
	// row-order FSST_PLUS candidate for all-unique/no-null (every row maps to its own entry, so no selection buffer
	// is needed).
	const bool do_sorted = committed == CutCommit::UNDECIDED || committed == CutCommit::PLUS_SORTED;
	const bool do_row = all_unique && (committed == CutCommit::UNDECIDED || committed == CutCommit::PLUS_ROW);
	auto indices_width = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(n));
	idx_t sorted_size = DConstants::INVALID_INDEX;
	idx_t row_size = DConstants::INVALID_INDEX;
	if (do_sorted) {
		dict.SyncSortedOrder();
		dict.Cleave(cut_dict, dict.sorted_order.data());
		sorted_size = CleavedDictionarySize(cut_dict, tuple_count, indices_width);
	}
	if (do_row) {
		dict.Cleave(cut_dict_row, nullptr);
		row_size = CleavedDictionarySize(cut_dict_row, tuple_count, 0);
	}
	// Size against exactly the mode finalize will write -- a forced (debug) mode, an already-committed cleaved mode, or
	// otherwise the auto choice among the candidates. Sizing against any OTHER mode could overflow the block: the
	// forced knob writes a non-minimal mode, so its cut must size that mode rather than the auto minimum.
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
	// Between-cleave upper-bound baselines from the CHOSEN mode's cleave, so the bound tracks what will actually be
	// written: FSST_PLUS is row-order with a bigger dict and NO selection buffer; the others use the sorted cleave with
	// a selection. (Sizing a written FSST_PLUS off the smaller sorted dict, or counting a selection it lacks, is
	// wrong.)
	const CleavedDictionary &base = cut_mode == DictFSSTMode::FSST_PLUS ? cut_dict_row : cut_dict;
	cl_dict_bytes = base.prefix_bytes + base.suffix_bytes;
	cl_prefix_count = base.prefixes.size();
	entry_at_cleave = n;
	flat_at_cleave = dict.flat_encoded;
	sel_at_cleave = CurSelBytes();
	return chosen_size;
}

idx_t DictFSSTCompressionState::CachedCutSize() const {
	// Size the current cut_mode from the cleave already in cut_dict/cut_dict_row at the current tuple_count -- the
	// entries have not changed, only the row count. Mirrors exactly what RefreshCleave would compute for cut_mode.
	if (cut_mode == DictFSSTMode::DICT_FSST_PLUS) {
		auto indices_width = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(dict.encoded.size()));
		return CleavedDictionarySize(cut_dict, tuple_count, indices_width);
	}
	if (cut_mode == DictFSSTMode::FSST_PLUS) {
		return CleavedDictionarySize(cut_dict_row, tuple_count, 0);
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
	// VALID upper bound on the current sorted-plus segment size, reset to the EXACT size at each cleave then grown by
	// valid bounds until the next -- so right after a cleave it equals the true size, and the near-block cut refines in
	// a couple of cleaves instead of stepping the whole metadata slack:
	//  - dictionary bytes (prefix + suffix): last cleave's exact bytes + the flat-encoding growth since (the cleave can
	//    only grow the total by <= a new entry's encoded length);
	//  - metadata (prefix_lengths + prefix_ids + suffix_lengths): each new entry adds at most one prefix, so
	//    prefix_count <= cl_prefix_count + (entries since cleave); the arrays are sized at that count with widths
	//    bounded by the max encoded length -- valid across any number of entries or width bumps since the last cleave;
	//  - selection: sized exactly every row, but ONLY for modes that carry it -- a segment committed to FSST_PLUS is
	//    row-order with no selection buffer, so counting it would inflate the bound by tens of KB and fire the cut far
	//    too early (many wasted cleaves); symbol table: constant.
	idx_t entry_n = dict.encoded.size();
	idx_t dict_count = entry_n + 1; // prefix_ids / suffix_lengths carry the leading null slot (see DictFSSTPlusLayout)
	idx_t pc_ub = cl_prefix_count + (entry_n - entry_at_cleave);
	auto pid_w = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(pc_ub)); // sentinel "no prefix" == count
	auto len_w = BitpackingPrimitives::MinimumBitWidth(dict.max_enc_len);      // bounds prefix_len and suffix_len
	idx_t meta_ub = BitpackingPrimitives::GetRequiredSize(pc_ub, len_w) +      // prefix_lengths (one per prefix)
	                BitpackingPrimitives::GetRequiredSize(dict_count, pid_w) + // prefix_ids (one per dict entry)
	                BitpackingPrimitives::GetRequiredSize(dict_count, len_w);  // suffix_lengths (one per entry)
	idx_t dict_ub = cl_dict_bytes + (dict.flat_encoded - flat_at_cleave);
	idx_t sel_ub = cut_mode == DictFSSTMode::FSST_PLUS ? 0 : CurSelBytes();
	return DictFSSTCompression::PLUS_HEADER_SIZE + dict.symbol_table_size + sel_ub + dict_ub + meta_ub;
}

DictFSSTMode DictFSSTCompressionState::EffectiveForcedNativeMode() const {
	// FSST_ONLY has no selection buffer, so it is valid only one-entry-per-row (no nulls, no duplicates). For
	// non-unique data fall back to DICT_FSST so the CUT sizes against -- and finalize writes -- a mode that fits
	// (sizing against FSST_ONLY then writing DICT_FSST's larger selection-carrying layout overflows the block).
	if (forced_mode == DictFSSTMode::FSST_ONLY && (null_count != 0 || dict.raw.size() != tuple_count)) {
		return DictFSSTMode::DICT_FSST;
	}
	return forced_mode;
}

idx_t DictFSSTCompressionState::PlainOnlySize(DictFSSTMode &out_mode) const {
	// A pinned native mode sizes against exactly that mode (the cut must fill the block under what gets written).
	if (IsNativeMode(forced_mode)) {
		out_mode = EffectiveForcedNativeMode();
		return NativeSize(out_mode);
	}
	idx_t entry_n = dict.encoded.size();
	bool all_unique = null_count == 0 && entry_n == tuple_count;
	idx_t s_plain = NativeSize(DictFSSTMode::DICT_FSST);
	idx_t s_fsst_only = all_unique ? NativeSize(DictFSSTMode::FSST_ONLY) : DConstants::INVALID_INDEX;
	// FSST_ONLY reads fastest for all-unique data (no selection indirection), else DICT_FSST.
	out_mode = s_fsst_only <= s_plain ? DictFSSTMode::FSST_ONLY : DictFSSTMode::DICT_FSST;
	return MinValue<idx_t>(s_plain, s_fsst_only);
}

idx_t DictFSSTCompressionState::ChooseMode(idx_t sorted_plus_size, idx_t row_plus_size, DictFSSTMode &out_mode) const {
	DictFSSTMode plain_mode;
	idx_t plain_size = PlainOnlySize(plain_mode);
	idx_t best = MinValue<idx_t>(plain_size, MinValue<idx_t>(sorted_plus_size, row_plus_size));
	// Among layouts within the tie tolerance of the smallest, prefer the read-faster one: a plain native layout (no
	// prefix reconstruct) first, then FSST_PLUS (prefix reconstruct but no selection indirection), then DICT_FSST_PLUS.
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

//===--------------------------------------------------------------------===//
// DictFSSTCompressionState -- serialize
//===--------------------------------------------------------------------===//

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
	sl[0] = 0; // null slot; every other index is unconditionally written by the loop below
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
	// Pinned native mode (force_dict_fsst_mode = dictionary/dict_fsst/fsst_only): write exactly that mode. DICTIONARY
	// writes the raw entries; DICT_FSST/FSST_ONLY need them FSST-encoded first (a small final segment may not have hit
	// the encode threshold yet).
	if (IsNativeMode(forced_mode)) {
		if (entry_n == 0) {
			return WriteNative(DictFSSTMode::DICTIONARY);
		}
		// Same fallback the CUT used (EffectiveForcedNativeMode), so a forced FSST_ONLY that the data can't satisfy
		// is written as the DICT_FSST it was sized against, not an unreadable row-order segment.
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
	bool all_unique = null_count == 0 && entry_n == tuple_count;

	// Committed to a plain (native) layout during the cut because prefix sharing was not worth the metadata: write it
	// straight from the encoded entries, no cleave/sort. (Forced modes ignore the auto give-up and cleave below.)
	if (committed == CutCommit::PLAIN && forced_mode == DictFSSTMode::COUNT) {
		DictFSSTMode plain_mode;
		PlainOnlySize(plain_mode);
		return WriteNative(plain_mode);
	}

	// force_dict_fsst_mode pins a plus mode: cleave and write it directly.
	if (IsPlusMode(forced_mode)) {
		if (forced_mode == DictFSSTMode::FSST_PLUS && all_unique) {
			// FSST_PLUS keeps native FSST_ONLY's shape: no selection buffer, entries in row order, read per row.
			dict.Cleave(cut_dict_row, nullptr);
			return WriteCleavedSegment(*this, DictFSSTMode::FSST_PLUS, cut_dict_row, {}, symbol_table,
			                           dict.symbol_table_size);
		}
		if (forced_mode != DictFSSTMode::FSST_PLUS) {
			// The sorted dict-plus layout.
			dict.SyncSortedOrder();
			dict.Cleave(cut_dict, dict.sorted_order.data());
			BuildSelNew(*this, cut_dict, entry_n);
			return WriteCleavedSegment(*this, DictFSSTMode::DICT_FSST_PLUS, cut_dict, serialize_scratch.sel,
			                           symbol_table, dict.symbol_table_size);
		}
		// forced FSST_PLUS on non-unique data cannot be honored -> fall through to the auto choice below.
	}

	// AUTO: reuse the triggering cleave (pure-auto common flush path -- cut_dict / cut_dict_row +
	// cut_mode are still valid for the unchanged entries) or recompute both candidates; then write the chosen
	// mode. DICT_FSST_PLUS factors shared prefixes with dedup+selection; FSST_PLUS drops the selection for
	// all-unique/no-null data; otherwise a read-faster plain native layout (DICT_FSST, or FSST_ONLY for
	// all-unique/no-null) is used.
	if (!use_cached || forced_mode != DictFSSTMode::COUNT) {
		RefreshCleave();
	}
	if (cut_mode == DictFSSTMode::DICT_FSST_PLUS) {
		BuildSelNew(*this, cut_dict, entry_n);
		return WriteCleavedSegment(*this, cut_mode, cut_dict, serialize_scratch.sel, symbol_table,
		                           dict.symbol_table_size);
	}
	if (cut_mode == DictFSSTMode::FSST_PLUS) {
		return WriteCleavedSegment(*this, cut_mode, cut_dict_row, {}, symbol_table, dict.symbol_table_size);
	}
	return WriteNative(cut_mode);
}

//===--------------------------------------------------------------------===//
// DictFSSTCompressionState -- flush / entry points
//===--------------------------------------------------------------------===//
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

void DictFSSTCompressionState::MaybeEncodeOrCutSmall(idx_t block_size) {
	if (forced_mode != DictFSSTMode::DICTIONARY && dict.raw_bytes >= ENCODE_THRESHOLD) {
		// Enough raw dictionary to be worth FSST-encoding. Encode once it comes within ENCODE_HEADROOM of a block
		// (room for the symbol table + a representative sample), OR once the dictionary has stopped growing (a
		// low-cardinality column would otherwise never reach a block within a row group). The encoded/cleaved cut
		// then fills the rest of the block. (A pinned DICTIONARY never encodes.)
		if (NativeSize(DictFSSTMode::DICTIONARY) + ENCODE_HEADROOM >= block_size ||
		    rows_since_new >= DICT_STABLE_ROWS) {
			dict.EncodeAll();
			// Baseline for the cut upper bound: one real cleave (cheap -- the dictionary is ~threshold-sized here).
			RefreshCleave();
			// The just-encoded state is well under a block -- seed the rewind checkpoint here.
			fit_rows = tuple_count;
			fit_raw_count = dict.raw.size();
		}
	} else if (NativeSize(DictFSSTMode::DICTIONARY) + dict.max_raw_len + ROW_HEADROOM >= block_size) {
		// Tiny raw dictionary (selection-dominated, no FSST benefit): cut a plain DICTIONARY when it fills the block.
		// The plain size is exact, so a one-row margin (largest raw entry + headroom) is enough.
		Flush(false);
	}
}

bool DictFSSTCompressionState::NearBlock(bool was_new) const {
	// Far from the block, no cleave happens: only advance once the segment has grown by CLEAVE_GAP since the last
	// cleave baseline (sparse -> fast), EXCEPT a row that just widened the selection bitpacking (entry count crossed
	// a power of two: its whole-selection jump could overshoot by more than one row) always forces a check so it is
	// caught on its own row.
	idx_t entry_n = dict.encoded.size();
	bool sel_width_bumped = was_new && entry_n >= 2 &&
	                        BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n)) !=
	                            BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n - 1));
	idx_t grown = (dict.flat_encoded - flat_at_cleave) + (CurSelBytes() - sel_at_cleave);
	return grown >= CLEAVE_GAP || sel_width_bumped;
}

void DictFSSTCompressionState::CutPlain(UnifiedVectorFormat &vf, const string_t *strings, idx_t i, bool was_new,
                                        idx_t margin, idx_t block_size) {
	// Plus already lost a cleave this segment (no prefix sharing worth the metadata) -> size against the exact plain
	// layout directly, no more cleave/sort. Same cadence (grown/GAP) and the same size the cleave was returning, so
	// the segment is cut byte-identically to the always-cleave path. Plain size is monotonic in rows, so excluding
	// the overshooting row always recovers the block -- no rewind needed here.
	DictFSSTMode plain_mode;
	idx_t plain_c = PlainOnlySize(plain_mode);
	if (plain_c + margin < block_size) {
		// advance the growth baseline so the next check is a full GAP later (mirrors the cleave's reset)
		flat_at_cleave = dict.flat_encoded;
		sel_at_cleave = CurSelBytes();
		return;
	}
	if (plain_c <= block_size || tuple_count == 1) {
		Flush(false);
		return;
	}
	PopRow(was_new); // overshot -> exclude row i
	Flush(false);
	AddScanRow(vf, strings, i);
}

void DictFSSTCompressionState::CutCleaved(UnifiedVectorFormat &vf, const string_t *strings, idx_t i, bool was_new,
                                          idx_t margin, idx_t block_size) {
	if (CleavedUpperBound() + margin < block_size) {
		return;
	}
	idx_t true_c = RefreshCleave(); // sets cut_mode + resets the growth baseline
	if (committed == CutCommit::UNDECIDED) {
		// First decision this segment -> commit to the winner so later cuts skip the loser's cleave/sort.
		if (!IsPlusMode(cut_mode)) {
			committed = CutCommit::PLAIN; // a plain layout won -> stop cleaving/sorting entirely
		} else {
			// DICT_FSST_PLUS or FSST_PLUS won -> only that cleave hereafter.
			committed = cut_mode == DictFSSTMode::DICT_FSST_PLUS ? CutCommit::PLUS_SORTED : CutCommit::PLUS_ROW;
		}
	}
	if (true_c + margin < block_size) {
		// Fits with room: record the rewind checkpoint the rare escalation rewinds to.
		fit_rows = tuple_count;
		fit_raw_count = dict.raw.size();
		return;
	}
	if (true_c <= block_size || tuple_count == 1) {
		Flush(false, true); // fits -> flush now, reusing the cleave just computed above
		return;
	}
	// Overshot. Excluding the just-added row is the tight recovery. If it added a dictionary entry, that entry is gone
	// now too, so the cleave changes -- re-cleave to size [seg_start..i-1]. If it did NOT (a repeat or a null), the
	// entries are unchanged and only the row count dropped, so size the cleave already computed above directly
	// (CachedCutSize) rather than repeating it. If the excluded segment STILL overflows, reclustering -- not row i
	// alone -- grew it past the block, so rewind to the last comfortably-fitting checkpoint and move that whole tail
	// to the next segment instead.
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
			MaybeEncodeOrCutSmall(block_size);
			continue;
		}
		// Encoded. NearBlock is a cheap gate on CleavedUpperBound (itself a cheap VALID upper bound): far from the
		// block, skip. Near it, CutPlain/CutCleaved flush the segment -- WITH the current row when it fits, else
		// rewind to the last fitting checkpoint (moving the overshoot rows to the next segment).
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
	Flush(true);
}

} // namespace dict_fsst
} // namespace duckdb
