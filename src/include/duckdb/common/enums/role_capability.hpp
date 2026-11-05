//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/role_capability.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class RoleCapability : uint32_t {
	None = 0x0,
	ReadAllData = 0x1,
	WriteAllData = 0x2,
	ReadAllSettings = 0x4,
	ReadAllStats = 0x8,
	SignalBackend = 0x10,
	ReadServerFiles = 0x20,
	WriteServerFiles = 0x40,
	Checkpoint = 0x80,
	Maintain = 0x100,
	CreateSubscription = 0x200,
	Superuser = 0x400,
};

constexpr RoleCapability operator|(RoleCapability a, RoleCapability b) {
	return static_cast<RoleCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr RoleCapability operator&(RoleCapability a, RoleCapability b) {
	return static_cast<RoleCapability>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr RoleCapability &operator|=(RoleCapability &a, RoleCapability b) {
	a = a | b;
	return a;
}

} // namespace duckdb
