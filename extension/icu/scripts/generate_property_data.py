#!/usr/bin/env python3

import os
import struct
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icu_data import Normalization, PropertyNames  # noqa: E402
from unicode_inputs import (
    CODE_POINT_LIMIT,
    ICU_VERSION,
    UNICODE_VERSION,
    UCD,
    Unit,
    format_units,  # noqa: E402
    load_icu_package,
    load_icu_source,
    load_icu_source_bytes,
)

OUTPUT = os.path.join("extension", "icu", "properties", "generated", "property_data.cpp")

BINARY_LIMIT = 0x1000
INT_START = 0x1000
INT_LIMIT = 0x2000
GENERAL_CATEGORY = 0x1005
GENERAL_CATEGORY_MASK = 0x2000
NUMERIC_VALUE = 0x3000
AGE = 0x4000
SCRIPT = 0x100A
SCRIPT_EXTENSIONS = 0x7000
IDENTIFIER_TYPE = 0x7001
NO_NUMERIC_VALUE = -123456789.0
WITHOUT_DATA = {0x1016, 0x1017, 0x1018}

BINARY_FILES = [
    "PropList.txt",
    "DerivedCoreProperties.txt",
    "extracted/DerivedBinaryProperties.txt",
    "DerivedNormalizationProps.txt",
    "emoji/emoji-data.txt",
]

ENUMERATED = {
    0x1000: ("extracted/DerivedBidiClass.txt", 1, None, None),
    0x1001: ("Blocks.txt", 1, None, None),
    0x1003: ("extracted/DerivedDecompositionType.txt", 1, None, None),
    0x1004: ("EastAsianWidth.txt", 1, None, None),
    0x1005: ("extracted/DerivedGeneralCategory.txt", 1, None, None),
    0x1006: ("extracted/DerivedJoiningGroup.txt", 1, None, None),
    0x1007: ("extracted/DerivedJoiningType.txt", 1, None, None),
    0x1008: ("LineBreak.txt", 1, None, None),
    0x1009: ("extracted/DerivedNumericType.txt", 1, None, None),
    0x100A: ("Scripts.txt", 1, None, "Unknown"),
    0x100B: ("HangulSyllableType.txt", 1, None, None),
    0x100C: ("DerivedNormalizationProps.txt", 2, "NFD_QC", None),
    0x100D: ("DerivedNormalizationProps.txt", 2, "NFKD_QC", None),
    0x100E: ("DerivedNormalizationProps.txt", 2, "NFC_QC", None),
    0x100F: ("DerivedNormalizationProps.txt", 2, "NFKC_QC", None),
    0x1012: ("auxiliary/GraphemeBreakProperty.txt", 1, None, None),
    0x1013: ("auxiliary/SentenceBreakProperty.txt", 1, None, None),
    0x1014: ("auxiliary/WordBreakProperty.txt", 1, None, None),
    0x1015: ("BidiBrackets.txt", 2, None, "n"),
    0x1019: ("security/IdentifierStatus.txt", 1, None, None),
    0x101A: ("DerivedCoreProperties.txt", 2, "InCB", None),
}


class Properties:
    def __init__(self, ucd, names, package):
        self.ucd = ucd
        self.names = names
        self.package = package
        self.data = ucd.unicode_data()
        self.binary_sources = self._binary_sources()
        self.general_category = self._enumerated(GENERAL_CATEGORY)
        self.ccc = [0] * CODE_POINT_LIMIT
        for c, fields in self.data.items():
            self.ccc[c] = int(fields[3])

    def value_number(self, prop, name):
        key = PropertyNames.normalize(name)
        for value, aliases in self.names.values[prop].items():
            if any(PropertyNames.normalize(alias) == key for alias in aliases if alias):
                return value
        raise KeyError("%s has no value %r" % (self.names.properties[prop][1], name))

    def _binary_sources(self):
        sources = {}
        for name in BINARY_FILES:
            for fields in self.ucd.lines(name):
                if len(fields) != 2:
                    continue
                first, last = self.ucd.parse_range(fields[0])
                sources.setdefault(PropertyNames.normalize(fields[1]), set()).update(range(first, last + 1))
        return sources

    def _enumerated(self, prop):
        name, field, column, default = ENUMERATED[prop]
        names = self.ucd.values(name, field, column, default)
        cache = {}
        values = [0] * CODE_POINT_LIMIT
        for c in range(CODE_POINT_LIMIT):
            name = names[c]
            if name is None:
                raise ValueError("U+%04X has no value of %s" % (c, self.names.properties[prop][1]))
            value = cache.get(name)
            if value is None:
                value = self.value_number(prop, name)
                cache[name] = value
            values[c] = value
        return values

    def binary(self, prop):
        long_name = self.names.properties[prop][1]
        special = {
            "Case_Sensitive": self.case_sensitive,
            "NFD_Inert": lambda: self.inert("nfc", decompose=True),
            "NFKD_Inert": lambda: self.inert("nfkc", decompose=True),
            "NFC_Inert": lambda: self.inert("nfc", decompose=False),
            "NFKC_Inert": lambda: self.inert("nfkc", decompose=False),
            "Segment_Starter": self.segment_starter,
            "alnum": self.posix_alnum,
            "blank": self.posix_blank,
            "graph": self.posix_graph,
            "print": self.posix_print,
            "xdigit": self.posix_xdigit,
            "Basic_Emoji": self.basic_emoji,
            "RGI_Emoji": self.basic_emoji,
            "Emoji_Keycap_Sequence": lambda: set(),
            "RGI_Emoji_Modifier_Sequence": lambda: set(),
            "RGI_Emoji_Flag_Sequence": lambda: set(),
            "RGI_Emoji_Tag_Sequence": lambda: set(),
            "RGI_Emoji_ZWJ_Sequence": lambda: set(),
        }
        if long_name in special:
            members = special[long_name]()
        else:
            members = self.binary_sources.get(PropertyNames.normalize(long_name))
            if members is None:
                raise ValueError("no source for the binary property %s" % long_name)
        values = [0] * CODE_POINT_LIMIT
        for c in members:
            values[c] = 1
        return values

    def normalization(self, name):
        if name == "nfc":
            payload = load_icu_source_bytes("data/in/nfc.nrm")
            header_size = struct.unpack_from("<H", payload, 0)[0]
            return Normalization(payload[header_size:], tuple(payload[16:20]))
        payload, _, version = self.package.payload("%s.nrm" % name)
        return Normalization(payload, version)

    def inert(self, name, decompose):
        data = self.normalization(name)
        test = data.is_decomp_inert if decompose else data.is_comp_inert
        return {c for c in range(CODE_POINT_LIMIT) if test(c)}

    def segment_starter(self):
        starters = self.normalization("nfc").segment_starters(CODE_POINT_LIMIT)
        return {c for c in range(CODE_POINT_LIMIT) if starters[c]}

    def case_sensitive(self):
        result = {0x3C2}
        mappings = {}
        for c, fields in self.data.items():
            for field in (12, 13, 14):
                if fields[field]:
                    mappings.setdefault(c, []).append(int(fields[field], 16))
        for fields in self.ucd.lines("SpecialCasing.txt"):
            c = int(fields[0], 16)
            mappings.setdefault(c, [])
            if len(fields) > 4 and fields[4]:
                continue
            for field in (1, 2, 3):
                mappings[c].extend(int(p, 16) for p in fields[field].split())
        for fields in self.ucd.lines("CaseFolding.txt"):
            c = int(fields[0], 16)
            mappings.setdefault(c, [])
            if fields[1] in ("C", "S", "F"):
                mappings[c].extend(int(p, 16) for p in fields[2].split())
        for c, targets in mappings.items():
            result.add(c)
            result.update(targets)
        return result

    def posix_alnum(self):
        alphabetic = self.binary_sources[PropertyNames.normalize("Alphabetic")]
        digit = self.value_number(GENERAL_CATEGORY, "Nd")
        return {c for c in range(CODE_POINT_LIMIT) if c in alphabetic or self.general_category[c] == digit}

    def posix_blank(self):
        space = self.value_number(GENERAL_CATEGORY, "Zs")
        return {
            c for c in range(CODE_POINT_LIMIT) if (c in (0x9, 0x20) if c <= 0x9F else self.general_category[c] == space)
        }

    def posix_graph(self):
        excluded = {self.value_number(GENERAL_CATEGORY, name) for name in ("Cc", "Cs", "Cn", "Zs", "Zl", "Zp")}
        return {c for c in range(CODE_POINT_LIMIT) if self.general_category[c] not in excluded}

    def posix_print(self):
        space = self.value_number(GENERAL_CATEGORY, "Zs")
        graph = self.posix_graph()
        return {c for c in range(CODE_POINT_LIMIT) if self.general_category[c] == space or c in graph}

    def posix_xdigit(self):
        digit = self.value_number(GENERAL_CATEGORY, "Nd")
        result = set()
        for c in range(CODE_POINT_LIMIT):
            if (
                (0x41 <= c <= 0x46)
                or (0x61 <= c <= 0x66)
                or (0xFF21 <= c <= 0xFF26)
                or (0xFF41 <= c <= 0xFF46)
                or self.general_category[c] == digit
            ):
                result.add(c)
        return result

    def basic_emoji(self):
        result = set()
        for fields in self.ucd.lines("emoji/emoji-sequences.txt"):
            if fields[1] == "Basic_Emoji" and " " not in fields[0]:
                first, last = self.ucd.parse_range(fields[0])
                result.update(range(first, last + 1))
        return result

    def enumerated(self, prop):
        if prop == GENERAL_CATEGORY:
            return self.general_category
        if prop == 0x1002:
            return self.ccc
        if prop in (0x1010, 0x1011):
            return self.lead_trail_ccc(prop == 0x1010)
        return self._enumerated(prop)

    def full_decomposition(self, c, mappings):
        if 0xAC00 <= c <= 0xD7A3:
            index = c - 0xAC00
            result = [0x1100 + index // 588, 0x1161 + (index % 588) // 28]
            if index % 28:
                result.append(0x11A7 + index % 28)
            return result
        chars = mappings.get(c)
        if chars is None:
            return [c]
        result = []
        for d in chars:
            result.extend(self.full_decomposition(d, mappings))
        return result

    def lead_trail_ccc(self, lead):
        mappings = {}
        for c, fields in self.data.items():
            if fields[5] and not fields[5].startswith("<"):
                mappings[c] = [int(p, 16) for p in fields[5].split()]
        values = list(self.ccc)
        for c in list(mappings) + list(range(0xAC00, 0xD7A4)):
            chars = self.full_decomposition(c, mappings)
            values[c] = self.ccc[chars[0] if lead else chars[-1]]
        return values

    def script_extensions(self, script_values):
        lists = [None] * CODE_POINT_LIMIT
        for fields in self.ucd.lines("ScriptExtensions.txt"):
            first, last = self.ucd.parse_range(fields[0])
            scripts = tuple(sorted(self.value_number(SCRIPT, name) for name in fields[1].split()))
            for c in range(first, last + 1):
                lists[c] = scripts
        return [lists[c] if lists[c] is not None else (script_values[c],) for c in range(CODE_POINT_LIMIT)]

    def identifier_types(self):
        names = self.ucd.values("security/IdentifierType.txt", 1)
        cache = {}
        result = [None] * CODE_POINT_LIMIT
        for c in range(CODE_POINT_LIMIT):
            value = cache.get(names[c])
            if value is None:
                value = tuple(sorted(self.value_number(IDENTIFIER_TYPE, name) for name in names[c].split()))
                cache[names[c]] = value
            result[c] = value
        return result

    def numeric_values(self):
        values = [NO_NUMERIC_VALUE] * CODE_POINT_LIMIT
        for fields in self.ucd.lines("extracted/DerivedNumericValues.txt"):
            first, last = self.ucd.parse_range(fields[0])
            value = Fraction(fields[3])
            number = float(value.numerator) / value.denominator if value.denominator != 1 else float(value.numerator)
            for c in range(first, last + 1):
                values[c] = number
        return values

    def ages(self):
        names = self.ucd.values("DerivedAge.txt", 1)
        result = [0] * CODE_POINT_LIMIT
        for c in range(CODE_POINT_LIMIT):
            if names[c] != "Unassigned":
                major, minor = names[c].split(".")
                result[c] = int(major) << 8 | int(minor)
        return result


def runs_of(values):
    starts = []
    run_values = []
    previous = None
    for c in range(CODE_POINT_LIMIT):
        if values[c] != previous:
            starts.append(c)
            run_values.append(values[c])
            previous = values[c]
    return starts, run_values


def generate():
    ucd = UCD()
    package = load_icu_package()
    names = PropertyNames(load_icu_source("common/propname_data.h"))
    properties = Properties(ucd, names, package)

    numbers = sorted(names.properties)
    records = []
    run_starts = []
    run_values = []
    value_lists = [0]
    list_index = {(): 0}

    def add_runs(number, values, value_map):
        starts, values_of_runs = runs_of(values)
        records.append((number, len(run_starts), len(starts), value_map))
        run_starts.extend(starts)
        run_values.extend(values_of_runs)

    def intern_list(values):
        index = list_index.get(values)
        if index is None:
            index = len(value_lists)
            list_index[values] = index
            value_lists.append(len(values))
            value_lists.extend(values)
        return index

    value_maps = []
    value_aliases = []
    property_aliases = []
    chars = bytearray()
    char_index = {}

    def intern_name(name):
        key = PropertyNames.normalize(name).encode("ascii")
        offset = char_index.get(key)
        if offset is None:
            offset = len(chars)
            char_index[key] = offset
            chars.extend(key)
        return offset, len(key)

    for number in numbers:
        seen = set()
        for alias in names.properties[number]:
            if alias and PropertyNames.normalize(alias) not in seen:
                seen.add(PropertyNames.normalize(alias))
                property_aliases.append(intern_name(alias) + (number,))
    property_aliases.sort(key=lambda entry: bytes(chars[entry[0] : entry[0] + entry[1]]))

    map_of_property = {}
    for number in sorted(names.values):
        entries = []
        seen = {}
        for value, aliases in sorted(names.values[number].items()):
            for alias in aliases:
                key = PropertyNames.normalize(alias)
                if not alias or key in seen:
                    continue
                seen[key] = value
                entries.append(intern_name(alias) + (value & 0xFFFFFFFF,))
        entries.sort(key=lambda entry: bytes(chars[entry[0] : entry[0] + entry[1]]))
        map_of_property[number] = len(value_maps)
        value_maps.append((number, len(value_aliases), len(entries)))
        value_aliases.extend(entries)

    script_values = None
    numeric = []
    for number in numbers:
        if number < BINARY_LIMIT:
            add_runs(number, properties.binary(number), map_of_property[number])
        elif number in WITHOUT_DATA:
            continue
        elif INT_START <= number < INT_LIMIT:
            values = properties.enumerated(number)
            if number == SCRIPT:
                script_values = values
            add_runs(number, values, map_of_property[number])
        elif number == NUMERIC_VALUE:
            values = properties.numeric_values()
            distinct = sorted(set(values))
            index = {value: i for i, value in enumerate(distinct)}
            numeric = distinct
            add_runs(number, [index[v] for v in values], 0xFFFFFFFF)
        elif number == AGE:
            add_runs(number, properties.ages(), 0xFFFFFFFF)
        elif number == SCRIPT_EXTENSIONS:
            lists = properties.script_extensions(script_values)
            add_runs(number, [intern_list(values) for values in lists], map_of_property[SCRIPT])
        elif number == IDENTIFIER_TYPE:
            lists = properties.identifier_types()
            add_runs(number, [intern_list(values) for values in lists], map_of_property[IDENTIFIER_TYPE])
    if max(run_values) > 0xFFFF or max(value_lists) > 0xFFFF:
        raise ValueError("property values do not fit 16 bits")

    unit = Unit()
    unit.add("d", numeric)
    unit.add("I", run_starts)
    unit.add("I", [v for record in records for v in record])
    unit.add("I", [v for record in value_maps for v in record])
    unit.add("I", [v for record in property_aliases for v in record])
    unit.add("I", [v for record in value_aliases for v in record])
    unit.add("H", run_values)
    unit.add("H", value_lists)
    unit.add_bytes(bytes(chars))
    print(
        "properties: %d properties, %d runs, %d aliases, %d value aliases"
        % (len(records), len(run_starts), len(property_aliases), len(value_aliases))
    )

    return [("properties", unit.encode())]


HEADER = """//===----------------------------------------------------------------------===//
//                         DuckDB
//
// property_data.cpp
//
// This file is generated by extension/icu/scripts/generate_property_data.py
// from the ICU %s sources and data package and UCD %s. Do not edit.
//
//===----------------------------------------------------------------------===//

#include "property_data.hpp"

namespace duckdb {
namespace text {
"""


def main():
    units = generate()
    out = [HEADER % (ICU_VERSION, UNICODE_VERSION)]
    out += format_units(units, "property")
    out.append("const uint32_t property_values_unit = 0;")
    out.append("")
    out.append("} // namespace text")
    out.append("} // namespace duckdb")
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print("wrote %s" % OUTPUT)


if __name__ == "__main__":
    main()
