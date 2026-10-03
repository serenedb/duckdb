import os
import struct
import subprocess
import tempfile
import urllib.request
import zipfile

from icu_data import DataPackage

ICU_VERSION = os.environ.get("ICU_VERSION", "78.3")
UNICODE_VERSION = os.environ.get("UNICODE_VERSION", "17.0.0")
ICU_DATA_URL = "https://github.com/unicode-org/icu/releases/download/release-%s/icu4c-%s-data-bin-l.zip" % (
    ICU_VERSION,
    ICU_VERSION,
)
ICU_SOURCE_URL = "https://raw.githubusercontent.com/unicode-org/icu/release-%s/icu4c/source/" % ICU_VERSION
UNICODE_URL = "https://www.unicode.org/Public/%s/" % UNICODE_VERSION
CACHE_DIR = os.environ.get(
    "TEXT_DATA_CACHE", os.path.join(os.path.expanduser("~"), ".cache", "duckdb-text-data", ICU_VERSION)
)

MAX_CODE_POINT = 0x10FFFF
CODE_POINT_LIMIT = 0x110000


def fetch(url, name):
    os.makedirs(CACHE_DIR, exist_ok=True)
    path = os.path.join(CACHE_DIR, name)
    if not os.path.exists(path):
        print("downloading %s" % url)
        with urllib.request.urlopen(url) as response:
            data = response.read()
        with open(path + ".tmp", "wb") as handle:
            handle.write(data)
        os.rename(path + ".tmp", path)
    return path


def load_icu_package():
    path = fetch(ICU_DATA_URL, "icu4c-%s-data-bin-l.zip" % ICU_VERSION)
    with zipfile.ZipFile(path) as archive:
        name = next(n for n in archive.namelist() if n.endswith(".dat"))
        return DataPackage(archive.read(name))


def load_icu_source(relative):
    return open(fetch(ICU_SOURCE_URL + relative, os.path.basename(relative)), encoding="utf-8").read()


def load_icu_source_bytes(relative):
    with open(fetch(ICU_SOURCE_URL + relative, os.path.basename(relative)), "rb") as handle:
        return handle.read()


class UCD:
    def __init__(self):
        self.archive = zipfile.ZipFile(fetch(UNICODE_URL + "ucd/UCD.zip", "UCD-%s.zip" % UNICODE_VERSION))
        self.contents = set(self.archive.namelist())

    def text(self, name):
        if name in self.contents:
            return self.archive.read(name).decode("utf-8")
        with open(fetch(UNICODE_URL + name, name.replace("/", "-")), encoding="utf-8") as handle:
            return handle.read()

    def lines(self, name):
        for line in self.text(name).splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                yield [field.strip() for field in line.split(";")]

    def missing_lines(self, name):
        for line in self.text(name).splitlines():
            if line.startswith("# @missing:"):
                yield [field.strip() for field in line[len("# @missing:") :].split(";")]

    @staticmethod
    def parse_range(field):
        if ".." in field:
            first, last = field.split("..")
            return int(first, 16), int(last, 16)
        value = int(field, 16)
        return value, value

    def values(self, name, field=1, prop=None, default=None):
        result = [default] * CODE_POINT_LIMIT
        for source in (self.missing_lines(name), self.lines(name)):
            for fields in source:
                if prop is not None and fields[1] != prop:
                    continue
                first, last = self.parse_range(fields[0])
                value = fields[field] if field < len(fields) else ""
                result[first : last + 1] = [value] * (last - first + 1)
        return result

    def property_map(self, name, value_field=1, default=None):
        return self.values(name, value_field, None, default)

    def binary_set(self, name, prop):
        result = set()
        for fields in self.lines(name):
            if fields[1] == prop:
                first, last = self.parse_range(fields[0])
                result.update(range(first, last + 1))
        return result

    def unicode_data(self):
        result = {}
        range_start = None
        for fields in self.lines("UnicodeData.txt"):
            c = int(fields[0], 16)
            name = fields[1]
            if name.endswith(", First>"):
                range_start = c
                continue
            if name.endswith(", Last>"):
                for d in range(range_start, c + 1):
                    result[d] = fields
                continue
            result[c] = fields
        return result


def format_array(ctype, name, values, per_line=16, fmt=None):
    if fmt is None:
        fmt = (lambda v: "0x%02X" % v) if ctype == "uint8_t" else str
    out = ["const %s %s[] = {" % (ctype, name)]
    for i in range(0, len(values), per_line):
        out.append("    " + ", ".join(fmt(v) for v in values[i : i + per_line]) + ",")
    out.append("};")
    return out


def ranges_of(predicate_values):
    ranges = []
    for c in predicate_values:
        if ranges and ranges[-1][1] == c - 1:
            ranges[-1][1] = c
        else:
            ranges.append([c, c])
    return ranges


def format_ranges(name, ranges):
    out = ["const CodePointRange %s[] = {" % name]
    for i in range(0, len(ranges), 6):
        out.append("    " + ", ".join("{0x%X, 0x%X}" % (a, b) for a, b in ranges[i : i + 6]) + ",")
    out.append("};")
    out.append("const uint32_t %s_count = %d;" % (name, len(ranges)))
    return out


def compress(data):
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "unit")
        with open(path, "wb") as handle:
            handle.write(data)
        subprocess.run(["zstd", "-q", "-f", "--ultra", "-22", path, "-o", path + ".zst"], check=True)
        with open(path + ".zst", "rb") as handle:
            return handle.read()


class Unit:
    def __init__(self):
        self.arrays = []

    def add(self, code, values):
        size = struct.calcsize("<" + code)
        if self.arrays and size > self.arrays[-1][2]:
            raise ValueError("arrays must be added largest element first")
        self.arrays.append((code, values, size))

    def add_bytes(self, data):
        self.add("B", list(data))

    def encode(self):
        header = struct.pack("<%dI" % len(self.arrays), *[len(values) for _, values, _ in self.arrays])
        header += b"\0" * ((8 - len(header) % 8) % 8)
        body = b"".join(struct.pack("<%d%s" % (len(values), code), *values) for code, values, _ in self.arrays)
        return header + body


def format_units(units, prefix):
    out = []
    sizes = []
    total_raw = 0
    total_compressed = 0
    for i, (name, data) in enumerate(units):
        compressed = compress(data)
        total_raw += len(data)
        total_compressed += len(compressed)
        sizes.append(len(compressed))
        out += format_array("uint8_t", "%s_unit_%d" % (prefix, i), list(compressed), per_line=24)
        print("unit %s: %d bytes, %d compressed" % (name, len(data), len(compressed)))
    out.append("const TextUnit %s_units[] = {" % prefix)
    for i, (name, data) in enumerate(units):
        out.append("    {%s_unit_%d, %d, %d}, // %s" % (prefix, i, sizes[i], len(data), name))
    out.append("};")
    out.append("const uint32_t %s_unit_count = %d;" % (prefix, len(units)))
    print("units: %d bytes, %d compressed" % (total_raw, total_compressed))
    return out


class TwoStageTable:
    SHIFT = 6

    def __init__(self, values, limit=CODE_POINT_LIMIT):
        block = 1 << self.SHIFT
        self.stage1 = []
        self.stage2 = []
        known = {}
        for start in range(0, limit, block):
            chunk = tuple(values[start : start + block])
            offset = known.get(chunk)
            if offset is None:
                offset = len(self.stage2)
                known[chunk] = offset
                self.stage2.extend(chunk)
            self.stage1.append(offset)
        if len(self.stage2) > 0xFFFF + 1 - block:
            raise ValueError("table does not fit 16-bit block offsets")
