#include "duckdb/main/valid_checker.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

ValidChecker::ValidChecker(DatabaseInstance &db, Scope scope) : is_invalidated(false), db(db), scope(scope) {
}

void ValidChecker::Invalidate(string error) {
	lock_guard<mutex> l(invalidate_lock);
	is_invalidated = true;
	invalidated_msg = std::move(error);
}

bool ValidChecker::IsInvalidated() {
	if (!is_invalidated) {
		return false;
	}
	// disable_database_invalidation only suppresses DB-level invalidation; the
	// per-transaction aborted bit must still be honored so PG-style
	// "aborted transaction" semantics can work independently.
	return !(scope == Scope::DATABASE && Settings::Get<DisableDatabaseInvalidationSetting>(db));
}

string ValidChecker::InvalidatedMessage() {
	lock_guard<mutex> l(invalidate_lock);
	return invalidated_msg;
}

} // namespace duckdb
