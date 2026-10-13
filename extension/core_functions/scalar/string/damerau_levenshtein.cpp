#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/vector.hpp"

#include <algorithm>

namespace duckdb {

static constexpr idx_t DAMERAU_UNTRACKED_BYTES = 64 * 1024;
static constexpr idx_t DAMERAU_CHECK_CELLS = 1 << 20;

// Using Lowrance-Wagner (LW) algorithm: https://doi.org/10.1145%2F321879.321880
// Can't calculate as trivial modification to levenshtein algorithm
// as we need to potentially know about earlier in the string
static idx_t DamerauLevenshteinDistance(ExpressionState &state, const string_t &source, const string_t &target) {
	// costs associated with each type of edit, to aid readability
	constexpr uint8_t COST_SUBSTITUTION = 1;
	constexpr uint8_t COST_INSERTION = 1;
	constexpr uint8_t COST_DELETION = 1;
	constexpr uint8_t COST_TRANSPOSITION = 1;
	const auto source_len = source.GetSize();
	const auto target_len = target.GetSize();

	// If one string is empty, the distance equals the length of the other string
	// either through target_len insertions
	// or source_len deletions
	if (source_len == 0) {
		return target_len * COST_INSERTION;
	} else if (target_len == 0) {
		return source_len * COST_DELETION;
	}

	const auto source_str = source.GetData();
	const auto target_str = target.GetData();

	// larger than the largest possible value:
	const auto inf = source_len * COST_DELETION + target_len * COST_INSERTION + 1;
	// minimum edit distance from prefix of source string to prefix of target string
	// same object as H in LW paper (with indices offset by 1)
	const idx_t row_size = target_len + 2;
	if (source_len + 2 > NumericLimits<idx_t>::Maximum() / sizeof(idx_t) / row_size) {
		throw InvalidInputException("Strings of %llu and %llu bytes are too long for damerau_levenshtein", source_len,
		                            target_len);
	}
	const idx_t cell_count = (source_len + 2) * row_size;
	vector<idx_t> small_distance;
	AllocatedData large_distance;
	idx_t *distance;
	if (cell_count * sizeof(idx_t) <= DAMERAU_UNTRACKED_BYTES) {
		small_distance.resize(cell_count);
		distance = small_distance.data();
	} else {
		auto &allocator = state.HasContext() ? BufferAllocator::Get(state.GetContext()) : Allocator::DefaultAllocator();
		large_distance = allocator.Allocate(cell_count * sizeof(idx_t));
		distance = reinterpret_cast<idx_t *>(large_distance.get());
	}
	auto cell = [&](idx_t source_pos, idx_t target_pos) -> idx_t & {
		return distance[source_pos * row_size + target_pos];
	};
	std::fill_n(distance, row_size, inf);
	cell(1, 0) = inf;
	// keeps track of the largest string indices of source string matching each character
	// same as DA in LW paper
	idx_t largest_source_chr_matching[256] = {};

	// initialise row/column corresponding to zero-length strings
	// partial string -> empty requires a deletion for each character
	for (idx_t source_idx = 0; source_idx <= source_len; source_idx++) {
		cell(source_idx + 1, 1) = source_idx * COST_DELETION;
	}
	// and empty -> partial string means simply inserting characters
	for (idx_t target_idx = 1; target_idx <= target_len; target_idx++) {
		cell(1, target_idx + 1) = target_idx * COST_INSERTION;
	}
	// loop through string indices - these are offset by 2 from distance indices
	idx_t unchecked_cells = 0;
	for (idx_t source_idx = 0; source_idx < source_len; source_idx++) {
		unchecked_cells += target_len;
		if (unchecked_cells >= DAMERAU_CHECK_CELLS) {
			unchecked_cells = 0;
			state.InterruptCheck();
		}
		cell(source_idx + 2, 0) = inf;
		// keeps track of the largest string indices of target string matching current source character
		// same as DB in LW paper
		idx_t largest_target_chr_matching;
		largest_target_chr_matching = 0;
		for (idx_t target_idx = 0; target_idx < target_len; target_idx++) {
			// correspond to i1 and j1 in LW paper respectively
			idx_t largest_source_chr_matching_target;
			idx_t largest_target_chr_matching_source;
			// cost associated to diagnanl shift in distance matrix
			// corresponds to d in LW paper
			uint8_t cost_diagonal_shift;
			largest_source_chr_matching_target =
			    largest_source_chr_matching[static_cast<unsigned char>(target_str[target_idx])];
			largest_target_chr_matching_source = largest_target_chr_matching;
			// if characters match, diagonal move costs nothing and we update our largest target index
			// otherwise move is substitution and costs as such
			if (source_str[source_idx] == target_str[target_idx]) {
				cost_diagonal_shift = 0;
				largest_target_chr_matching = target_idx + 1;
			} else {
				cost_diagonal_shift = COST_SUBSTITUTION;
			}
			cell(source_idx + 2, target_idx + 2) = MinValue(
			    cell(source_idx + 1, target_idx + 1) + cost_diagonal_shift,
			    MinValue(cell(source_idx + 2, target_idx + 1) + COST_INSERTION,
			             MinValue(cell(source_idx + 1, target_idx + 2) + COST_DELETION,
			                      cell(largest_source_chr_matching_target, largest_target_chr_matching_source) +
			                          (source_idx - largest_source_chr_matching_target) * COST_DELETION +
			                          COST_TRANSPOSITION +
			                          (target_idx - largest_target_chr_matching_source) * COST_INSERTION)));
		}
		largest_source_chr_matching[static_cast<unsigned char>(source_str[source_idx])] = source_idx + 1;
	}
	return cell(source_len + 1, target_len + 1);
}

static void DamerauLevenshteinFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &source_vec = args.data[0];
	const auto &target_vec = args.data[1];

	BinaryExecutor::Execute<string_t, string_t, int64_t>(
	    source_vec, target_vec, result, [&](string_t source, string_t target) {
		    return UnsafeNumericCast<int64_t>(DamerauLevenshteinDistance(state, source, target));
	    });
}

ScalarFunction DamerauLevenshteinFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::BIGINT, DamerauLevenshteinFunction);
	fun.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
