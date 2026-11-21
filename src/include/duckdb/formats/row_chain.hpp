//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_chain.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "absl/cleanup/cleanup.h"
#include "duckdb/common/array.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

template <class START, class ENTRY, idx_t ORIGINS>
class RowChain {
public:
	using Transfers = array<ENTRY, ORIGINS>;

	void Assign(idx_t slots, const START &first) {
		transfers.assign(slots, Transfers());
		computing.assign(slots, 0);
		starts.assign(slots + 1, START());
		starts[0] = first;
		resolved = 0;
	}

	void Grow(idx_t count) {
		transfers.resize(count);
		computing.resize(count, 0);
		starts.resize(count + 1);
	}

	bool Publish(idx_t range, uint8_t origin, const ENTRY &entry, idx_t range_count) {
		auto &slot = transfers[range][origin];
		if (!slot.computed) {
			slot = entry;
		}
		const idx_t known = MinValue<idx_t>(range_count, transfers.size());
		const idx_t before = resolved;
		while (resolved < known) {
			const auto &current = starts[resolved];
			const auto &found = transfers[resolved][current.Origin()];
			if (!found.computed) {
				break;
			}
			starts[resolved + 1] = current.Next(found);
			resolved++;
		}
		return resolved != before;
	}

	template <class COMPUTE, class PUBLISH>
	START StartState(mutex &lock, idx_t range, COMPUTE &&compute, PUBLISH &&publish) {
		while (true) {
			idx_t missing;
			uint8_t origin;
			{
				const absl::MutexLock guard(&lock);
				const auto ready = [&]() {
					return resolved >= range || computing[resolved] == 0;
				};
				lock.Await(absl::Condition(&ready));
				if (resolved >= range) {
					return starts[range];
				}
				missing = resolved;
				origin = starts[missing].Origin();
				computing[missing]++;
			}
			ENTRY entry;
			absl::Cleanup stop = [&] {
				const absl::MutexLock guard(&lock);
				computing[missing]--;
				publish(missing, origin, entry);
			};
			entry = compute(missing, origin);
		}
	}

public:
	vector<Transfers> transfers;
	vector<uint32_t> computing;
	vector<START> starts;
	idx_t resolved = 0;
};

} // namespace duckdb
