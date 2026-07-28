#pragma once

#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/storage/compression/dict_fsst/common.hpp"
#include "duckdb/storage/compression/dict_fsst/analyze.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/common/bitpacking.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"

#include <absl/container/flat_hash_map.h>

namespace duckdb {
namespace dict_fsst {

// Dictionary compression uses a combination of bitpacking and a dictionary to compress string segments. The data is
// stored across three buffers: the index buffer, the selection buffer and the dictionary. Firstly the Index buffer
// contains the offsets into the dictionary which are also used to determine the string lengths. Each value in the
// dictionary gets a single unique index in the index buffer. Secondly, the selection buffer maps the tuples to an index
// in the index buffer. The selection buffer is compressed with bitpacking. Finally, the dictionary contains simply all
// the unique strings without lengths or null termination as we can deduce the lengths from the index buffer. The
// addition of the selection buffer is done for two reasons: firstly, to allow the scan to emit dictionary vectors by
// scanning the whole dictionary at once and then scanning the selection buffer for each emitted vector. Secondly, it
// allows for efficient bitpacking compression as the selection values should remain relatively small.

//! Reused scratch for Dictionary::EncodeAll (once per segment): batch FSST-compress input/output spans.
struct EncodeScratch {
	vector<size_t> sizes;
	vector<unsigned char *> ptrs;
	vector<size_t> out_sizes;
	vector<unsigned char *> out_ptrs;
};

//===--------------------------------------------------------------------===//
// Cleaved (FSST+) dictionary
//===--------------------------------------------------------------------===//
//! One shared prefix (a span of FSST-encoded bytes; the data pointer borrows the caller-owned encoded byte copy).
struct PlusPrefix {
	const unsigned char *data;
	uint32_t len;
};

//! Per dictionary entry in cleave order (entries 1..dict_count-1; entry 0 == NULL).
struct PlusEntry {
	uint32_t prefix_id;          //! index into prefixes, or == prefix_count when no prefix
	const unsigned char *suffix; //! span into the caller-owned encoded byte copy
	uint32_t suffix_len;
	uint32_t original_index; //! position in the pre-cleave entry list (0-based over entries 1..dict_count-1)
};

//! Result of cleaving a set of ALREADY-FSST-ENCODED dictionary entries. Prefix/suffix spans borrow the caller's
//! encoded byte copy; the FSST encoder + serialized symbol table are owned by the compression state (reused, not
//! re-created), so this struct owns nothing and never destroys an encoder.
struct CleavedDictionary {
	vector<PlusPrefix> prefixes;
	vector<PlusEntry> entries;
	//! Size of the serialized FSST symbol table (supplied by the caller: it is the SAME table dict_fsst already built).
	idx_t symbol_table_size = 0;

	uint32_t max_prefix_len = 0;
	uint32_t max_suffix_len = 0;
	idx_t prefix_bytes = 0;
	idx_t suffix_bytes = 0;

	//! Clear for reuse across cleaves, keeping vector capacity (CleaveEncoded appends from empty).
	void Reset() {
		prefixes.clear();
		entries.clear();
		max_prefix_len = 0;
		max_suffix_len = 0;
		prefix_bytes = 0;
		suffix_bytes = 0;
		symbol_table_size = 0;
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

//! Reused scratch buffers for CleaveEncoded (one owned by Dictionary, see Dictionary::cleave_scratch), so repeated
//! trial cleaves (RefreshCleave) do not heap-allocate. Every buffer is assign/resize/clear()'d each call to exactly
//! the range it needs before anything reads it, so a previous (possibly larger) call's contents are never observed.
struct CleaveScratch {
	//! Post-order DP traversal stack frame over the Cartesian tree: `node` still needs its children pushed if !done.
	struct DFrame {
		uint32_t node;
		bool done;
	};
	//! Emit-order stack frame: entry range [a,b] and its governing split index `node` (leftmost-min lcp of [a,b-1]).
	struct EFrame {
		uint32_t a, b;
		uint32_t node;
	};

	vector<uint32_t> lcp;       //! adjacent-LCP array, size n-1
	vector<uint32_t> lc, rc;    //! Cartesian-tree children per lcp node, NONE == no child
	vector<uint32_t> mono;      //! monotonic-stack scratch used while building lc/rc
	vector<idx_t> value;        //! max prefix savings per subtree
	vector<uint32_t> sz;        //! subtree size (lcp-nodes) per subtree
	vector<uint8_t> take_whole; //! whether a subtree shares its whole-range min LCP rather than splitting
	vector<DFrame> dstk;        //! post-order DP traversal stack
	vector<EFrame> estk;        //! emit traversal stack
};

//===--------------------------------------------------------------------===//
// Dictionary
//===--------------------------------------------------------------------===//
//! The deduplicated entries accumulated for the segment being compressed, their FSST encoding, and the scratch
//! buffers needed to cleave them into a shared-prefix (FSST+) layout. Lives for a segment (see
//! DictFSSTCompressionState::dict), reset between segments by Clear(). Selection/cut/framework state is the
//! compressor's concern, not the dictionary's.
struct Dictionary {
public:
	~Dictionary();

	//! True once EncodeAll has run: the FSST encoder exists iff the entries have been encoded.
	bool EncodedReady() const {
		return encoder != nullptr;
	}

	//! Reset for the next segment: destroy both heaps, clear the entry vectors + dedup map, zero the counters/flags,
	//! destroy the FSST encoder, and invalidate the symbol table size.
	void Clear();

	struct AddResult {
		uint32_t index; //! 0-based entry index (new or pre-existing)
		bool was_new;
	};
	//! Add one non-null value: dedup, and if new, copy into raw_heap, push to raw, update raw_bytes/max_raw_len, and
	//! (once EncodedReady()) FSST-encode it immediately. Returns the entry's 0-based index and whether it is new.
	AddResult Add(const string_t &s);
	//! Pop the most-recently-added entry (dedup + raw, and if EncodedReady() the matching encoded entry too),
	//! invalidating sorted_order if it had already synced past the popped entry. The single entry-pop primitive
	//! shared by the compressor's row-undo and the overshoot rewind.
	void PopLastEntry();

	//! Build the FSST symbol table from the raw entries collected so far and encode all of them (once the raw
	//! dictionary crosses the encode threshold); after this new entries are encoded incrementally by EncodeOne. Does
	//! NOT seed the cut upper-bound baseline -- the caller that needs that (the encode-trigger path in Compress)
	//! calls RefreshCleave right after.
	void EncodeAll();
	void EncodeOne(const string_t &raw);
	//! Append `count` freshly FSST-compressed spans to the encoded heap (pushed to `encoded`), updating the byte total
	//! + max length. Shared by the batch (EncodeAll) and incremental (EncodeOne) encode.
	void StoreEncoded(const size_t *out_sizes, unsigned char *const *out_ptrs, idx_t count);
	//! Bring sorted_order (the global lexicographic order of the encoded entries) up to date by sorting the
	//! newly-appended tail and merging it in -- so repeated cut-estimation cleaves never re-sort the whole list.
	void SyncSortedOrder();
	//! Cleave the current encoded entries into `out` (reset first, reusing its capacity), stamping the symbol-table
	//! size. `order` is the sorted order (DICT_FSST_PLUS); null uses the identity/row order (FSST_PLUS).
	void Cleave(CleavedDictionary &out, const uint32_t *order);

public:
	//! Owns the raw (uncompressed) entry bytes; raw[j] borrows into this and is dict entry j+1 (entry 0 == NULL).
	StringHeap raw_heap;
	vector<string_t> raw;
	//! The FSST-encoded entries: encoded_heap owns the bytes, encoded[i] borrows into it (populated once EncodeAll has
	//! run). Short entries inline into the string_t itself -- no arena allocation. Mirrors raw/raw_heap. The
	//! sort/cleave/serialize read each entry's span directly via its string_t; valid because `encoded` never
	//! changes within a cleave, so no denormalized pointer/length arrays are kept.
	StringHeap encoded_heap;
	vector<string_t> encoded;
	//! raw value -> 0-based entry index. A hash map, not the native path's fixed-size PrimitiveDictionary: the segment
	//! accumulates until the CLEAVED size fills a block, so it holds more unique entries than fit uncleaved, and the
	//! raw bytes must stay addressable as string_t for the dedup keys and DICTIONARY-mode write -- so raw lives in the
	//! heap above.
	absl::flat_hash_map<string_t, uint32_t, StringHash, StringEquality> dedup;
	idx_t raw_bytes = 0;    //! running raw byte total (drives the encode threshold)
	idx_t flat_encoded = 0; //! running encoded byte total
	uint32_t max_raw_len = 0;
	uint32_t max_enc_len = 0;
	//! FSST encoder (its existence is EncodedReady()), its serialized symbol table, and a reusable compress buffer.
	void *encoder = nullptr;
	unsafe_unique_array<unsigned char> symbol_table;
	idx_t symbol_table_size = DConstants::INVALID_INDEX;
	unsafe_unique_array<unsigned char> encode_buffer = nullptr;
	idx_t encode_buffer_size = 0;

	//! Cleave scratch (members only to reuse the allocation across cleaves).
	//! Global lexicographic (string_t <) order of the encoded entries (the DICT_FSST_PLUS cleave order; the row cleave
	//! uses identity).
	vector<uint32_t> sorted_order;
	//! Destination of SyncSortedOrder's merge, swapped into sorted_order (member only to reuse the allocation).
	vector<uint32_t> merge_result;
	//! Reused CleaveEncoded scratch (cut_dict / cut_dict_row cleaves run sequentially, so one scratch suffices).
	CleaveScratch cleave_scratch;
	//! Reused EncodeAll scratch.
	EncodeScratch encode_scratch;
};

//! The segment's cut commitment, decided at the first near-block cleave (or fixed to PLAIN from the start for
//! native-only policies): which candidate(s) the cut weighs and how the segment is written.
enum class CutCommit : uint8_t {
	UNDECIDED,   //! weigh the plain layout + both plus candidates each cut
	PLAIN,       //! a plain (native) layout won -> stop cleaving; cut and write the exact plain size
	PLUS_SORTED, //! committed to DICT_FSST_PLUS (sorted cleave + selection)
	PLUS_ROW,    //! committed to FSST_PLUS (row-order cleave, no selection)
};

//! Reused scratch for the write/serialize path (per-flush, not per-cleave): grouped so the compressor does not carry
//! a pile of loose buffers. Each buffer is resized/filled at its use site right before reading, so contents left
//! over from a previous flush are never observed.
struct SerializeScratch {
	vector<uint32_t> list_to_new; //! BuildSelNew: pre-cleave dict index -> cleaved position (1-based)
	vector<uint32_t> sel;         //! BuildSelNew output: per-row selection; consumed by WriteCleavedSegment
	vector<uint32_t> pl;          //! WriteCleavedSegment: prefix lengths
	vector<uint32_t> pid;         //! WriteCleavedSegment: prefix ids (dict_count-sized, incl. the null slot)
	vector<uint32_t> sl;          //! WriteCleavedSegment suffix lengths / WriteNative string lengths (dict_count-sized)
};

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
struct DictFSSTCompressionState : public StandardCompressionState {
public:
	DictFSSTCompressionState(ColumnDataCheckpointData &checkpoint_data_p, unique_ptr<DictFSSTAnalyzeState> &&state);

public:
	// ---- Lifecycle ----
	void CreateEmptySegment();
	void ResetSegment();

	// ---- Add / undo a row ----
	//! Append one row for value `s` (or NULL): dedup, lazily FSST-encode a new entry, update stats + selection.
	//! Returns whether it added a NEW dictionary entry. Shared by the scan-vector add path and the overshoot
	//! rewind's re-add.
	bool AddValue(const string_t &s, bool is_null);
	//! Undo the last AddValue (to flush the segment WITHOUT that row and start the next one with it): pop the
	//! selection, and if it was a new dictionary entry, pop it too. Stats are left as a harmless superset -- the
	//! flushed segment's min/max only widen, and re-adding the row updates the new segment's stats.
	void PopRow(bool was_new);
	//! Give up a commitment to FSST_PLUS once a null or duplicate row makes that layout impossible.
	void RowModeBroken();

	// ---- Cut / size ----
	//! Cleave the current entries (global sort), record the exact dict/metadata byte baselines, and return the exact
	//! cleaved segment size (incl. symbol table + selection buffer).
	idx_t RefreshCleave();
	//! Exact size of the current cut_mode using the cleave ALREADY in cut_dict/cut_dict_row, at the current
	//! tuple_count -- no re-cleave. Valid only when the encoded entries are unchanged since the last RefreshCleave
	//! (the overshoot path uses it after popping a row that added no dictionary entry).
	idx_t CachedCutSize() const;
	//! Serialized size of a native (duckdb-compatible) layout: DICTIONARY (raw entries, no symbol table), DICT_FSST
	//! (encoded + selection), or FSST_ONLY (encoded, no selection, all-unique/no-null). Matches WriteNative.
	idx_t NativeSize(DictFSSTMode mode) const;
	//! Current selection-buffer size in bytes (bitpacked tuple_count at the width for the entry count).
	idx_t CurSelBytes() const;
	//! Cheap, valid UPPER bound on the current cleaved segment size, from the last trial cleave plus bounded growth.
	idx_t CleavedUpperBound() const;
	//! The native mode a forced (pinned) native policy actually writes: forced_mode itself, except forced FSST_ONLY
	//! falls back to DICT_FSST for non-unique data (FSST_ONLY has no selection buffer). Used by BOTH the cut sizing
	//! and finalize so they agree on a mode that fits the block.
	DictFSSTMode EffectiveForcedNativeMode() const;
	//! The smallest plain (native) encoded layout and its mode -- FSST_ONLY for all-unique/no-null (no selection), else
	//! DICT_FSST. O(1), no cleave/sort. Shared by ChooseMode, the give-up-cleave cut, and finalize.
	idx_t PlainOnlySize(DictFSSTMode &out_mode) const;
	//! Choose the write mode and return ITS size, given the sorted (DICT_FSST_PLUS) and row-order (FSST_PLUS) cleave
	//! sizes; row_plus_size is INVALID when FSST_PLUS does not apply (only all-unique/no-null rows map identically to
	//! entries, so row order needs no selection buffer). Picks the smallest of DICT_FSST_PLUS / FSST_PLUS / DICT_FSST /
	//! FSST_ONLY, preferring a read-faster layout within MODE_TIE_TOL. Shared by the cut and finalize so they
	//! agree.
	idx_t ChooseMode(idx_t sorted_plus_size, idx_t row_plus_size, DictFSSTMode &out_mode) const;

	// ---- Serialize ----
	//! Write a native (duckdb-compatible) layout for the given mode (DICTIONARY / DICT_FSST / FSST_ONLY) straight from
	//! the in-memory raw or FSST-encoded entries.
	idx_t WriteNative(DictFSSTMode mode);
	//! Serialize the accumulated segment, choosing the smallest applicable mode (see FinalizeSegment). When
	//! use_cached is set (pure auto, common flush path) the triggering cleave in cut_dict is reused as-is.
	idx_t FinalizeSegment(bool use_cached);

	// ---- Flush / entry points ----
	//! Overshoot recovery: the segment grew past the block since the last fitting cleave (reclustering /
	//! selection-width bump). Move the rows added since that checkpoint (fit_rows/fit_raw_count) to the next
	//! segment -- copying their values since the flush frees the entry heap -- rewind to the checkpoint, flush, and
	//! re-add them.
	void FlushRewind();
	//! Re-run one scan-vector row's add through AddValue. Shared by the main Compress loop and the undo-then-readd
	//! recovery paths (which re-add the same row after PopRow).
	bool AddScanRow(UnifiedVectorFormat &vf, const string_t *strings, idx_t j);
	//! Not yet FSST-encoded: check the encode trigger (raw dictionary big enough and either near a block or stopped
	//! growing) or, for a tiny selection-dominated dictionary, cut a plain DICTIONARY once it fills the block.
	void MaybeEncodeOrCutSmall(idx_t block_size);
	//! Cheap gate before the (still cheap, but less so) cut checks: true once the segment has grown by CLEAVE_GAP
	//! since the last cleave baseline, or the just-added entry crossed a selection-bitpacking width (which can
	//! overshoot by more than one row, so it cannot wait for the next gap).
	bool NearBlock(bool was_new_i) const;
	//! Cut path once the segment has given up cleaving (a plain layout already won this segment): size against the
	//! exact plain layout and flush -- WITH the current row when it fits, else excluding it (plain size is monotonic
	//! in rows, so excluding the overshooting row always recovers -- no rewind needed).
	void CutPlain(UnifiedVectorFormat &vf, const string_t *strings, idx_t i, bool was_new_i, idx_t margin,
	              idx_t block_size);
	//! Cut path while still (possibly) cleaving: gated by CleavedUpperBound, does the real RefreshCleave, commits to
	//! the winning mode on the segment's first decision, and on overshoot excludes row i (re-cleave) or, if that
	//! still overflows, rewinds to the last comfortably-fitting checkpoint.
	void CutCleaved(UnifiedVectorFormat &vf, const string_t *strings, idx_t i, bool was_new_i, idx_t margin,
	                idx_t block_size);
	//! Accumulates the whole segment in memory so it can be cut against the CLEAVED size -- packing enough rows that
	//! the block fills UNDER the prefix-factored encoding. One entry list (raw + FSST-encoded), one row->entry
	//! selection, cleaved once per segment at flush.
	void Compress(const Vector &scan_vector);
	void FinalizeCompress();
	//! use_cached_cleave: reuse cut_dict/cut_mode from the triggering RefreshCleave (common flush path,
	//! no entry change since) instead of re-cleaving from scratch.
	void Flush(bool final, bool use_cached_cleave = false);

	// ---- Dictionary: entries, FSST encoding, dedup, cleave scratch ----
	Dictionary dict;

	// ---- Per-row selection ----
	//! Row -> entry (1-based; 0 == NULL); bitpacked as the DICT_FSST / DICT_FSST_PLUS selection buffer.
	vector<uint32_t> dictionary_indices;

	// ---- Serialize scratch (reused across flushes; see SerializeScratch) ----
	SerializeScratch serialize_scratch;

	// ---- Cut / cadence state (transient, reset each segment by ResetSegment) ----
	//! The cleave from the last RefreshCleave, reused by the common flush path so finalize does not re-cleave the
	//! identical entries. cut_dict is the sorted cleave (DICT_FSST_PLUS); cut_dict_row the row-order cleave (FSST_PLUS,
	//! built only for all-unique/no-null segments).
	CleavedDictionary cut_dict;
	CleavedDictionary cut_dict_row;
	DictFSSTMode cut_mode = DictFSSTMode::COUNT;
	//! The segment's cut commitment (see CutCommit): fixed to PLAIN from the start for native-only policies, else
	//! decided at the first near-block cleave so later cuts skip the loser's candidate.
	CutCommit committed = CutCommit::UNDECIDED;
	//! Between-cleave upper-bound baselines: CleavedUpperBound resets to these EXACT counts at each cleave then adds
	//! only bounded growth, so most rows cost arithmetic and a real cleave runs only when that (tight) bound nears the
	//! block.
	idx_t cl_dict_bytes = 0;
	idx_t cl_prefix_count = 0;
	idx_t entry_at_cleave = 0;
	idx_t flat_at_cleave = 0;
	idx_t sel_at_cleave = 0;
	//! Last cleave that comfortably fit the block; FlushRewind rewinds an overshoot here and moves the excess rows on.
	idx_t fit_rows = 0;
	idx_t fit_raw_count = 0;

	// ---- Mode (resolved once from force_dict_fsst_mode) ----
	//! Whether the cut may cleave to a plus mode (DICT_FSST_PLUS / FSST_PLUS); false forces the native modes only.
	bool allow_plus = false;
	//! Pins a specific written mode; COUNT means auto-choose.
	DictFSSTMode forced_mode = DictFSSTMode::COUNT;

	// ---- Framework / output ----
	StatsWriter<string_t> stats_writer;
	unique_ptr<DictFSSTAnalyzeState> analyze;
	idx_t tuple_count = 0;
	//! How many values have we compressed so far?
	idx_t total_tuple_count = 0;
	idx_t null_count = 0;
	//! Rows since the last new entry; past DICT_STABLE_ROWS the dictionary is treated as complete and encoded even
	//! when the segment is not near a block (the low-cardinality case).
	idx_t rows_since_new = 0;
};

} // namespace dict_fsst
} // namespace duckdb
