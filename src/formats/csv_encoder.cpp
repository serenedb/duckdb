#include "duckdb/formats/csv_encoder.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

namespace {

enum CsvFieldQuoting : uint8_t {
	FIELD_RAW = 0,
	FIELD_QUOTED = 1,
	FIELD_ESCAPED = 2,
	FIELD_NULL = 3,
};

class CsvClassifier {
public:
	explicit CsvClassifier(const CsvEncoding &encoding_p) : encoding(encoding_p) {
		for (idx_t byte = 0; byte < 256; byte++) {
			requires_quotes[byte] = byte < encoding.requires_quotes.size() && encoding.requires_quotes[byte];
		}
	}

	CsvFieldQuoting Classify(const char *data, idx_t size, bool force, uint32_t &field_size) const {
		if (encoding.quote == '\0' || (!force && !RequiresQuotes(data, size))) {
			field_size = NumericCast<uint32_t>(size);
			return FIELD_RAW;
		}
		idx_t escapes = 0;
		if (encoding.escape != '\0') {
			for (idx_t i = 0; i < size; i++) {
				escapes += data[i] == encoding.quote || data[i] == encoding.escape;
			}
		}
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
			*out++ = encoding.quote;
			for (idx_t i = 0; i < size; i++) {
				if (data[i] == encoding.quote || data[i] == encoding.escape) {
					*out++ = encoding.escape;
				}
				*out++ = data[i];
			}
			*out++ = encoding.quote;
			return out;
		}
		return out;
	}

private:
	bool RequiresQuotes(const char *data, idx_t size) const {
		auto &null_str = encoding.null_str;
		if (size == null_str.size() && memcmp(data, null_str.data(), size) == 0) {
			return true;
		}
		const auto bytes = const_data_ptr_cast(data);
		for (idx_t i = 0; i < size; i++) {
			if (requires_quotes[bytes[i]]) {
				return true;
			}
		}
		return false;
	}

	const CsvEncoding &encoding;
	bool requires_quotes[256];
};

bool ForceQuote(const CsvEncoding &encoding, idx_t column) {
	return column < encoding.force_quote.size() && encoding.force_quote[column];
}

} // namespace

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
