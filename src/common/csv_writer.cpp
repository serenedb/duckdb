#include "duckdb/common/csv_writer.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

static string TransformNewLine(string new_line) {
	new_line = StringUtil::Replace(new_line, "\\r", "\r");
	return StringUtil::Replace(new_line, "\\n", "\n");
}

CSVWriterState::CSVWriterState()
    : flush_size(MemoryStream::DEFAULT_INITIAL_CAPACITY), stream(make_uniq<MemoryStream>()) {
}

CSVWriterState::CSVWriterState(ClientContext &context, idx_t flush_size_p)
    : flush_size(flush_size_p), stream(make_uniq<MemoryStream>(Allocator::Get(context), flush_size)) {
}

CSVWriterState::CSVWriterState(DatabaseInstance &db, idx_t flush_size_p)
    : flush_size(flush_size_p), stream(make_uniq<MemoryStream>(BufferAllocator::Get(db), flush_size)) {
}

CSVWriterState::~CSVWriterState() {
	if (stream && !Exception::UncaughtException()) {
		// Ensure we don't accidentally destroy unflushed data
		D_ASSERT(stream->GetPosition() == 0);
	}
}

CSVWriterOptions::CSVWriterOptions(const string &delim, const char &quote, const string &write_newline,
                                   char comment_char) {
	specials.Add('\n');
	specials.Add('\r');
	// quote values containing the comment char so they are not read back as comments (see #17744)
	if (comment_char != '\0') {
		specials.Add(comment_char);
	}
	specials.Add(delim[0]);
	specials.Add(quote);

	if (!write_newline.empty()) {
		newline = TransformNewLine(write_newline);
	}
}

// The comment char to protect on write. Defaults to '#' when unset, since the sniffer detects '#'
// comments by default; an explicit comment (including the empty '\0' from `comment ''`) is honored.
static char GetWriteCommentChar(CSVReaderOptions &options) {
	auto &comment = options.dialect_options.state_machine_options.comment;
	return comment.IsSetByUser() ? comment.GetValue() : '#';
}

CSVWriterOptions::CSVWriterOptions(CSVReaderOptions &options)
    : CSVWriterOptions(options.dialect_options.state_machine_options.delimiter.GetValue(),
                       options.dialect_options.state_machine_options.quote.GetValue(), options.write_newline) {
}

CSVWriter::CSVWriter(WriteStream &stream, vector<Identifier> name_list, bool shared)
    : writer_options(options.dialect_options.state_machine_options.delimiter.GetValue(),
                     options.dialect_options.state_machine_options.quote.GetValue(), options.write_newline),
      write_stream(stream), should_initialize(true), shared(shared) {
	auto size = name_list.size();
	options.name_list = std::move(name_list);
	options.force_quote.resize(size, false);

	if (!shared) {
		global_write_state = make_uniq<CSVWriterState>();
	}
}

CSVWriter::CSVWriter(CSVReaderOptions &options_p, AsyncFileWriter &file_writer_p, bool shared)
    : options(options_p), writer_options(options.dialect_options.state_machine_options.delimiter.GetValue(),
                                         options.dialect_options.state_machine_options.quote.GetValue(),
                                         options.write_newline, GetWriteCommentChar(options)),
      async_file_writer(file_writer_p), write_stream(file_writer_p), should_initialize(true), shared(shared) {
	if (!shared) {
		global_write_state = make_uniq<CSVWriterState>();
	}
}

CSVWriter::CSVWriter(CSVReaderOptions &options_p, FileSystem &fs, const string &file_path,
                     FileCompressionType compression, QueryContext context, bool shared)
    : options(options_p), writer_options(options.dialect_options.state_machine_options.delimiter.GetValue(),
                                         options.dialect_options.state_machine_options.quote.GetValue(),
                                         options.write_newline, GetWriteCommentChar(options)),
      file_writer(make_uniq<BufferedFileWriter>(fs, file_path,
                                                FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
                                                    FileLockType::WRITE_LOCK | std::move(compression),
                                                context)),
      write_stream(*file_writer), should_initialize(true), shared(shared) {
	if (!shared) {
		global_write_state = make_uniq<CSVWriterState>();
	}
}

CSVWriter::~CSVWriter() = default;

void CSVWriter::Initialize(bool force) {
	if (!force && !should_initialize) {
		return;
	}

	if (!options.prefix.empty()) {
		WriteRawString(options.prefix);
	}

	if (!(options.dialect_options.header.IsSetByUser() && !options.dialect_options.header.GetValue())) {
		WriteHeader();
	}

	should_initialize = false;
}

CsvEncoding CSVWriter::Encoding() const {
	auto &dialect = options.dialect_options.state_machine_options;
	return {dialect.delimiter.GetValue()[0],
	        dialect.quote.GetValue(),
	        dialect.escape.GetValue(),
	        options.null_str[0],
	        options.force_quote,
	        writer_options.specials,
	        writer_options.newline,
	        writer_options.newline_writing_mode == CSVNewLineMode::WRITE_BEFORE};
}

void CSVWriter::WriteChunk(DataChunk &input, CSVWriterState &local_state) {
	D_ASSERT(options.null_str.size() == 1);
	CsvEncodeChunk(Encoding(), input, *local_state.stream, local_state.written_anything, local_state.scratch);

	if (!local_state.require_manual_flush && local_state.stream->GetPosition() >= local_state.flush_size) {
		Flush(local_state);
	}
}

void CSVWriter::WriteChunk(DataChunk &input) {
	// Method intended for non-shared use only
	D_ASSERT(!shared);

	WriteChunk(input, *global_write_state);
}

void CSVWriter::WriteRawString(const string &raw_string) {
	if (shared) {
		lock_guard<mutex> flock(lock);
		bytes_written += raw_string.size();
		write_stream.WriteData(const_data_ptr_cast(raw_string.c_str()), raw_string.size());
	} else {
		bytes_written += raw_string.size();
		write_stream.WriteData(const_data_ptr_cast(raw_string.c_str()), raw_string.size());
	}
}

void CSVWriter::WriteRawString(const string &prefix, CSVWriterState &local_state) {
	local_state.stream->WriteData(const_data_ptr_cast(prefix.c_str()), prefix.size());

	if (!local_state.require_manual_flush && local_state.stream->GetPosition() >= writer_options.flush_size) {
		Flush(local_state);
	}
}

void CSVWriter::WriteHeader() {
	CSVWriterState state;
	CsvEncodeHeader(Encoding(), options.name_list, *state.stream);
	state.written_anything = true;
	Flush(state);
}

void CSVWriter::Flush(CSVWriterState &local_state) {
	if (shared) {
		lock_guard<mutex> flock(lock);
		FlushInternal(local_state);
		if (async_file_writer) {
			async_file_writer->ApplyBackpressure();
		}
	} else {
		FlushInternal(local_state);
		if (async_file_writer) {
			async_file_writer->ApplyBackpressure();
		}
	}
}

void CSVWriter::Flush() {
	// Method intended for non-shared use only
	D_ASSERT(!shared);
	Flush(*global_write_state);
}

void CSVWriter::Reset(optional_ptr<CSVWriterState> local_state) {
	if (shared) {
		lock_guard<mutex> flock(lock);
		ResetInternal(local_state);
	} else {
		ResetInternal(local_state);
	}
}

void CSVWriter::Close() {
	if (shared) {
		lock_guard<mutex> flock(lock);
		if (file_writer) {
			file_writer->Close();
		}
	} else {
		if (file_writer) {
			file_writer->Close();
		}
	}
}

void CSVWriter::FlushInternal(CSVWriterState &local_state) {
	if (!local_state.written_anything) {
		return;
	}

	if (written_anything && writer_options.newline_writing_mode == CSVNewLineMode::WRITE_BEFORE) {
		write_stream.WriteData(const_data_ptr_cast(writer_options.newline.c_str()), writer_options.newline.size());
		bytes_written += writer_options.newline.size();
	}

	written_anything = true;
	bytes_written += local_state.stream->GetPosition();
	write_stream.WriteData(local_state.stream->GetData(), local_state.stream->GetPosition());

	local_state.Reset();
}

void CSVWriter::ResetInternal(optional_ptr<CSVWriterState> local_state) {
	if (local_state) {
		local_state->Reset();
	}

	written_anything = false;
	bytes_written = 0;
}

idx_t CSVWriter::BytesWritten() {
	if (shared) {
		lock_guard<mutex> flock(lock);
		return bytes_written;
	}
	return bytes_written;
}

static idx_t GetFileSize(unique_ptr<BufferedFileWriter> &file_writer, idx_t &bytes_written) {
	if (file_writer) {
		return file_writer->GetFileSize();
	}
	return bytes_written;
}

idx_t CSVWriter::FileSize() {
	if (shared) {
		lock_guard<mutex> flock(lock);
		if (async_file_writer) {
			return async_file_writer->GetPhysicalFileSize();
		}
		return GetFileSize(file_writer, bytes_written);
	}
	if (async_file_writer) {
		return async_file_writer->GetPhysicalFileSize();
	}
	return GetFileSize(file_writer, bytes_written);
}

} // namespace duckdb
