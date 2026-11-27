#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "test_helpers.hpp"

#include <zlib.h>

using namespace duckdb;

namespace {

constexpr uint8_t GZIP_FLAG_EXTRA = 0x4;
constexpr uint8_t GZIP_FLAG_NAME = 0x8;

class ShortReadFileHandle : public FileHandle {
public:
	ShortReadFileHandle(FileSystem &file_system, const string &path)
	    : FileHandle(file_system, path, FileFlags::FILE_FLAGS_READ) {
	}

	void Close() override {
	}

	idx_t position = 0;
};

class ShortReadFileSystem : public FileSystem {
public:
	ShortReadFileSystem(const vector<data_t> &data_p, idx_t max_read_size_p, optional_idx read_boundary_p)
	    : data(data_p), max_read_size(max_read_size_p), read_boundary(read_boundary_p) {
	}

	string GetName() const override {
		return "ShortReadFileSystem";
	}

	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto &short_handle = handle.Cast<ShortReadFileHandle>();
		if (short_handle.position >= data.size()) {
			return 0;
		}
		auto read_size = MinValue<idx_t>(NumericCast<idx_t>(nr_bytes), max_read_size);
		read_size = MinValue<idx_t>(read_size, data.size() - short_handle.position);
		if (read_boundary.IsValid() && short_handle.position < read_boundary.GetIndex()) {
			read_size = MinValue<idx_t>(read_size, read_boundary.GetIndex() - short_handle.position);
		}
		memcpy(buffer, data.data() + short_handle.position, read_size);
		short_handle.position += read_size;
		return NumericCast<int64_t>(read_size);
	}

	void Seek(FileHandle &handle, idx_t location) override {
		if (location > data.size()) {
			throw InternalException("Attempted to seek past the end of the short-read test file");
		}
		handle.Cast<ShortReadFileHandle>().position = location;
	}

	void Reset(FileHandle &handle) override {
		handle.Cast<ShortReadFileHandle>().position = 0;
	}

	idx_t SeekPosition(FileHandle &handle) override {
		return handle.Cast<ShortReadFileHandle>().position;
	}

	int64_t GetFileSize(FileHandle &handle) override {
		return NumericCast<int64_t>(data.size());
	}

private:
	const vector<data_t> &data;
	idx_t max_read_size;
	optional_idx read_boundary;
};

vector<data_t> ReadGZipTestFile(const string &name) {
	auto file_system = FileSystem::CreateLocal();
	auto path = TestJoinPath(TestGetCurrentDirectory(), "data/csv/test/" + name);
	auto handle = file_system->OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = NumericCast<idx_t>(handle->GetFileSize());
	vector<data_t> result(file_size);
	handle->Read(QueryContext(), result.data(), file_size, 0);
	return result;
}

string ReadCompressedData(const vector<data_t> &compressed_data, idx_t max_read_size,
                          optional_idx read_boundary = optional_idx()) {
	ShortReadFileSystem file_system(compressed_data, max_read_size, read_boundary);
	auto child_handle = make_uniq<ShortReadFileHandle>(file_system, "short-read.gz");
	GZipFileSystem gzip_file_system;
	auto compressed_handle = gzip_file_system.OpenCompressedFile(QueryContext(), std::move(child_handle), false);

	string result;
	data_t buffer[37];
	while (true) {
		auto read_count = compressed_handle->Read(QueryContext(), buffer, sizeof(buffer));
		if (read_count == 0) {
			break;
		}
		result.append(char_ptr_cast(buffer), NumericCast<idx_t>(read_count));
	}
	return result;
}

idx_t FindNextGZipHeader(const vector<data_t> &data) {
	for (idx_t i = GZIP_HEADER_MINSIZE; i + 2 < data.size(); i++) {
		if (data[i] == 0x1F && data[i + 1] == 0x8B && data[i + 2] == GZIP_COMPRESSION_DEFLATE) {
			return i;
		}
	}
	throw InternalException("Could not find the second GZIP member in the test file");
}

idx_t FindGZipHeaderEnd(const vector<data_t> &data, idx_t header_start) {
	auto position = header_start + GZIP_HEADER_MINSIZE;
	if (data[header_start + 3] & GZIP_FLAG_EXTRA) {
		auto xlen = NumericCast<idx_t>((uint8_t)data[position] | (uint8_t)data[position + 1] << 8);
		position += 2 + xlen;
	}
	if (data[header_start + 3] & GZIP_FLAG_NAME) {
		while (position < data.size() && data[position++] != '\0') {
		}
	}
	if (position >= data.size()) {
		throw InternalException("Invalid GZIP header in test file");
	}
	return position;
}

string GZipMember(string input, int level, bool flagged) {
	z_stream stream;
	memset(&stream, 0, sizeof(stream));
	REQUIRE(deflateInit2(&stream, level, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
	char name[] = "member.csv";
	char comment[] = "a comment";
	gz_header header;
	memset(&header, 0, sizeof(header));
	header.text = 1;
	header.hcrc = 1;
	header.name = reinterpret_cast<Bytef *>(name);
	header.comment = reinterpret_cast<Bytef *>(comment);
	if (flagged) {
		REQUIRE(deflateSetHeader(&stream, &header) == Z_OK);
	}
	string output(deflateBound(&stream, NumericCast<uLong>(input.size())) + 64, '\0');
	stream.next_in = data_ptr_cast(&input[0]);
	stream.avail_in = NumericCast<uInt>(input.size());
	stream.next_out = data_ptr_cast(&output[0]);
	stream.avail_out = NumericCast<uInt>(output.size());
	const auto result = deflate(&stream, Z_FINISH);
	output.resize(stream.total_out);
	deflateEnd(&stream);
	REQUIRE(result == Z_STREAM_END);
	return output;
}

} // namespace

TEST_CASE("GZIP reads tolerate short input reads", "[file_system][gzip]") {
	for (auto &file_name : {"concat.gz", "bgzf.gz"}) {
		auto compressed_data = ReadGZipTestFile(file_name);
		auto expected = ReadCompressedData(compressed_data, compressed_data.size());

		for (idx_t read_size = 1; read_size <= GZIP_FOOTER_SIZE + GZIP_HEADER_MINSIZE; read_size++) {
			CAPTURE(file_name, read_size);
			REQUIRE(ReadCompressedData(compressed_data, read_size) == expected);
		}
	}
}

TEST_CASE("GZIP reads tolerate short reads at member boundaries", "[file_system][gzip]") {
	for (auto &file_name : {"concat.gz", "bgzf.gz"}) {
		auto compressed_data = ReadGZipTestFile(file_name);
		auto expected = ReadCompressedData(compressed_data, compressed_data.size());
		auto next_header = FindNextGZipHeader(compressed_data);
		auto header_end = FindGZipHeaderEnd(compressed_data, next_header);

		for (idx_t read_boundary = next_header + 1; read_boundary < header_end; read_boundary++) {
			CAPTURE(file_name, read_boundary);
			REQUIRE(ReadCompressedData(compressed_data, compressed_data.size(), read_boundary) == expected);
		}
	}
}

TEST_CASE("GZIP rejects truncated member boundaries", "[file_system][gzip]") {
	auto compressed_data = ReadGZipTestFile("concat.gz");
	auto next_header = FindNextGZipHeader(compressed_data);
	auto next_header_end = FindGZipHeaderEnd(compressed_data, next_header);

	for (idx_t footer_bytes = 1; footer_bytes < GZIP_FOOTER_SIZE; footer_bytes++) {
		CAPTURE(footer_bytes);
		vector<data_t> truncated(compressed_data.begin(),
		                         compressed_data.begin() + next_header - GZIP_FOOTER_SIZE + footer_bytes);
		REQUIRE_THROWS_AS(ReadCompressedData(truncated, truncated.size()), IOException);
	}
	for (idx_t header_bytes = 1; header_bytes < GZIP_HEADER_MINSIZE; header_bytes++) {
		CAPTURE(header_bytes);
		vector<data_t> truncated(compressed_data.begin(), compressed_data.begin() + next_header + header_bytes);
		REQUIRE_THROWS_AS(ReadCompressedData(truncated, truncated.size()), IOException);
	}

	SECTION("EOF after a complete subsequent member header") {
		vector<data_t> truncated(compressed_data.begin(), compressed_data.begin() + next_header_end);
		REQUIRE_THROWS_AS(ReadCompressedData(truncated, truncated.size()), IOException);
	}
	SECTION("EOF inside a subsequent member") {
		auto compressed_size = NumericCast<idx_t>(compressed_data.size());
		for (auto end : {next_header_end + 1, compressed_size - GZIP_FOOTER_SIZE, compressed_size - 1}) {
			CAPTURE(end);
			vector<data_t> truncated(compressed_data.begin(), compressed_data.begin() + end);
			REQUIRE_THROWS_AS(ReadCompressedData(truncated, truncated.size()), IOException);
		}
	}
	SECTION("EOF inside the first member") {
		auto first_header_end = FindGZipHeaderEnd(compressed_data, 0);
		for (auto end : {first_header_end, next_header - GZIP_FOOTER_SIZE - 1, next_header - GZIP_FOOTER_SIZE}) {
			CAPTURE(end);
			vector<data_t> truncated(compressed_data.begin(), compressed_data.begin() + end);
			REQUIRE_THROWS_AS(ReadCompressedData(truncated, truncated.size()), IOException);
		}
	}
	SECTION("EOF between complete members") {
		vector<data_t> first_member(compressed_data.begin(), compressed_data.begin() + next_header);
		REQUIRE_NOTHROW(ReadCompressedData(first_member, first_member.size()));
	}
}

TEST_CASE("GZIP rejects a header whose extra field is cut short", "[file_system][gzip]") {
	auto compressed_data = ReadGZipTestFile("bgzf.gz");
	REQUIRE((compressed_data[3] & GZIP_FLAG_EXTRA) != 0);
	auto header_end = FindGZipHeaderEnd(compressed_data, 0);
	for (idx_t end : {idx_t(GZIP_HEADER_MINSIZE + 1), header_end - 1}) {
		vector<data_t> truncated(compressed_data.begin(), compressed_data.begin() + end);
		for (idx_t read_size : {idx_t(1), NumericCast<idx_t>(truncated.size())}) {
			CAPTURE(end, read_size);
			REQUIRE_THROWS_AS(ReadCompressedData(truncated, read_size), IOException);
		}
	}
}

TEST_CASE("GZIP string decompression rejects a header-only stream with FNAME", "[file_system][gzip]") {
	const char header[] = {'\x1f', '\x8b', '\x08', '\x08', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00'};
	REQUIRE_THROWS_AS(GZipFileSystem::UncompressGZIPString(header, sizeof(header)), IOException);
}

TEST_CASE("GZIP string decompression reads every member and checks its CRC32", "[file_system][gzip]") {
	const string first = "first member\n";
	const string second = "second member, stored as written\n";
	auto data = GZipMember(first, Z_DEFAULT_COMPRESSION, true) + GZipMember(second, Z_NO_COMPRESSION, false);
	REQUIRE((data[3] & 0x1F) == 0x1B);
	REQUIRE(GZipFileSystem::UncompressGZIPString(data) == first + second);

	auto at = data.find("as written");
	REQUIRE(at != string::npos);
	data[at] ^= 1;
	REQUIRE_THROWS_WITH(GZipFileSystem::UncompressGZIPString(data), Catch::Contains("incorrect data check"));
}

TEST_CASE("GZIP reads of every size return the stream's bytes, into the reader's buffer or through the stream's own",
          "[file_system][gzip]") {
	string text;
	for (idx_t i = 0; i < 400000; i++) {
		text += to_string(i * 7919 % 100003) + (i % 7 == 0 ? "\n" : ",");
	}
	const auto half = text.size() / 2;
	const auto data = GZipMember(text.substr(0, half), Z_DEFAULT_COMPRESSION, false) +
	                  GZipMember(text.substr(half), Z_DEFAULT_COMPRESSION, false);
	const vector<data_t> compressed(data.begin(), data.end());
	ShortReadFileSystem file_system(compressed, compressed.size(), optional_idx());
	auto child_handle = make_uniq<ShortReadFileHandle>(file_system, "sizes.gz");
	GZipFileSystem gzip_file_system;
	auto handle = gzip_file_system.OpenCompressedFile(QueryContext(), std::move(child_handle), false);
	const idx_t sizes[] = {37, 70000, 1, 300000, 65536, 5, 65535};
	vector<data_t> buffer(300000);
	string result;
	for (idx_t i = 0;; i++) {
		const auto read_count = handle->Read(QueryContext(), buffer.data(), sizes[i % 7]);
		if (read_count == 0) {
			break;
		}
		result.append(char_ptr_cast(buffer.data()), NumericCast<idx_t>(read_count));
	}
	REQUIRE(result == text);
}

TEST_CASE("GZIP pieces written in any order form one member", "[file_system][gzip]") {
	GZipFileSystem gzip_file_system;
	auto compressor = gzip_file_system.CreatePieces()->CreateCompressor();

	string long_input;
	for (idx_t i = 0; i < 200000; i++) {
		long_input += to_string(i * 7919 % 100003) + (i % 3 == 0 ? "\n" : ",");
	}
	const vector<string> inputs {"first piece\n", "", long_input, "last\n"};
	vector<CompressedPiece> pieces(inputs.size());
	for (idx_t i = 0; i < inputs.size(); i++) {
		compressor->Compress(const_data_ptr_cast(inputs[i].data()), inputs[i].size(), pieces[i]);
		REQUIRE(pieces[i].input_size == inputs[i].size());
	}
	CompressedPiece split;
	const std::span<const data_t> halves[] = {
	    {const_data_ptr_cast(long_input.data()), 1000},
	    {const_data_ptr_cast(long_input.data()) + 1000, long_input.size() - 1000}};
	compressor->Compress(halves, false, split);
	REQUIRE(split.input_size == long_input.size());
	REQUIRE(split.input_crc == pieces[2].input_crc);

	for (bool last : {false, true}) {
		CAPTURE(last);
		auto frame = gzip_file_system.CreatePieces();
		string file = frame->Head();
		string expected;
		for (auto index : {idx_t(2), idx_t(0), idx_t(3), idx_t(1)}) {
			frame->Append(pieces[index]);
			file.append(const_char_ptr_cast(pieces[index].data.get()), pieces[index].size);
			expected += inputs[index];
		}
		CompressedPiece final_piece;
		compressor->Compress(halves, last, final_piece);
		frame->Append(final_piece);
		file.append(const_char_ptr_cast(final_piece.data.get()), final_piece.size);
		expected += long_input;
		file += frame->Tail();

		z_stream stream;
		memset(&stream, 0, sizeof(stream));
		REQUIRE(inflateInit2(&stream, 16 + MAX_WBITS) == Z_OK);
		string output(expected.size() + 1, '\0');
		stream.next_in = data_ptr_cast(&file[0]);
		stream.avail_in = NumericCast<uInt>(file.size());
		stream.next_out = data_ptr_cast(&output[0]);
		stream.avail_out = NumericCast<uInt>(output.size());
		const auto result = inflate(&stream, Z_FINISH);
		const auto remaining = stream.avail_in;
		output.resize(stream.total_out);
		inflateEnd(&stream);
		REQUIRE(result == Z_STREAM_END);
		REQUIRE(remaining == 0);
		REQUIRE(output == expected);

		const vector<data_t> bytes(file.begin(), file.end());
		REQUIRE(ReadCompressedData(bytes, bytes.size()) == expected);
	}
}
