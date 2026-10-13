#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"

#include <ctype.h>
#include <algorithm>

namespace duckdb {

// See: https://www.kdnuggets.com/2020/10/optimizing-levenshtein-distance-measuring-text-similarity.html
// And: Iterative 2-row algorithm: https://en.wikipedia.org/wiki/Levenshtein_distance
// Note: A first implementation using the array algorithm version resulted in an error raised by duckdb
// (too muach memory usage)

static constexpr idx_t LEVENSHTEIN_UNTRACKED_BYTES = 64 * 1024;
static constexpr idx_t LEVENSHTEIN_CHECK_CELLS = 1 << 20;

static idx_t LevenshteinDistance(ExpressionState &state, const string_t &txt, const string_t &tgt) {
	auto txt_len = txt.GetSize();
	auto tgt_len = tgt.GetSize();

	// If one string is empty, the distance equals the length of the other string
	if (txt_len == 0) {
		return tgt_len;
	} else if (tgt_len == 0) {
		return txt_len;
	}

	auto txt_str = txt.GetData();
	auto tgt_str = tgt.GetData();

	// Create two working vectors
	const idx_t row_size = tgt_len + 1;
	vector<idx_t> small_rows;
	AllocatedData large_rows;
	idx_t *distances0;
	if (2 * row_size * sizeof(idx_t) <= LEVENSHTEIN_UNTRACKED_BYTES) {
		small_rows.resize(2 * row_size);
		distances0 = small_rows.data();
	} else {
		auto &allocator = state.HasContext() ? BufferAllocator::Get(state.GetContext()) : Allocator::DefaultAllocator();
		large_rows = allocator.Allocate(2 * row_size * sizeof(idx_t));
		distances0 = reinterpret_cast<idx_t *>(large_rows.get());
	}
	auto distances1 = distances0 + row_size;

	idx_t cost_substitution = 0;
	idx_t cost_insertion = 0;
	idx_t cost_deletion = 0;

	// initialize distances0 vector
	// edit distance for an empty txt string is just the number of characters to delete from tgt
	for (idx_t pos_tgt = 0; pos_tgt <= tgt_len; pos_tgt++) {
		distances0[pos_tgt] = pos_tgt;
	}

	idx_t unchecked_cells = 0;
	for (idx_t pos_txt = 0; pos_txt < txt_len; pos_txt++) {
		unchecked_cells += tgt_len;
		if (unchecked_cells >= LEVENSHTEIN_CHECK_CELLS) {
			unchecked_cells = 0;
			state.InterruptCheck();
		}
		// calculate distances1 (current raw distances) from the previous row

		distances1[0] = pos_txt + 1;

		for (idx_t pos_tgt = 0; pos_tgt < tgt_len; pos_tgt++) {
			cost_deletion = distances0[pos_tgt + 1] + 1;
			cost_insertion = distances1[pos_tgt] + 1;
			cost_substitution = distances0[pos_tgt];

			if (txt_str[pos_txt] != tgt_str[pos_tgt]) {
				cost_substitution += 1;
			}

			distances1[pos_tgt + 1] = MinValue(cost_deletion, MinValue(cost_substitution, cost_insertion));
		}
		// since data in distances1 is always invalidated, a swap without copy is more efficient
		std::swap(distances0, distances1);
	}

	return distances0[tgt_len];
}

static void LevenshteinFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vec = args.data[0];
	const auto &tgt_vec = args.data[1];

	BinaryExecutor::Execute<string_t, string_t, int64_t>(str_vec, tgt_vec, result, [&](string_t str, string_t tgt) {
		return UnsafeNumericCast<int64_t>(LevenshteinDistance(state, str, tgt));
	});
}

ScalarFunction LevenshteinFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::BIGINT, LevenshteinFunction);
	fun.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
