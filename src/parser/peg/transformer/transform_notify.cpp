#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

// notify.gram — LISTEN / NOTIFY / UNLISTEN are PostgreSQL pub/sub commands.
// SereneDB has no notification engine yet, so they parse cleanly (to give a
// clear error instead of a confusing "syntax error at or near") and the
// transform throws.
unique_ptr<SQLStatement> PEGTransformerFactory::TransformListenStatement(PEGTransformer &transformer,
                                                                         ParseResult &parse_result) {
	throw NotImplementedException("LISTEN is not supported by SereneDB yet");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformNotifyStatement(PEGTransformer &transformer,
                                                                         ParseResult &parse_result) {
	throw NotImplementedException("NOTIFY is not supported by SereneDB yet");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformUnlistenStatement(PEGTransformer &transformer,
                                                                           ParseResult &parse_result) {
	throw NotImplementedException("UNLISTEN is not supported by SereneDB yet");
}

} // namespace duckdb
