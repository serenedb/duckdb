#include "duckdb/common/stack_depth.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <pthread.h>

namespace duckdb {

namespace {

constexpr idx_t SAFETY_MARGIN = 512 * 1024;

struct ThreadStack {
	uintptr_t low = 0;
	idx_t size = 0;
};

ThreadStack CurrentThreadStack() {
	ThreadStack stack;
#if defined(__APPLE__)
	auto self = pthread_self();
	stack.size = pthread_get_stacksize_np(self);
	stack.low = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self)) - stack.size;
#elif defined(__linux__)
	pthread_attr_t attributes;
	if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
		return stack;
	}
	void *address = nullptr;
	size_t size = 0;
	if (pthread_attr_getstack(&attributes, &address, &size) == 0) {
		stack = {reinterpret_cast<uintptr_t>(address), size};
	}
	pthread_attr_destroy(&attributes);
#endif
	return stack;
}

} // namespace

uintptr_t StackDepth::Limit() {
	if (limit == UNREAD) {
		auto stack = CurrentThreadStack();
		limit = stack.size == 0 ? 0 : stack.low + std::clamp(SAFETY_MARGIN, stack.size / 4, stack.size / 2);
	}
	return limit;
}

void StackDepth::Throw() {
	throw Exception(Exception::InitializeExtraInfo("STATEMENT_TOO_COMPLEX", QueryLocation()),
	                ExceptionType::OBJECT_SIZE, "stack depth limit exceeded");
}

} // namespace duckdb
