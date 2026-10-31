#include "duckdb/common/stack_depth.hpp"

#include "duckdb/common/exception.hpp"

#include <pthread.h>

namespace duckdb {

uintptr_t StackDepth::Limit() {
	if (limit != std::numeric_limits<uintptr_t>::max()) {
		return limit;
	}
	uintptr_t low = 0;
	size_t size = 0;
#if defined(__APPLE__)
	auto self = pthread_self();
	size = pthread_get_stacksize_np(self);
	low = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self)) - size;
#elif defined(__linux__)
	pthread_attr_t attributes;
	if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
		void *address = nullptr;
		if (pthread_attr_getstack(&attributes, &address, &size) != 0) {
			size = 0;
		}
		pthread_attr_destroy(&attributes);
		low = reinterpret_cast<uintptr_t>(address);
	}
#endif
	limit = size == 0 ? 0 : low + MinValue<idx_t>(MaxValue<idx_t>(SAFETY_MARGIN, size / 4), size / 2);
	return limit;
}

void StackDepth::CheckSlow() {
	if (StackPointer() < Limit()) {
		Throw();
	}
}

idx_t StackDepth::Remaining() {
	auto stack_pointer = StackPointer();
	auto stack_limit = Limit();
	return stack_pointer > stack_limit ? stack_pointer - stack_limit : 0;
}

idx_t StackDepth::NestingLimit() {
	return Remaining() / STACK_PER_NESTING_LEVEL;
}

void StackDepth::Throw() {
	throw Exception(Exception::InitializeExtraInfo("STATEMENT_TOO_COMPLEX", QueryLocation()),
	                ExceptionType::OBJECT_SIZE, "stack depth limit exceeded");
}

} // namespace duckdb
