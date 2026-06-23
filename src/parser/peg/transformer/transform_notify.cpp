#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

// notify.gram — LISTEN / NOTIFY / UNLISTEN are PostgreSQL pub/sub commands.
// SereneDB has no notification engine yet, so they parse cleanly (to give a
// clear error instead of a confusing "syntax error at or near") and the
// transform throws. These are hand-registered (kept out of grammar_types.yml)
// so the generator does not also emit a wrapper for them.
unique_ptr<SQLStatement> PEGTransformerFactory::TransformListenStatement(PEGTransformer &, ParseResult &) {
	throw NotImplementedException("LISTEN is not supported by SereneDB yet");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformNotifyStatement(PEGTransformer &, ParseResult &) {
	throw NotImplementedException("NOTIFY is not supported by SereneDB yet");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformUnlistenStatement(PEGTransformer &, ParseResult &) {
	throw NotImplementedException("UNLISTEN is not supported by SereneDB yet");
}

} // namespace duckdb
