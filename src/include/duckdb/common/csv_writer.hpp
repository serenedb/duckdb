//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/csv_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/formats/csv_encoder.hpp"

namespace duckdb {
class CompressedPiece;
class CopyFileWriter;
class MemoryStream;
class PieceCompressor;

enum class CSVNewLineMode {
	//! Newlines are written before writing out a new csv line. Ensures we don't write an empty line at the end of
	//! a csv file. Uses CSVWriterState::written_anything to keep track of when to insert newlines
	WRITE_BEFORE = 0,
	//! Newlines are written after every line. This is cleanest in stdout, where lines are expected to end with a
	//! newline
	WRITE_AFTER = 1
};

struct CSVWriterOptions {
	CSVWriterOptions(const string &delim, const char &quote, const string &write_newline, char comment_char = '#');
	explicit CSVWriterOptions(CSVReaderOptions &options);

	//! The newline string to write
	string newline = "\n";
	//! The size of the CSV file (in bytes) that we buffer before we flush it to disk
	idx_t flush_size = 4096ULL * 8ULL;
	//! The bytes that make the CSV file quote a field containing them
	CsvSpecials specials;
	//! How to write newlines
	CSVNewLineMode newline_writing_mode = CSVNewLineMode::WRITE_BEFORE;
};

struct CSVWriterState {
	CSVWriterState(ClientContext &context, idx_t flush_size);
	CSVWriterState(DatabaseInstance &db, idx_t flush_size);
	CSVWriterState();
	~CSVWriterState();

	void Reset() {
		stream->Rewind();
		written_anything = false;
	}

	idx_t flush_size;
	unique_ptr<MemoryStream> stream;
	bool written_anything = false;

	bool require_manual_flush = false;
	CsvEncodeScratch scratch;
	unique_ptr<PieceCompressor> compressor;
	unique_ptr<CompressedPiece> piece;
};

class CSVWriter {
public:
	//! Create a CSVWriter that writes to a (non-owned) WriteStream
	CSVWriter(WriteStream &stream, vector<Identifier> name_list, bool shared = true);
	CSVWriter(CSVReaderOptions &options, CopyFileWriter &copy_file, bool shared = true);

	//! Create a CSVWriter that writes to a file
	CSVWriter(CSVReaderOptions &options, FileSystem &fs, const string &file_path, FileCompressionType compression,
	          QueryContext context = QueryContext(), bool shared = true);
	~CSVWriter();

	//! Writes header and prefix if necessary
	void Initialize(bool force = false);

	//! Writes the raw string directly into the output stream
	void WriteRawString(const string &data);
	//! Writes the header directly into the output stream
	void WriteHeader();
	//! Write the Raw String, using the local_state
	void WriteRawString(const string &prefix, CSVWriterState &local_state);
	//! Write a chunk of VARCHAR vectors to the CSV file (any casts are the responsibility of caller)
	void WriteChunk(DataChunk &input, CSVWriterState &local_state);
	//! (Non-shared only) variant of WriteChunk
	void WriteChunk(DataChunk &input);

	//! Flushes all data in the local write state
	void Flush(CSVWriterState &local_state);
	void Compress(CSVWriterState &local_state);
	//! (Non-shared only) variant of Flush
	void Flush();

	//! Resets the state of the writer. Warning: the file_writer is not reset
	void Reset(optional_ptr<CSVWriterState> local_state);

	//! Closes the writer, optionally writes a postfix
	void Close();

	vector<unique_ptr<Expression>> string_casts;

	idx_t BytesWritten();

	//! BytesWritten + OriginalSize;
	idx_t FileSize();

	bool WrittenAnything() {
		return written_anything;
	}
	void SetWrittenAnything(bool val) {
		if (shared) {
			lock_guard<mutex> guard(lock);
			written_anything = val;
		} else {
			written_anything = val;
		}
	}

	CSVReaderOptions options;
	CSVWriterOptions writer_options;

protected:
	void FlushInternal(CSVWriterState &local_state);
	void ResetInternal(optional_ptr<CSVWriterState> local_state);

	//! If we've written any rows yet, allows us to prevent a trailing comma when writing JSON ARRAY
	bool written_anything = false;

	//! (optional) The owned file writer of this CSVWriter
	unique_ptr<BufferedFileWriter> file_writer;
	optional_ptr<CopyFileWriter> copy_file;

	//! The WriteStream to write the CSV data to
	optional_ptr<WriteStream> write_stream;

	idx_t bytes_written = 0;

	bool should_initialize;

	mutex lock;
	bool shared;

	unique_ptr<CSVWriterState> global_write_state;

	CsvEncoding Encoding() const;
};

} // namespace duckdb
