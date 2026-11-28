#include "duckdb/formats/csv_encoder.hpp"

#include "duckdb/common/read_within_page.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/formats/csv_simd.hpp"

#include <bit>

namespace duckdb {

namespace {

bool Escaped(char c, char quote, char escape) {
	return escape != '\0' && (c == quote || c == escape);
}

enum CsvFieldQuoting : uint8_t {
	FIELD_RAW = 0,
	FIELD_QUOTED = 1,
	FIELD_ESCAPED = 2,
	FIELD_NULL = 3,
};

class CsvClassifier {
public:
	explicit CsvClassifier(const CsvEncoding &encoding_p) : encoding(encoding_p) {
	}

	CsvFieldQuoting Classify(const char *data, idx_t size, bool force, uint32_t &field_size) const {
		const std::string_view field(data, size);
		if (encoding.quote == '\0' || (!force && !encoding.specials.Quote(field, encoding.null_str))) {
			field_size = NumericCast<uint32_t>(size);
			return FIELD_RAW;
		}
		const idx_t escapes = CsvEscapes(field, encoding.quote, encoding.escape);
		field_size = NumericCast<uint32_t>(size + 2 + escapes);
		return escapes ? FIELD_ESCAPED : FIELD_QUOTED;
	}

	char *Write(char *out, const char *data, idx_t size, CsvFieldQuoting quoting) const {
		switch (quoting) {
		case FIELD_NULL:
			memcpy(out, encoding.null_str.data(), encoding.null_str.size());
			return out + encoding.null_str.size();
		case FIELD_RAW:
			memcpy(out, data, size);
			return out + size;
		case FIELD_QUOTED:
			*out++ = encoding.quote;
			memcpy(out, data, size);
			out += size;
			*out++ = encoding.quote;
			return out;
		case FIELD_ESCAPED:
			return out + CsvWriteQuoted(out, std::string_view(data, size), encoding.quote, encoding.escape);
		}
		return out;
	}

private:
	const CsvEncoding &encoding;
};

bool ForceQuote(const CsvEncoding &encoding, idx_t column) {
	return column < encoding.force_quote.size() && encoding.force_quote[column];
}

} // namespace

void CsvSpecials::Add(char byte) {
	const auto value = static_cast<uint8_t>(byte);
	auto &group = high[value >> 4];
	if (!group) {
		if (groups == 8) {
			throw InternalException("CsvSpecials holds bytes of at most eight different high nibbles");
		}
		group = static_cast<uint8_t>(1U << groups++);
	}
	low[value & 0x0F] |= group;
}

idx_t CsvSpecials::Find(std::string_view field, idx_t from) const {
	idx_t i = from;
	for (; i + CSV_BLOCK_BYTES <= field.size(); i += CSV_BLOCK_BYTES) {
		if (const uint64_t bits = CsvVector(field.data() + i).Lookup(low, high)) {
			return i + NumericCast<idx_t>(std::countr_zero(bits));
		}
	}
	if (i >= field.size()) {
		return field.size();
	}
	const idx_t rest = field.size() - i;
	const char *tail = field.data() + i;
	char padded[CSV_BLOCK_BYTES];
	if (!ReadableWithinPage(tail, CSV_BLOCK_BYTES)) {
		memset(padded, 0, CSV_BLOCK_BYTES);
		memcpy(padded, tail, rest);
		tail = padded;
	}
	const uint64_t bits = CsvVector(tail).Lookup(low, high) & ((uint64_t(1) << rest) - 1);
	return bits ? i + NumericCast<idx_t>(std::countr_zero(bits)) : field.size();
}

idx_t CsvEscapes(std::string_view field, char quote, char escape) {
	idx_t escapes = 0;
	for (const char c : field) {
		escapes += Escaped(c, quote, escape);
	}
	return escapes;
}

idx_t CsvWriteQuoted(char *out, std::string_view field, char quote, char escape) {
	char *begin = out;
	*out++ = quote;
	idx_t from = 0;
	for (idx_t i = 0; i < field.size(); i++) {
		if (Escaped(field[i], quote, escape)) {
			memcpy(out, field.data() + from, i - from);
			out += i - from;
			*out++ = escape;
			from = i;
		}
	}
	memcpy(out, field.data() + from, field.size() - from);
	out += field.size() - from;
	*out++ = quote;
	return NumericCast<idx_t>(out - begin);
}

idx_t CsvQuoteInPlace(char *data, idx_t size, char quote, char escape) {
	const idx_t escapes = CsvEscapes(std::string_view(data, size), quote, escape);
	char *in = data + size;
	char *out = in + escapes + 1;
	*out = quote;
	while (in != data) {
		const char c = *--in;
		*--out = c;
		if (Escaped(c, quote, escape)) {
			*--out = escape;
		}
	}
	*--out = quote;
	return size + escapes + 2;
}

void CsvEncodeChunk(const CsvEncoding &encoding, DataChunk &input, MemoryStream &out, bool &written_anything,
                    CsvEncodeScratch &scratch) {
	const idx_t count = input.size();
	if (count == 0) {
		return;
	}
	const idx_t columns = input.ColumnCount();
	const CsvClassifier classifier(encoding);
	scratch.sizes.resize(columns * count);
	scratch.quoting.resize(columns * count);
	auto &cursors = scratch.cursors;
	cursors.assign(count, 0);
	for (idx_t column = 0; column < columns; column++) {
		const bool force = ForceQuote(encoding, column);
		auto values = input.data[column].Values<string_t>();
		auto sizes = scratch.sizes.data() + column * count;
		auto quoting = scratch.quoting.data() + column * count;
		for (idx_t row = 0; row < count; row++) {
			auto entry = values[row];
			if (!entry.IsValid()) {
				sizes[row] = NumericCast<uint32_t>(encoding.null_str.size());
				quoting[row] = FIELD_NULL;
			} else {
				auto value = entry.GetValue();
				quoting[row] = classifier.Classify(value.GetData(), value.GetSize(), force, sizes[row]);
			}
			cursors[row] += sizes[row];
		}
	}
	const idx_t newline_size = encoding.newline.size();
	const idx_t delimiters = columns > 0 ? columns - 1 : 0;
	idx_t total = 0;
	for (idx_t row = 0; row < count; row++) {
		const bool before = encoding.newline_before && (row > 0 || written_anything);
		const idx_t row_size = cursors[row] + delimiters + newline_size;
		cursors[row] = total + (before ? newline_size : 0);
		total += row_size - (encoding.newline_before && !before ? newline_size : 0);
	}
	auto base = char_ptr_cast(out.Alloc(total));
	for (idx_t row = 0; row < count; row++) {
		if (encoding.newline_before && (row > 0 || written_anything)) {
			memcpy(base + cursors[row] - newline_size, encoding.newline.data(), newline_size);
		}
	}
	for (idx_t column = 0; column < columns; column++) {
		auto values = input.data[column].Values<string_t>();
		auto quoting = scratch.quoting.data() + column * count;
		for (idx_t row = 0; row < count; row++) {
			char *dst = base + cursors[row];
			if (column > 0) {
				*dst++ = encoding.delimiter;
			}
			const auto field = static_cast<CsvFieldQuoting>(quoting[row]);
			if (field == FIELD_NULL) {
				dst = classifier.Write(dst, nullptr, 0, field);
			} else {
				auto value = values[row].GetValue();
				dst = classifier.Write(dst, value.GetData(), value.GetSize(), field);
			}
			cursors[row] = NumericCast<idx_t>(dst - base);
		}
	}
	if (!encoding.newline_before) {
		for (idx_t row = 0; row < count; row++) {
			memcpy(base + cursors[row], encoding.newline.data(), newline_size);
		}
	}
	written_anything = true;
}

void CsvEncodeHeader(const CsvEncoding &encoding, const vector<Identifier> &names, MemoryStream &out) {
	const CsvClassifier classifier(encoding);
	string line;
	for (idx_t i = 0; i < names.size(); i++) {
		if (i > 0) {
			line += encoding.delimiter;
		}
		auto &name = names[i];
		uint32_t size = 0;
		const auto quoting = classifier.Classify(name.c_str(), name.size(), ForceQuote(encoding, i), size);
		const idx_t offset = line.size();
		line.resize(offset + size);
		classifier.Write(&line[offset], name.c_str(), name.size(), quoting);
	}
	out.WriteData(const_data_ptr_cast(line.data()), line.size());
}

} // namespace duckdb
