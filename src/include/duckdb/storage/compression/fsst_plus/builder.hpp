#pragma once

#include "duckdb/storage/compression/fsst_plus/common.hpp"
#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/exception.hpp"
#include "fsst.h"

#include <algorithm>
#include <limits>

namespace duckdb {
namespace fsst_plus {

//! One shared prefix (a span of FSST-encoded bytes inside CleavedDictionary::encoded).
struct PlusPrefix {
	const unsigned char *data;
	uint32_t len;
};

//! Per dictionary entry in cleave order (entries 1..dict_count-1; entry 0 == NULL).
struct PlusEntry {
	uint32_t prefix_id;         //! index into prefixes, or == prefix_count when no prefix
	const unsigned char *suffix; //! span into CleavedDictionary::encoded
	uint32_t suffix_len;
	uint32_t original_index;    //! position in the pre-cleave entry list (for selection remap)
};

//! Result of FSST-encoding + cleaving a set of dictionary entries. Owns the FSST
//! encoder and the encoded byte backing store; prefix/suffix spans point into it.
struct CleavedDictionary {
	void *encoder = nullptr;
	vector<unsigned char> encoded; //! sized once; never resized so spans stay valid
	vector<PlusPrefix> prefixes;
	vector<PlusEntry> entries;
	idx_t symbol_table_size = 0;

	uint32_t max_prefix_len = 0;
	uint32_t max_suffix_len = 0;
	idx_t prefix_bytes = 0;
	idx_t suffix_bytes = 0;

	CleavedDictionary() = default;
	CleavedDictionary(const CleavedDictionary &) = delete;
	CleavedDictionary &operator=(const CleavedDictionary &) = delete;
	~CleavedDictionary() {
		if (encoder) {
			duckdb_fsst_destroy(reinterpret_cast<duckdb_fsst_encoder_t *>(encoder));
		}
	}

	bitpacking_width_t PrefixLengthsWidth() const {
		return BitpackingPrimitives::MinimumBitWidth(max_prefix_len);
	}
	bitpacking_width_t SuffixLengthsWidth() const {
		return BitpackingPrimitives::MinimumBitWidth(max_suffix_len);
	}
	//! sentinel "no prefix" == prefixes.size(), so the width must cover that value
	bitpacking_width_t PrefixIdWidth() const {
		return BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(prefixes.size()));
	}
	uint32_t NoPrefixSentinel() const {
		return NumericCast<uint32_t>(prefixes.size());
	}
};

//! Lexicographic compare of two FSST-encoded spans, truncated to MAX_PREFIX,
//! matching the ordering used by the thesis TruncatedSort.
inline bool EncodedLess(const unsigned char *a, idx_t la, const unsigned char *b, idx_t lb) {
	idx_t ta = MinValue<idx_t>(la, FSSTPlusCompression::MAX_PREFIX);
	idx_t tb = MinValue<idx_t>(lb, FSSTPlusCompression::MAX_PREFIX);
	idx_t c = MinValue<idx_t>(ta, tb);
	for (idx_t i = 0; i < c; i++) {
		if (a[i] != b[i]) {
			return a[i] < b[i];
		}
	}
	return ta > tb;
}

//! Cleave one run (<= CLEAVE_RUN encoded entries) via TruncatedSort + the thesis
//! DP, appending prefixes + entries to `dict`. `enc_ptr`/`enc_len` are the encoded
//! spans of the whole entry list; `base`/`n` delimit the run.
inline void CleaveRun(CleavedDictionary &dict, const vector<const unsigned char *> &enc_ptr,
                      const vector<uint32_t> &enc_len, idx_t base, idx_t n) {
	// index array over the run, sorted by encoded bytes
	vector<uint32_t> idx(n);
	for (idx_t i = 0; i < n; i++) {
		idx[i] = NumericCast<uint32_t>(base + i);
	}
	std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
		return EncodedLess(enc_ptr[a], enc_len[a], enc_ptr[b], enc_len[b]);
	});
	// LCP between consecutive (escape-255 guarded so we never split a code)
	vector<idx_t> lcp(n ? n - 1 : 0, 0);
	for (idx_t i = 0; i + 1 < n; i++) {
		idx_t maxl = MinValue<idx_t>(MinValue<idx_t>(enc_len[idx[i]], enc_len[idx[i + 1]]), FSSTPlusCompression::MAX_PREFIX);
		const unsigned char *s1 = enc_ptr[idx[i]];
		const unsigned char *s2 = enc_ptr[idx[i + 1]];
		idx_t l = 0;
		while (l < maxl && s1[l] == s2[l]) {
			l++;
		}
		if (l != 0 && s1[l - 1] == 255) {
			l--;
		}
		lcp[i] = l;
	}
	// min_lcp[i][j] over the run
	vector<vector<idx_t>> min_lcp(n, vector<idx_t>(n, 0));
	for (idx_t i = 0; i < n; i++) {
		min_lcp[i][i] = MinValue<idx_t>(enc_len[idx[i]], FSSTPlusCompression::MAX_PREFIX);
		for (idx_t j = i + 1; j < n; j++) {
			min_lcp[i][j] = MinValue<idx_t>(min_lcp[i][j - 1], lcp[j - 1]);
		}
	}
	vector<idx_t> psum(n + 1, 0);
	for (idx_t i = 0; i < n; i++) {
		psum[i + 1] = psum[i] + enc_len[idx[i]];
	}
	const idx_t INF = std::numeric_limits<idx_t>::max();
	vector<idx_t> dp(n + 1, INF), prev(n + 1, 0), pfor(n + 1, 0);
	dp[0] = 0;
	for (idx_t i = 1; i <= n; i++) {
		for (idx_t j = 0; j < i; j++) {
			idx_t mcp = min_lcp[j][i - 1];
			idx_t candidates[2] = {0, mcp};
			for (idx_t ci = 0; ci < 2; ci++) {
				idx_t p = candidates[ci];
				idx_t cnt = i - j;
				idx_t per = 1 + (p > 0 ? 2 : 0);
				idx_t cost = dp[j] + cnt * per + (psum[i] - psum[j]) - (cnt - 1) * p;
				if (cost < dp[i]) {
					dp[i] = cost;
					prev[i] = j;
					pfor[i] = p;
				}
			}
		}
	}
	// reconstruct chunks (reverse)
	vector<idx_t> chunk_starts;
	vector<idx_t> chunk_pfx;
	for (idx_t k = n; k > 0;) {
		idx_t s = prev[k];
		chunk_starts.push_back(s);
		chunk_pfx.push_back(pfor[k]);
		k = s;
	}
	std::reverse(chunk_starts.begin(), chunk_starts.end());
	std::reverse(chunk_pfx.begin(), chunk_pfx.end());
	for (idx_t c = 0; c < chunk_starts.size(); c++) {
		idx_t cstart = chunk_starts[c];
		idx_t cstop = (c + 1 < chunk_starts.size()) ? chunk_starts[c + 1] : n;
		idx_t plen = chunk_pfx[c];
		uint32_t prefix_id;
		if (plen > 0) {
			prefix_id = NumericCast<uint32_t>(dict.prefixes.size());
			PlusPrefix pfx;
			pfx.data = enc_ptr[idx[cstart]];
			pfx.len = NumericCast<uint32_t>(plen);
			dict.prefixes.push_back(pfx);
			dict.prefix_bytes += plen;
			if (pfx.len > dict.max_prefix_len) {
				dict.max_prefix_len = pfx.len;
			}
		} else {
			prefix_id = 0; // fixed up to the sentinel after prefix_count is known
		}
		for (idx_t k = cstart; k < cstop; k++) {
			uint32_t g = idx[k];
			PlusEntry e;
			e.prefix_id = plen > 0 ? prefix_id : 0xFFFFFFFFu; // temporary marker for "no prefix"
			e.suffix = enc_ptr[g] + plen;
			e.suffix_len = enc_len[g] - NumericCast<uint32_t>(plen);
			e.original_index = g;
			dict.suffix_bytes += e.suffix_len;
			if (e.suffix_len > dict.max_suffix_len) {
				dict.max_suffix_len = e.suffix_len;
			}
			dict.entries.push_back(e);
		}
	}
}

//! FSST-encode `entries_in` and cleave them into `dict`. Returns false (graceful,
//! no throw) if any entry is >= STRING_SIZE_LIMIT or FSST could not encode all
//! entries into the conservative buffer -- the caller then opts out to dict_fsst.
inline bool BuildCleavedDictionary(CleavedDictionary &dict, const vector<string_t> &entries_in,
                                   bool enable_prefix = true) {
	idx_t n = entries_in.size();
	if (n == 0) {
		return true;
	}
	vector<size_t> len_in(n);
	vector<unsigned char *> str_in(n);
	idx_t total = 0;
	for (idx_t i = 0; i < n; i++) {
		auto sz = entries_in[i].GetSize();
		if (sz >= FSSTPlusCompression::STRING_SIZE_LIMIT) {
			return false;
		}
		len_in[i] = sz;
		str_in[i] = (unsigned char *)entries_in[i].GetData(); // NOLINT: fsst API is non-const
		total += sz;
	}
	dict.encoder = reinterpret_cast<void *>(duckdb_fsst_create(n, len_in.data(), str_in.data(), 0));
	auto encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(dict.encoder);

	// Conservative output size per fsst.h (7 + 2*len). Sized once; never resized.
	dict.encoded.resize(7 + 2 * total + 1);
	vector<size_t> out_len(n, 0);
	vector<unsigned char *> out_ptr(n, nullptr);
	idx_t compressed = duckdb_fsst_compress(encoder, n, len_in.data(), str_in.data(), dict.encoded.size(),
	                                        dict.encoded.data(), out_len.data(), out_ptr.data());
	if (compressed != n) {
		return false;
	}
	vector<const unsigned char *> enc_ptr(n);
	vector<uint32_t> enc_len(n);
	for (idx_t i = 0; i < n; i++) {
		enc_ptr[i] = out_ptr[i];
		enc_len[i] = NumericCast<uint32_t>(out_len[i]);
	}
	dict.entries.reserve(n);
	if (enable_prefix) {
		for (idx_t base = 0; base < n; base += FSSTPlusCompression::CLEAVE_RUN) {
			idx_t run = MinValue<idx_t>(FSSTPlusCompression::CLEAVE_RUN, n - base);
			CleaveRun(dict, enc_ptr, enc_len, base, run);
		}
	} else {
		for (idx_t i = 0; i < n; i++) {
			PlusEntry e;
			e.prefix_id = 0xFFFFFFFFu; // no prefix; fixed to sentinel below
			e.suffix = enc_ptr[i];
			e.suffix_len = enc_len[i];
			e.original_index = NumericCast<uint32_t>(i);
			dict.suffix_bytes += e.suffix_len;
			if (e.suffix_len > dict.max_suffix_len) {
				dict.max_suffix_len = e.suffix_len;
			}
			dict.entries.push_back(e);
		}
	}
	// fix up the "no prefix" sentinel now that prefix_count is known
	uint32_t sentinel = dict.NoPrefixSentinel();
	for (auto &e : dict.entries) {
		if (e.prefix_id == 0xFFFFFFFFu) {
			e.prefix_id = sentinel;
		}
	}
	unsigned char header_buf[FSST_MAXHEADER];
	dict.symbol_table_size = duckdb_fsst_export(encoder, header_buf);
	return true;
}

//! Byte offsets (relative to the block start) of every region in a FSST+ segment.
//! Computed once and used by BOTH the writer (Finalize) and the reader
//! (Initialize) so the two can never disagree. `dict_count` includes entry 0
//! (NULL); entry_count == dict_count - 1.
struct FSSTPlusLayout {
	idx_t selection_space = 0;
	idx_t prefix_lengths_space = 0;
	idx_t prefix_ids_space = 0;
	idx_t suffix_lengths_space = 0;

	idx_t selection_dest = 0;
	idx_t symtab_dest = 0;
	idx_t prefix_lengths_dest = 0;
	idx_t prefix_bytes_dest = 0;
	idx_t prefix_ids_dest = 0;
	idx_t suffix_lengths_dest = 0;
	idx_t suffix_bytes_dest = 0;
	idx_t total = 0;

	static FSSTPlusLayout Compute(idx_t tuple_count, idx_t dict_count, idx_t prefix_count,
	                              bitpacking_width_t indices_width, bitpacking_width_t prefix_lengths_width,
	                              bitpacking_width_t prefix_id_width, bitpacking_width_t suffix_lengths_width,
	                              idx_t symbol_table_size, idx_t prefix_bytes, idx_t suffix_bytes) {
		FSSTPlusLayout l;
		idx_t entry_count = dict_count > 0 ? dict_count - 1 : 0;
		l.selection_space = BitpackingPrimitives::GetRequiredSize(tuple_count, indices_width);
		l.prefix_lengths_space = BitpackingPrimitives::GetRequiredSize(prefix_count, prefix_lengths_width);
		l.prefix_ids_space = BitpackingPrimitives::GetRequiredSize(entry_count, prefix_id_width);
		l.suffix_lengths_space = BitpackingPrimitives::GetRequiredSize(entry_count, suffix_lengths_width);

		l.selection_dest = AlignValue<idx_t>(FSSTPlusCompression::HEADER_SIZE);
		l.symtab_dest = AlignValue<idx_t>(l.selection_dest + l.selection_space);
		l.prefix_lengths_dest = AlignValue<idx_t>(l.symtab_dest + symbol_table_size);
		l.prefix_bytes_dest = AlignValue<idx_t>(l.prefix_lengths_dest + l.prefix_lengths_space);
		l.prefix_ids_dest = AlignValue<idx_t>(l.prefix_bytes_dest + prefix_bytes);
		l.suffix_lengths_dest = AlignValue<idx_t>(l.prefix_ids_dest + l.prefix_ids_space);
		l.suffix_bytes_dest = AlignValue<idx_t>(l.suffix_lengths_dest + l.suffix_lengths_space);
		l.total = l.suffix_bytes_dest + suffix_bytes;
		return l;
	}
};

//! Total serialized size (bytes) of a cleaved dictionary INCLUDING the selection
//! buffer for `tuple_count` rows at `indices_width`.
inline idx_t CleavedDictionarySize(const CleavedDictionary &dict, idx_t tuple_count,
                                   bitpacking_width_t indices_width) {
	idx_t dict_count = dict.entries.size() + 1;
	auto l = FSSTPlusLayout::Compute(tuple_count, dict_count, dict.prefixes.size(), indices_width,
	                                 dict.PrefixLengthsWidth(), dict.PrefixIdWidth(), dict.SuffixLengthsWidth(),
	                                 dict.symbol_table_size, dict.prefix_bytes, dict.suffix_bytes);
	return l.total;
}

} // namespace fsst_plus
} // namespace duckdb
