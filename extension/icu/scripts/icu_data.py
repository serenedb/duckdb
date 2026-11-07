import struct


class DataPackage:
    def __init__(self, data):
        header_size = struct.unpack_from("<H", data, 0)[0]
        count = struct.unpack_from("<I", data, header_size)[0]
        entries = []
        for i in range(count):
            name_offset, data_offset = struct.unpack_from("<II", data, header_size + 4 + 8 * i)
            entries.append((name_offset, data_offset))
        self.items = {}
        base = header_size
        for i, (name_offset, data_offset) in enumerate(entries):
            end = data.index(b"\0", base + name_offset)
            name = data[base + name_offset : end].decode("ascii")
            name = name.split("/", 1)[1] if "/" in name else name
            limit = entries[i + 1][1] if i + 1 < count else len(data) - base
            self.items[name] = data[base + data_offset : base + limit]

    @staticmethod
    def open(path):
        with open(path, "rb") as handle:
            return DataPackage(handle.read())

    def names(self, prefix=""):
        return sorted(name for name in self.items if name.startswith(prefix))

    def payload(self, name):
        item = self.items[name]
        header_size = struct.unpack_from("<H", item, 0)[0]
        data_format = item[12:16]
        format_version = tuple(item[16:20])
        return item[header_size:], data_format, format_version


class CodePointTrie:
    SHIFT_3 = 4
    SHIFT_2 = 5 + SHIFT_3
    SHIFT_1 = 5 + SHIFT_2
    INDEX_2_MASK = (1 << (SHIFT_1 - SHIFT_2)) - 1
    INDEX_3_MASK = (1 << (SHIFT_2 - SHIFT_3)) - 1
    SMALL_DATA_MASK = (1 << SHIFT_3) - 1
    FAST_SHIFT = 6
    FAST_DATA_MASK = (1 << FAST_SHIFT) - 1
    BMP_INDEX_LENGTH = 0x10000 >> FAST_SHIFT
    SMALL_INDEX_LENGTH = 0x1000 >> FAST_SHIFT
    OMITTED_BMP_INDEX_1_LENGTH = 0x10000 >> SHIFT_1

    def __init__(self, data, offset=0):
        signature, options, index_length, data_length, index3_null, data_null, shifted_high_start = struct.unpack_from(
            "<IHHHHHH", data, offset
        )
        if signature != 0x54726933:
            raise ValueError("not a UCPTrie")
        self.type = (options >> 6) & 3
        self.value_width = options & 7
        self.data_length = ((options & 0xF000) << 4) | data_length
        self.high_start = shifted_high_start << self.SHIFT_2
        position = offset + 16
        self.index = struct.unpack_from("<%dH" % index_length, data, position)
        position += index_length * 2
        if self.value_width == 0:
            self.data = struct.unpack_from("<%dH" % self.data_length, data, position)
            position += self.data_length * 2
        elif self.value_width == 1:
            self.data = struct.unpack_from("<%dI" % self.data_length, data, position)
            position += self.data_length * 4
        else:
            self.data = data[position : position + self.data_length]
            position += self.data_length
        self.size = position - offset

    def _small_index(self, c):
        i1 = c >> self.SHIFT_1
        if self.type == 0:
            i1 += self.BMP_INDEX_LENGTH - self.OMITTED_BMP_INDEX_1_LENGTH
        else:
            i1 += self.SMALL_INDEX_LENGTH
        i3_block = self.index[self.index[i1] + ((c >> self.SHIFT_2) & self.INDEX_2_MASK)]
        i3 = (c >> self.SHIFT_3) & self.INDEX_3_MASK
        if (i3_block & 0x8000) == 0:
            data_block = self.index[i3_block + i3]
        else:
            i3_block = (i3_block & 0x7FFF) + (i3 & ~7) + (i3 >> 3)
            i3 &= 7
            data_block = (self.index[i3_block] << (2 + (2 * i3))) & 0x30000
            data_block |= self.index[i3_block + 1 + i3]
        return data_block + (c & self.SMALL_DATA_MASK)

    def get(self, c):
        fast_max = 0xFFFF if self.type == 0 else 0xFFF
        if c <= fast_max:
            index = self.index[c >> self.FAST_SHIFT] + (c & self.FAST_DATA_MASK)
        elif c <= 0x10FFFF:
            if c >= self.high_start:
                index = self.data_length - 2
            else:
                index = self._small_index(c)
        else:
            index = self.data_length - 1
        return self.data[index]

    def values(self):
        return [self.get(c) for c in range(0x110000)]


class BreakRules:
    def __init__(self, payload):
        fields = struct.unpack_from("<I4BII10I", payload, 0)
        magic = fields[0]
        if magic != 0xB1A0:
            raise ValueError("not RBBI data")
        self.format_version = fields[1:5]
        if self.format_version[0] != 6:
            raise ValueError("unsupported RBBI data format %s" % (self.format_version,))
        self.length = fields[5]
        self.category_count = fields[6]
        forward_offset, forward_length = fields[7], fields[8]
        trie_offset, trie_length = fields[11], fields[12]
        status_offset, status_length = fields[15], fields[16]
        self.forward = StateTable(payload, forward_offset, self.category_count)
        self.trie = CodePointTrie(payload, trie_offset)
        self.statuses = list(struct.unpack_from("<%di" % (status_length // 4), payload, status_offset))


class StateTable:
    LOOKAHEAD_HARD_BREAK = 1
    BOF_REQUIRED = 2
    EIGHT_BIT_ROWS = 4

    def __init__(self, payload, offset, category_count):
        (
            self.state_count,
            self.row_length,
            self.dictionary_categories_start,
            self.lookahead_results_size,
            self.flags,
        ) = struct.unpack_from("<5I", payload, offset)
        self.eight_bit = (self.flags & self.EIGHT_BIT_ROWS) != 0
        width = 1 if self.eight_bit else 2
        code = "B" if self.eight_bit else "H"
        self.rows = []
        position = offset + 20
        for state in range(self.state_count):
            row = struct.unpack_from("<%d%s" % (3 + category_count, code), payload, position)
            self.rows.append(row)
            position += self.row_length
        if self.row_length != (3 + category_count) * width:
            raise ValueError("unexpected row length")


class Dictionary:
    TRIE_TYPE_BYTES = 0
    TRIE_TYPE_UCHARS = 1
    TRIE_TYPE_MASK = 7
    TRIE_HAS_VALUES = 8
    TRANSFORM_TYPE_OFFSET = 0x1000000
    TRANSFORM_TYPE_MASK = 0x7F000000
    TRANSFORM_OFFSET_MASK = 0x1FFFFF

    def __init__(self, payload):
        indexes = struct.unpack_from("<8i", payload, 0)
        trie_offset, total_size, trie_type, transform = indexes[0], indexes[3], indexes[4], indexes[5]
        self.trie_type = trie_type & self.TRIE_TYPE_MASK
        self.has_values = (trie_type & self.TRIE_HAS_VALUES) != 0
        self.transform = transform
        self.trie = payload[trie_offset:total_size]


class ResourceBundle:
    STRING, BINARY, TABLE, ALIAS, TABLE32, TABLE16, STRING_V2, INT, ARRAY, ARRAY16, INT_VECTOR = (
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        14,
    )

    def __init__(self, payload):
        self.data = payload
        root = struct.unpack_from("<I", payload, 0)[0]
        index_length = struct.unpack_from("<i", payload, 4)[0] & 0xFF
        indexes = struct.unpack_from("<%di" % index_length, payload, 4)
        self.keys_top = indexes[1] if index_length > 1 else 0
        attributes = indexes[5] if index_length > 5 else 0
        if attributes & 4:
            raise ValueError("bundles that use a pool bundle are not supported")
        sixteen_top = indexes[6] if index_length > 6 else self.keys_top
        self.units_offset = self.keys_top * 4
        self.units_count = (sixteen_top - self.keys_top) * 2
        self.root = self._read(root)

    def _key(self, offset):
        end = self.data.index(b"\0", offset)
        return self.data[offset:end].decode("ascii")

    def _unit(self, index):
        return struct.unpack_from("<H", self.data, self.units_offset + index * 2)[0]

    def _string_v2(self, offset):
        first = self._unit(offset)
        if (first & 0xFC00) != 0xDC00:
            length = 0
            while self._unit(offset + length) != 0:
                length += 1
            start = offset
        elif first < 0xDFEF:
            length = first & 0x3FF
            start = offset + 1
        elif first < 0xDFFF:
            length = ((first - 0xDFEF) << 16) | self._unit(offset + 1)
            start = offset + 2
        else:
            length = (self._unit(offset + 1) << 16) | self._unit(offset + 2)
            start = offset + 3
        units = struct.unpack_from("<%dH" % length, self.data, self.units_offset + start * 2)
        return bytes(struct.pack("<%dH" % length, *units)).decode("utf-16-le")

    def _read16(self, res16):
        return self._string_v2(res16)

    def _read(self, res):
        kind = res >> 28
        offset = res & 0x0FFFFFFF
        if kind == self.STRING_V2:
            return self._string_v2(offset)
        if kind in (self.STRING, self.ALIAS):
            if offset == 0:
                value = ""
            else:
                length = struct.unpack_from("<i", self.data, offset * 4)[0]
                value = self.data[offset * 4 + 4 : offset * 4 + 4 + length * 2].decode("utf-16-le")
            return ("alias", value) if kind == self.ALIAS else value
        if kind == self.INT:
            return offset if offset < 0x08000000 else offset - 0x10000000
        if kind == self.BINARY:
            if offset == 0:
                return b""
            length = struct.unpack_from("<i", self.data, offset * 4)[0]
            return self.data[offset * 4 + 4 : offset * 4 + 4 + length]
        if kind == self.INT_VECTOR:
            if offset == 0:
                return []
            length = struct.unpack_from("<i", self.data, offset * 4)[0]
            return list(struct.unpack_from("<%di" % length, self.data, offset * 4 + 4))
        if kind == self.ARRAY:
            if offset == 0:
                return []
            count = struct.unpack_from("<i", self.data, offset * 4)[0]
            items = struct.unpack_from("<%dI" % count, self.data, offset * 4 + 4)
            return [self._read(item) for item in items]
        if kind == self.ARRAY16:
            count = self._unit(offset)
            return [self._read16(self._unit(offset + 1 + i)) for i in range(count)]
        if kind == self.TABLE:
            if offset == 0:
                return {}
            count = struct.unpack_from("<H", self.data, offset * 4)[0]
            keys = struct.unpack_from("<%dH" % count, self.data, offset * 4 + 2)
            values_offset = offset * 4 + 2 + count * 2
            values_offset += (4 - values_offset % 4) % 4
            values = struct.unpack_from("<%dI" % count, self.data, values_offset)
            return {self._key(key): self._read(value) for key, value in zip(keys, values)}
        if kind == self.TABLE32:
            if offset == 0:
                return {}
            count = struct.unpack_from("<i", self.data, offset * 4)[0]
            keys = struct.unpack_from("<%di" % count, self.data, offset * 4 + 4)
            values = struct.unpack_from("<%dI" % count, self.data, offset * 4 + 4 + count * 4)
            return {self._key(key): self._read(value) for key, value in zip(keys, values)}
        if kind == self.TABLE16:
            count = self._unit(offset)
            keys = [self._unit(offset + 1 + i) for i in range(count)]
            values = [self._unit(offset + 1 + count + i) for i in range(count)]
            return {self._key(key): self._read16(value) for key, value in zip(keys, values)}
        raise ValueError("unsupported resource type %d" % kind)


class Normalization:
    IX_EXTRA_DATA_OFFSET = 1
    IX_MIN_COMP_NO_MAYBE_CP = 9
    IX_MIN_YES_NO = 10
    IX_MIN_NO_NO = 11
    IX_LIMIT_NO_NO = 12
    IX_MIN_MAYBE_YES = 13
    IX_MIN_YES_NO_MAPPINGS_ONLY = 14
    IX_MIN_NO_NO_COMP_NO_MAYBE_CC = 16
    IX_MIN_MAYBE_NO = 20

    INERT = 1
    JAMO_VT = 0xFE00
    MIN_NORMAL_MAYBE_YES = 0xFC00
    HAS_COMP_BOUNDARY_AFTER = 1
    OFFSET_SHIFT = 1
    DELTA_SHIFT = 3
    MAX_DELTA = 0x40
    MAPPING_HAS_CCC_LCCC_WORD = 0x80
    MAPPING_LENGTH_MASK = 0x1F

    def __init__(self, payload, format_version):
        if format_version[0] != 5:
            raise ValueError("unsupported normalization data format %s" % (format_version,))
        index_count = struct.unpack_from("<i", payload, 0)[0] // 4
        self.indexes = struct.unpack_from("<%di" % index_count, payload, 0)
        self.trie = CodePointTrie(payload, self.indexes[0])
        extra_start = self.indexes[self.IX_EXTRA_DATA_OFFSET]
        extra_end = self.indexes[2]
        self.extra = struct.unpack_from("<%dH" % ((extra_end - extra_start) // 2), payload, extra_start)
        self.min_comp_no_maybe_cp = self.indexes[self.IX_MIN_COMP_NO_MAYBE_CP]
        self.min_yes_no = self.indexes[self.IX_MIN_YES_NO]
        self.min_yes_no_mappings_only = self.indexes[self.IX_MIN_YES_NO_MAPPINGS_ONLY]
        self.min_no_no = self.indexes[self.IX_MIN_NO_NO]
        self.min_no_no_comp_no_maybe_cc = self.indexes[self.IX_MIN_NO_NO_COMP_NO_MAYBE_CC]
        self.limit_no_no = self.indexes[self.IX_LIMIT_NO_NO]
        self.min_maybe_no = self.indexes[self.IX_MIN_MAYBE_NO]
        self.min_maybe_yes = self.indexes[self.IX_MIN_MAYBE_YES]
        self.center_no_no_delta = (self.min_maybe_no >> self.DELTA_SHIFT) - self.MAX_DELTA - 1

    def raw_norm16(self, c):
        return self.trie.get(c)

    def norm16(self, c):
        if 0xD800 <= c <= 0xDBFF:
            return self.INERT
        return self.trie.get(c)

    def has_comp_boundary_before(self, c):
        if c < self.min_comp_no_maybe_cp:
            return True
        norm16 = self.norm16(c)
        return norm16 < self.min_no_no_comp_no_maybe_cc or self.limit_no_no <= norm16 < self.min_maybe_no

    def is_decomp_inert(self, c):
        norm16 = self.norm16(c)
        return (
            norm16 < self.min_yes_no
            or norm16 == self.JAMO_VT
            or self.min_maybe_yes <= norm16 <= self.MIN_NORMAL_MAYBE_YES
        )

    def is_comp_inert(self, c):
        norm16 = self.norm16(c)
        return norm16 < self.min_no_no and (norm16 & self.HAS_COMP_BOUNDARY_AFTER) != 0

    def segment_starters(self, limit):
        not_starter = bytearray(limit)
        for c in range(limit):
            norm16 = self.INERT if 0xD800 <= c <= 0xDBFF else self.trie.get(c)
            if (
                norm16 == self.INERT
                or self.min_yes_no <= norm16 < self.min_no_no
                or self.min_maybe_no <= norm16 < self.min_maybe_yes
            ):
                continue
            if norm16 >= self.min_maybe_yes:
                not_starter[c] = 1
                continue
            if norm16 < self.min_yes_no:
                continue
            c2 = c
            norm16_2 = norm16
            if self.limit_no_no <= norm16_2 < self.min_maybe_no:
                c2 = c + (norm16_2 >> self.DELTA_SHIFT) - self.center_no_no_delta
                norm16_2 = self.trie.get(c2)
            if norm16_2 <= self.min_yes_no:
                continue
            offset = norm16_2 >> self.OFFSET_SHIFT
            first_unit = self.extra[offset]
            length = first_unit & self.MAPPING_LENGTH_MASK
            if first_unit & self.MAPPING_HAS_CCC_LCCC_WORD and c == c2 and self.extra[offset - 1] & 0xFF:
                not_starter[c] = 1
            if length == 0 or norm16_2 < self.min_no_no:
                continue
            units = self.extra[offset + 1 : offset + 1 + length]
            decoded = []
            i = 0
            while i < len(units):
                unit = units[i]
                if 0xD800 <= unit <= 0xDBFF and i + 1 < len(units):
                    decoded.append(0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00))
                    i += 2
                else:
                    decoded.append(unit)
                    i += 1
            for d in decoded[1:]:
                not_starter[d] = 1
        return [not not_starter[c] for c in range(limit)]


class PropertyNames:
    def __init__(self, source):
        value_maps = self._array(source, "valueMaps")
        groups = bytes(self._array(source, "nameGroups"))
        self.properties = {}
        self.values = {}
        range_count = value_maps[0]
        position = 1
        for _ in range(range_count):
            start, limit = value_maps[position], value_maps[position + 1]
            position += 2
            for prop in range(start, limit):
                name_group, value_map = value_maps[position], value_maps[position + 1]
                position += 2
                self.properties[prop] = self._names(groups, name_group)
                if value_map:
                    self.values[prop] = self._value_map(value_maps, value_map, groups)

    @staticmethod
    def _array(source, name):
        start = source.index("PropNameData::%s[" % name)
        body = source[source.index("{", start) + 1 : source.index("};", start)]
        values = []
        position = 0
        while position < len(body):
            ch = body[position]
            if ch == "'":
                if body[position + 1] == "\\":
                    values.append(ord(body[position + 2]))
                    position += 4
                else:
                    values.append(ord(body[position + 1]))
                    position += 3
            elif ch.isdigit() or ch == "-":
                end = position + 1
                while end < len(body) and (body[end].isalnum()):
                    end += 1
                values.append(int(body[position:end], 0))
                position = end
            else:
                position += 1
        return values

    @staticmethod
    def _names(groups, offset):
        count = groups[offset]
        names = []
        position = offset + 1
        for _ in range(count):
            end = groups.index(b"\0", position)
            names.append(groups[position:end].decode("ascii"))
            position = end + 1
        return names

    def _value_map(self, value_maps, index, groups):
        count = value_maps[index + 1]
        result = {}
        position = index + 2
        if count < 0x10:
            for _ in range(count):
                start, limit = value_maps[position], value_maps[position + 1]
                position += 2
                for value in range(start, limit):
                    name_group = value_maps[position]
                    position += 1
                    if name_group:
                        result[value] = self._names(groups, name_group)
        else:
            count -= 0x10
            values = value_maps[position : position + count]
            offsets = value_maps[position + count : position + 2 * count]
            for value, name_group in zip(values, offsets):
                result[value] = self._names(groups, name_group)
        return result

    @staticmethod
    def normalize(name):
        return "".join(ch.lower() for ch in name if ch not in "-_ \t\n\v\f\r")

    def property_number(self, name):
        key = self.normalize(name)
        for number, names in self.properties.items():
            if any(self.normalize(alias) == key for alias in names if alias):
                return number
        raise KeyError(name)
