#!/usr/bin/env python3

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icu_data import BreakRules, Dictionary, Normalization, ResourceBundle  # noqa: E402
from unicode_inputs import (
    CODE_POINT_LIMIT,
    ICU_VERSION,
    UNICODE_VERSION,
    UCD,
    TwoStageTable,
    Unit,  # noqa: E402
    format_array,
    format_ranges,
    format_units,
    load_icu_package,
    load_icu_source,
    ranges_of,
)

OUTPUT = os.path.join("extension", "icu", "text", "generated", "text_data.cpp")

RULE_SETS = [
    ("word", "brkitr/word.brk"),
    ("word_posix", "brkitr/word_POSIX.brk"),
    ("sentence", "brkitr/sent.brk"),
    ("sentence_el", "brkitr/sent_el.brk"),
]


class CategoryTries:
    BLOCK = 64
    CHUNK = 4096

    def __init__(self):
        self.blocks = []
        self.known_blocks = {}
        self.chunks = []
        self.known_chunks = {}
        self.out = []

    def _block(self, values, start):
        chunk = tuple(values[start : start + self.BLOCK])
        index = self.known_blocks.get(chunk)
        if index is None:
            index = len(self.blocks)
            self.known_blocks[chunk] = index
            self.blocks.append(chunk)
        return index

    def add(self, values, name):
        if max(values) > 0xFF:
            raise ValueError("categories do not fit a byte")
        bmp = [self._block(values, start) for start in range(0, 0x10000, self.BLOCK)]
        supplementary = []
        for start in range(0x10000, CODE_POINT_LIMIT, self.CHUNK):
            chunk = tuple(self._block(values, s) for s in range(start, start + self.CHUNK, self.BLOCK))
            index = self.known_chunks.get(chunk)
            if index is None:
                index = len(self.chunks)
                self.known_chunks[chunk] = index
                self.chunks.append(chunk)
            supplementary.append(index * (self.CHUNK // self.BLOCK))
        self.out += format_array("uint8_t", "%s_ascii" % name, values[:128])
        self.out += format_array("uint16_t", "%s_bmp" % name, bmp)
        self.out += format_array("uint16_t", "%s_supplementary" % name, supplementary)
        self.out.append(
            "const BreakCategoryTrie %s = {%s_ascii, %s_bmp, %s_supplementary, break_category_chunks, "
            "break_category_blocks};" % (name, name, name, name)
        )

    def finish(self):
        if len(self.blocks) > 0xFFFF or len(self.chunks) * (self.CHUNK // self.BLOCK) > 0xFFFF:
            raise ValueError("too many category blocks")
        out = []
        out += format_array("uint8_t", "break_category_blocks", [v for chunk in self.blocks for v in chunk])
        out += format_array("uint16_t", "break_category_chunks", [index for chunk in self.chunks for index in chunk])
        size = len(self.blocks) * self.BLOCK + len(self.chunks) * 2 * (self.CHUNK // self.BLOCK)
        return out + self.out, size


def generate_rule_sets(package):
    out = []
    tries = {}
    category_tries = CategoryTries()
    total = 0
    loaded = []
    for name, item in RULE_SETS:
        rules = BreakRules(package.payload(item)[0])
        categories = rules.trie.values()
        key = tuple(categories)
        trie_name = tries.get(key)
        if trie_name is None:
            trie_name = "break_categories_%s" % name
            tries[key] = trie_name
            category_tries.add(categories, trie_name)
        loaded.append((name, rules, trie_name))
    trie_out, size = category_tries.finish()
    out += trie_out
    total += size + len(tries) * (128 + 2 * 1024 + 2 * 256)
    for name, rules, trie_name in loaded:
        table = rules.forward
        if not table.eight_bit or table.lookahead_results_size != 0 or any(row[1] != 0 for row in table.rows):
            raise ValueError("%s needs 16-bit rows or look-ahead rules" % name)
        if any(row[0] > 1 for row in table.rows):
            raise ValueError("%s has look-ahead accepting states" % name)
        flat = [value for row in table.rows for value in (row[0], row[2]) + tuple(row[3:])]
        out += format_array("uint8_t", "break_states_%s" % name, flat, per_line=2 + rules.category_count)
        out += format_array("int32_t", "break_statuses_%s" % name, rules.statuses)
        out.append(
            "const BreakRuleSet break_rules_%s = {&%s, break_states_%s, break_statuses_%s, %d, %d, %d, %s, %d};"
            % (
                name,
                trie_name,
                name,
                name,
                rules.category_count,
                table.state_count,
                table.dictionary_categories_start,
                "true" if table.flags & table.BOF_REQUIRED else "false",
                len(rules.statuses),
            )
        )
        total += len(flat) + 4 * len(rules.statuses)
        print(
            "%s: %d categories, %d states, %d status values"
            % (name, rules.category_count, table.state_count, len(rules.statuses))
        )
    print("break rules: %d bytes" % total)
    return out


DICTIONARIES = [
    ("cj", "brkitr/cjdict.dict"),
    ("thai", "brkitr/thaidict.dict"),
    ("lao", "brkitr/laodict.dict"),
    ("burmese", "brkitr/burmesedict.dict"),
    ("khmer", "brkitr/khmerdict.dict"),
]


def generate_dictionaries(package, units):
    out = ["const BreakDictionary break_dictionaries[] = {"]
    for name, item in DICTIONARIES:
        dictionary = Dictionary(package.payload(item)[0])
        unit = len(units)
        units.append(("dictionary_%s" % name, dictionary.trie))
        out.append(
            "    {%d, %d, %s, 0x%X},"
            % (unit, dictionary.trie_type, "true" if dictionary.has_values else "false", dictionary.transform)
        )
        print("%s dictionary: %d bytes" % (name, len(dictionary.trie)))
    out.append("};")
    return out


def generate_engine_sets(ucd, package):
    script = ucd.property_map("Scripts.txt", default="Unknown")
    line_break = ucd.property_map("LineBreak.txt", default="XX")
    category = [None] * CODE_POINT_LIMIT
    for c, fields in ucd.unicode_data().items():
        category[c] = fields[2]
    mark = lambda c: category[c] in ("Mn", "Mc", "Me")

    def code_points(predicate):
        return [c for c in range(CODE_POINT_LIMIT) if predicate(c)]

    sets = {}

    def complex_context(name):
        return lambda c: script[c] == name and line_break[c] == "SA"

    for engine, script_name in (("thai", "Thai"), ("lao", "Lao"), ("burmese", "Myanmar"), ("khmer", "Khmer")):
        word = complex_context(script_name)
        sets[engine + "_word"] = code_points(word)
        marks = code_points(lambda c, word=word: word(c) and mark(c))
        sets[engine + "_marks"] = sorted(set(marks) | {0x20})
    thai_word = set(sets["thai_word"])
    sets["thai_end_word"] = sorted(thai_word - {0x0E31} - set(range(0x0E40, 0x0E45)))
    sets["thai_begin_word"] = sorted(set(range(0x0E01, 0x0E2F)) | set(range(0x0E40, 0x0E45)))
    lao_word = set(sets["lao_word"])
    sets["lao_end_word"] = sorted(lao_word - set(range(0x0EC0, 0x0EC5)))
    sets["lao_begin_word"] = sorted(set(range(0x0E81, 0x0EAF)) | {0x0EDC, 0x0EDD} | set(range(0x0EC0, 0x0EC5)))
    sets["burmese_end_word"] = sets["burmese_word"]
    sets["burmese_begin_word"] = list(range(0x1000, 0x102B))
    khmer_word = set(sets["khmer_word"])
    sets["khmer_end_word"] = sorted(khmer_word - {0x17D2})
    sets["khmer_begin_word"] = list(range(0x1780, 0x17B4))
    sets["cj_word"] = sorted(
        set(code_points(lambda c: script[c] in ("Han", "Hiragana", "Katakana"))) | {0x30FC, 0xFF70, 0xFF9E, 0xFF9F}
    )

    handled = set()
    for name in ("thai_word", "lao_word", "burmese_word", "khmer_word", "cj_word"):
        handled.update(sets[name])
    unhandled_scripts = set()
    for item in ("brkitr/word.brk", "brkitr/word_POSIX.brk", "brkitr/sent.brk", "brkitr/sent_el.brk"):
        rules = BreakRules(package.payload(item)[0])
        categories = rules.trie.values()
        start = rules.forward.dictionary_categories_start
        unhandled_scripts.update(
            script[c] for c in range(CODE_POINT_LIMIT) if categories[c] >= start and c not in handled
        )
    unhandled_scripts = sorted(unhandled_scripts)
    print("scripts of dictionary characters without an engine: %s" % ", ".join(unhandled_scripts))

    out = []
    for name in sorted(sets):
        out += format_ranges("break_engine_%s" % name, ranges_of(sets[name]))
    script_ranges = []
    for c in range(CODE_POINT_LIMIT):
        if script[c] not in unhandled_scripts:
            continue
        index = unhandled_scripts.index(script[c])
        if script_ranges and script_ranges[-1][1] == c - 1 and script_ranges[-1][2] == index:
            script_ranges[-1][1] = c
        else:
            script_ranges.append([c, c, index])
    out.append("// the scripts: %s" % ", ".join(unhandled_scripts))
    out.append("const BreakScriptRange break_engine_unhandled[] = {")
    for i in range(0, len(script_ranges), 5):
        out.append("    " + ", ".join("{0x%X, 0x%X, %d}" % tuple(r) for r in script_ranges[i : i + 5]) + ",")
    out.append("};")
    out.append("const uint32_t break_engine_unhandled_count = %d;" % len(script_ranges))
    out += format_ranges("nonspacing_marks", ranges_of(code_points(lambda c: category[c] == "Mn")))
    return out


def generate_nfkc_boundaries(package):
    payload, _, version = package.payload("nfkc.nrm")
    nfkc = Normalization(payload, version)
    no_boundary = [c for c in range(CODE_POINT_LIMIT) if not nfkc.has_comp_boundary_before(c)]
    return format_ranges("nfkc_no_boundary_before", ranges_of(no_boundary))


EXCEPTION_PARTIAL = 1
EXCEPTION_MATCH = 2


def utf16_key(text):
    data = text.encode("utf-16-le")
    return struct.unpack("<%dH" % (len(data) // 2), data)


def exception_tries(strings):
    strings = sorted(set(strings), key=utf16_key)
    count = len(strings)
    suppress_in_reverse = 1
    add_to_forward = 2
    partials = [0] * count
    backward = []
    forward = []
    for i in range(count):
        position = strings[i].find(".")
        if position > -1 and position + 1 != len(strings[i]):
            same_as = -1
            for j in range(count):
                if j == i:
                    continue
                if utf16_key(strings[i][: position + 1]) == utf16_key(strings[j][: position + 1]):
                    if partials[j] == 0:
                        partials[j] = suppress_in_reverse | add_to_forward
                    elif partials[j] & suppress_in_reverse:
                        same_as = j
            prefix = strings[i][: position + 1]
            if same_as == -1 and partials[i] == 0:
                backward.append((prefix[::-1], EXCEPTION_PARTIAL))
                partials[i] = suppress_in_reverse | add_to_forward
    for i in range(count):
        if partials[i] == 0:
            backward.append((strings[i][::-1], EXCEPTION_MATCH))
        else:
            forward.append((strings[i], EXCEPTION_MATCH))
    keys = [key for key, _ in backward]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate backward exception")
    backward.sort(key=lambda entry: [ord(ch) for ch in entry[0]])
    forward.sort(key=lambda entry: [ord(ch) for ch in entry[0]])
    return backward, forward


def generate_sentence_exceptions(package):
    out = []
    infos = []
    chars = []
    entries = []
    for name in package.names("brkitr/"):
        if not name.endswith(".res"):
            continue
        bundle = ResourceBundle(package.payload(name)[0]).root
        exceptions = bundle.get("exceptions", {}).get("SentenceBreak")
        if not exceptions:
            continue
        locale = name[len("brkitr/") : -len(".res")]
        backward, forward = exception_tries(exceptions)
        first_backward = len(entries)
        for text, value in backward:
            entries.append((len(chars), len(text), value))
            chars.extend(ord(ch) for ch in text)
        first_forward = len(entries)
        for text, value in forward:
            entries.append((len(chars), len(text), value))
            chars.extend(ord(ch) for ch in text)
        infos.append((locale, first_backward, len(backward), first_forward, len(forward)))
    out += format_array("uint32_t", "sentence_exception_chars", chars, fmt=lambda v: "0x%X" % v)
    out.append("const SentenceException sentence_exception_entries[] = {")
    for offset, length, value in entries:
        out.append("    {%d, %d, %d}," % (offset, length, value))
    out.append("};")
    out.append("const SentenceExceptionLocale sentence_exception_locales[] = {")
    for locale, first_backward, backward_count, first_forward, forward_count in sorted(infos):
        out.append(
            '    {"%s", %d, %d, %d, %d},' % (locale, first_backward, backward_count, first_forward, forward_count)
        )
    out.append("};")
    out.append("const uint32_t sentence_exception_locale_count = %d;" % len(infos))
    return out


def generate_break_locales(package):
    out = ["const BreakRuleLocale break_rule_locales[] = {"]
    names = {
        "word": "&break_rules_word",
        "word_POSIX": "&break_rules_word_posix",
        "sentence": "&break_rules_sentence",
        "sentence_el": "&break_rules_sentence_el",
    }
    files = {"word.brk": "word", "word_POSIX.brk": "word_POSIX", "sent.brk": "sentence", "sent_el.brk": "sentence_el"}
    rows = []
    for name in package.names("brkitr/"):
        if not name.endswith(".res"):
            continue
        locale = name[len("brkitr/") : -len(".res")]
        if locale in ("root", "res_index"):
            continue
        boundaries = ResourceBundle(package.payload(name)[0]).root.get("boundaries", {})
        for kind in ("word", "sentence"):
            if kind in boundaries:
                rows.append((locale, kind, names[files[boundaries[kind]]]))
    for locale, kind, rules in sorted(rows):
        out.append(
            '    {"%s", %s, %s},' % (locale, "BreakKind::WORD" if kind == "word" else "BreakKind::SENTENCE", rules)
        )
    out.append("};")
    out.append("const uint32_t break_rule_locale_count = %d;" % len(rows))
    return out


HANGUL_S_BASE = 0xAC00
HANGUL_S_COUNT = 11172


def full_decomposition(c, mappings, compatibility):
    if HANGUL_S_BASE <= c < HANGUL_S_BASE + HANGUL_S_COUNT:
        index = c - HANGUL_S_BASE
        result = [0x1100 + index // 588, 0x1161 + (index % 588) // 28]
        if index % 28:
            result.append(0x11A7 + index % 28)
        return result
    entry = mappings.get(c)
    if entry is None:
        return [c]
    tag, chars = entry
    if tag is not None and not compatibility:
        return [c]
    result = []
    for d in chars:
        result.extend(full_decomposition(d, mappings, compatibility))
    return result


def canonical_order(chars, ccc):
    chars = list(chars)
    i = 1
    while i < len(chars):
        current = ccc.get(chars[i], 0)
        j = i
        while j > 0 and current != 0 and ccc.get(chars[j - 1], 0) > current:
            chars[j - 1], chars[j] = chars[j], chars[j - 1]
            j -= 1
        i += 1
    return chars


def generate_normalization(ucd, units):
    data = ucd.unicode_data()
    ccc = {}
    mappings = {}
    for c, fields in data.items():
        if fields[3] != "0":
            ccc[c] = int(fields[3])
        if fields[5]:
            parts = fields[5].split()
            tag = None
            if parts[0].startswith("<"):
                tag = parts[0]
                parts = parts[1:]
            mappings[c] = (tag, [int(p, 16) for p in parts])
    exclusions = set()
    casefold = {}
    nfc_not_yes = set()
    nfkc_not_yes = set()
    for fields in ucd.lines("DerivedNormalizationProps.txt"):
        first, last = ucd.parse_range(fields[0])
        if fields[1] == "Full_Composition_Exclusion":
            exclusions.update(range(first, last + 1))
        elif fields[1] == "NFKC_CF":
            value = [int(p, 16) for p in fields[2].split()] if fields[2] else []
            for c in range(first, last + 1):
                casefold[c] = value
        elif fields[1] == "NFC_QC":
            nfc_not_yes.update(range(first, last + 1))
        elif fields[1] == "NFKC_QC":
            nfkc_not_yes.update(range(first, last + 1))
    compositions = []
    for c, (tag, chars) in sorted(mappings.items()):
        if tag is None and len(chars) == 2 and c not in exclusions:
            compositions.append((chars[0], chars[1], c))
    compositions.sort()

    pool = []
    pool_index = {}

    def intern(chars):
        key = tuple(chars)
        offset = pool_index.get(key)
        if offset is None:
            offset = len(pool)
            pool_index[key] = offset
            pool.extend(chars)
        if len(chars) > 0xFF or offset >= 1 << 24:
            raise ValueError("mapping does not fit an entry")
        return offset | (len(chars) << 24)

    entries = [(0, 0, 0, 0)]
    entry_index = {entries[0]: 0}
    values = [0] * CODE_POINT_LIMIT
    for c in range(CODE_POINT_LIMIT):
        if HANGUL_S_BASE <= c < HANGUL_S_BASE + HANGUL_S_COUNT:
            continue
        canonical = full_decomposition(c, mappings, False)
        compat = full_decomposition(c, mappings, True)
        has_canonical = canonical != [c]
        has_compat = compat != [c]
        flags = 0
        canonical_value = 0
        compat_value = 0
        casefold_value = 0
        if has_canonical:
            canonical_value = intern(canonical_order(canonical, ccc))
            flags |= 1
        if has_compat:
            compat_value = intern(canonical_order(compat, ccc))
            flags |= 2
        if c in casefold:
            mapped = []
            for d in casefold[c]:
                mapped.extend(full_decomposition(d, mappings, False))
            casefold_value = intern(canonical_order(mapped, ccc))
            flags |= 4
        if c in exclusions:
            flags |= 8
        if c in nfc_not_yes:
            flags |= 16
        if c in nfkc_not_yes:
            flags |= 32
        entry = (canonical_value, compat_value, casefold_value, ccc.get(c, 0) | (flags << 8))
        index = entry_index.get(entry)
        if index is None:
            index = len(entries)
            entry_index[entry] = index
            entries.append(entry)
        values[c] = index
    if len(entries) > 0xFFFF:
        raise ValueError("too many normalization entries")
    table = TwoStageTable(values)
    unit = Unit()
    unit.add("I", [v for entry in entries for v in entry])
    unit.add("I", pool)
    unit.add("I", [first for first, _, _ in compositions])
    unit.add("I", [second for _, second, _ in compositions])
    unit.add("I", [result for _, _, result in compositions])
    unit.add("H", table.stage1)
    unit.add("H", table.stage2)
    units.append(("normalization", unit.encode()))
    print(
        "normalization: %d entries, %d mapped code points in the pool, %d compositions"
        % (len(entries), len(pool), len(compositions))
    )
    return len(units) - 1


CASE_CASED = 1
CASE_IGNORABLE = 2
CASE_DOT_SHIFT = 2
CASE_SOFT_DOTTED = 1
CASE_ABOVE = 2
CASE_OTHER_ACCENT = 3


def generate_case(ucd, units):
    data = ucd.unicode_data()
    simple_upper = {}
    simple_lower = {}
    for c, fields in data.items():
        if fields[12]:
            simple_upper[c] = int(fields[12], 16)
        if fields[13]:
            simple_lower[c] = int(fields[13], 16)
    full_lower = {}
    full_upper = {}
    for fields in ucd.lines("SpecialCasing.txt"):
        if len(fields) > 4 and fields[4]:
            continue
        c = int(fields[0], 16)
        full_lower[c] = [int(p, 16) for p in fields[1].split()]
        full_upper[c] = [int(p, 16) for p in fields[3].split()]
    full_fold = {}
    for fields in ucd.lines("CaseFolding.txt"):
        c = int(fields[0], 16)
        if fields[1] in ("C", "F"):
            full_fold[c] = [int(p, 16) for p in fields[2].split()]
    cased = ucd.binary_set("DerivedCoreProperties.txt", "Cased")
    ignorable = ucd.binary_set("DerivedCoreProperties.txt", "Case_Ignorable")
    soft_dotted = ucd.binary_set("PropList.txt", "Soft_Dotted")
    ccc = {c: int(fields[3]) for c, fields in data.items() if fields[3] != "0"}

    pool = []
    pool_index = {}

    def intern(chars):
        key = tuple(chars)
        offset = pool_index.get(key)
        if offset is None:
            offset = len(pool)
            pool_index[key] = offset
            pool.extend(chars)
        return offset | (len(chars) << 24) | (1 << 31)

    def full_value(c, full):
        if c in full:
            return intern(full[c])
        return 0

    entries = [(0,) * 6]
    entry_index = {entries[0]: 0}
    values = [0] * CODE_POINT_LIMIT
    for c in range(CODE_POINT_LIMIT):
        flags = 0
        if c in cased:
            flags |= CASE_CASED
        if c in ignorable:
            flags |= CASE_IGNORABLE
        combining = ccc.get(c, 0)
        if c in soft_dotted:
            if combining != 0:
                raise ValueError("U+%04X is soft-dotted and has a combining class" % c)
            flags |= CASE_SOFT_DOTTED << CASE_DOT_SHIFT
        elif combining == 230:
            flags |= CASE_ABOVE << CASE_DOT_SHIFT
        elif combining != 0:
            flags |= CASE_OTHER_ACCENT << CASE_DOT_SHIFT
        entry = (
            (simple_lower.get(c, c) - c) & 0xFFFFFFFF,
            (simple_upper.get(c, c) - c) & 0xFFFFFFFF,
            full_value(c, full_lower),
            full_value(c, full_upper),
            full_value(c, full_fold),
            flags,
        )
        index = entry_index.get(entry)
        if index is None:
            index = len(entries)
            entry_index[entry] = index
            entries.append(entry)
        values[c] = index
    if len(entries) > 0xFFFF:
        raise ValueError("too many case entries")
    table = TwoStageTable(values)
    unit = Unit()
    unit.add("I", [v for entry in entries for v in entry])
    unit.add("I", pool)
    unit.add("H", table.stage1)
    unit.add("H", table.stage2)
    units.append(("case", unit.encode()))
    print("case: %d entries, %d code points in full mappings" % (len(entries), len(pool)))
    return len(units) - 1


GREEK_UPPER_CONSTANTS = {
    "UPPER_MASK": 0x3FF,
    "HAS_VOWEL": 0x1000,
    "HAS_YPOGEGRAMMENI": 0x2000,
    "HAS_ACCENT": 0x4000,
    "HAS_DIALYTIKA": 0x8000,
}


def evaluate_flags(expression, constants):
    value = 0
    for term in expression.split("|"):
        term = term.strip()
        value |= constants[term] if term in constants else int(term, 0)
    return value


def generate_greek_upper():
    header = load_icu_source("common/ucasemap_imp.h")
    source = load_icu_source("common/ustrcase.cpp")
    constants = {}
    namespace = header[header.index("namespace GreekUpper {") :]
    namespace = "\n".join(line.split("//", 1)[0] for line in namespace.splitlines())
    for statement in namespace.split(";"):
        statement = " ".join(statement.split())
        start = statement.find("static const uint32_t ")
        if start >= 0 and "=" in statement:
            name, expression = statement[start + len("static const uint32_t ") :].split("=", 1)
            constants[name.strip()] = evaluate_flags(expression, constants)
    for name, value in GREEK_UPPER_CONSTANTS.items():
        if constants.get(name) != value:
            raise ValueError("GreekUpper::%s changed in ICU" % name)
    if "c < 0x370 || 0x2126 < c || (0x3ff < c && c < 0x1f00)" not in source:
        raise ValueError("the ranges of GreekUpper::getLetterData changed in ICU")
    tables = {}
    for name, size in (("data0370", 0x90), ("data1F00", 0x100)):
        start = source.index("static const uint16_t %s[] = {" % name)
        body = source[source.index("{", start) + 1 : source.index("};", start)]
        body = "\n".join(line.split("//", 1)[0] for line in body.splitlines())
        values = [evaluate_flags(item, constants) for item in body.split(",") if item.strip()]
        if len(values) != size:
            raise ValueError("GreekUpper::%s has %d entries" % (name, len(values)))
        tables[name] = values
    start = source.index("static const uint16_t data2126 = ")
    ohm = evaluate_flags(source[start + len("static const uint16_t data2126 = ") : source.index(";", start)], constants)
    out = []
    out += format_array("uint16_t", "greek_upper_0370", tables["data0370"], per_line=12, fmt=lambda v: "0x%04X" % v)
    out += format_array("uint16_t", "greek_upper_1f00", tables["data1F00"], per_line=12, fmt=lambda v: "0x%04X" % v)
    out.append("const uint16_t greek_upper_2126 = 0x%04X;" % ohm)
    return out


def parse_c_string_lists(source, name):
    start = source.index("constexpr const char* %s[] = {" % name)
    end = source.index("};", start)
    body = source[start:end]
    body = "\n".join(line.split("/*", 1)[0] if "*/" in line else line for line in body.splitlines())
    lists = [[]]
    for token in body.split("{", 1)[1].replace("\n", " ").split(","):
        token = token.strip()
        if not token:
            continue
        if token == "nullptr":
            lists.append([])
        else:
            lists[-1].append(token.strip('"'))
    if len(lists) < 2:
        raise ValueError("%s is not two nullptr-terminated lists" % name)
    return lists[:2]


def generate_locales():
    source = load_icu_source("common/uloc.cpp")
    languages = parse_c_string_lists(source, "LANGUAGES")
    languages_3 = parse_c_string_lists(source, "LANGUAGES_3")
    countries = parse_c_string_lists(source, "COUNTRIES")
    countries_3 = parse_c_string_lists(source, "COUNTRIES_3")
    for two, three in ((languages, languages_3), (countries, countries_3)):
        if [len(items) for items in two] != [len(items) for items in three]:
            raise ValueError("the ISO tables of uloc.cpp are not aligned")
    out = []

    def emit(name, lists):
        flat = lists[0] + lists[1]
        out.append("const char *const %s[] = {" % name)
        for i in range(0, len(flat), 12):
            out.append("    " + ", ".join('"%s"' % item for item in flat[i : i + 12]) + ",")
        out.append("};")
        out.append("const uint32_t %s_current_count = %d;" % (name, len(lists[0])))
        out.append("const uint32_t %s_count = %d;" % (name, len(flat)))

    emit("iso_languages", languages)
    emit("iso_languages_3", languages_3)
    emit("iso_countries", countries)
    emit("iso_countries_3", countries_3)
    print("locales: %d languages, %d countries" % (len(languages[0]), len(countries[0])))
    return out


COLLATION_DATA = os.environ.get(
    "TEXT_COLLATION_DATA", os.path.join("extension", "icu", "collation", "generated", "collation_data.cpp")
)


def read_collation_names():
    source = open(COLLATION_DATA, encoding="utf-8").read()
    start = source.index("const CollationInfo collation_infos[] = {")
    body = source[start : source.index("};", start)]
    names = []
    for part in body.split("{")[2:]:
        names.append(part.split('"')[1])
    return names


def generate_collation_locales(package):
    bundles = {}
    for name in package.names("coll/"):
        if name.endswith(".res") and name != "coll/res_index.res":
            bundles[name[len("coll/") : -len(".res")]] = ResourceBundle(package.payload(name)[0]).root

    def parent(name):
        bundle = bundles.get(name, {})
        if "%%Parent" in bundle:
            return bundle["%%Parent"]
        if name == "root":
            return None
        separator = name.rfind("_")
        return name[:separator] if separator >= 0 else "root"

    def open_bundle(name):
        while name not in bundles:
            separator = name.rfind("_")
            if separator < 0:
                return "root"
            name = name[:separator]
        alias = bundles[name].get("%%ALIAS")
        return open_bundle(alias) if alias else name

    def resolve(locale):
        chain = []
        name = open_bundle(locale)
        while name is not None:
            chain.append(name)
            name = parent(name)
        collations = [bundles.get(name, {}).get("collations", {}) for name in chain]
        kind = next(c["default"] for c in collations if isinstance(c, dict) and c.get("default"))
        tailoring = next(name for name, c in zip(chain, collations) if isinstance(c, dict) and kind in c)
        return tailoring, kind

    def icu_name(name):
        parts = name.split("_")
        return "_".join([parts[0]] + [p.upper() if len(p) == 2 else p.title() for p in parts[1:]])

    by_tailoring = {}
    for name in read_collation_names():
        by_tailoring.setdefault(resolve(icu_name(name)), []).append(name)
    aliases = {}
    for name, bundle in bundles.items():
        if bundle.get("%%ALIAS"):
            aliases.setdefault(bundle["%%ALIAS"], []).append(name.lower())
    root = resolve("root")
    rows = []
    unsupported = []
    for locale in sorted(bundles):
        target = resolve(locale)
        if target == root:
            rows.append((locale, "", True))
            continue
        candidates = by_tailoring.get(target)
        if not candidates:
            rows.append((locale, "", False))
            unsupported.append(locale)
            continue
        preferred = next(
            (name for name in [locale.lower()] + sorted(aliases.get(locale, [])) if name in candidates),
            sorted(candidates)[0],
        )
        rows.append((locale, preferred, True))
    out = ["const CollationLocale collation_locales[] = {"]
    for locale, collation, supported in rows:
        out.append('    {"%s", "%s", %s},' % (locale, collation, "true" if supported else "false"))
    out.append("};")
    out.append("const uint32_t collation_locale_count = %d;" % len(rows))
    print("collation locales: %d, without a DuckDB collation: %s" % (len(rows), ", ".join(unsupported)))
    return out


HEADER = """//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_data.cpp
//
// This file is generated by extension/icu/scripts/generate_text_data.py
// from the ICU %s data package and sources and UCD %s. Do not edit.
//
//===----------------------------------------------------------------------===//

#include "text_data.hpp"

namespace duckdb {
namespace text {
"""


def main():
    package = load_icu_package()
    ucd = UCD()
    units = []
    out = [HEADER % (ICU_VERSION, UNICODE_VERSION)]
    out += generate_rule_sets(package)
    out.append("")
    out += generate_break_locales(package)
    out.append("")
    out += generate_engine_sets(ucd, package)
    out.append("")
    out += generate_nfkc_boundaries(package)
    out.append("")
    out += generate_sentence_exceptions(package)
    out.append("")
    out += generate_dictionaries(package, units)
    out.append("")
    normalization_unit = generate_normalization(ucd, units)
    case_unit = generate_case(ucd, units)
    out += generate_greek_upper()
    out.append("")
    out += generate_locales()
    out.append("")
    out += generate_collation_locales(package)
    out.append("")
    out += format_units(units, "text")
    out.append("const uint32_t text_normalization_unit = %d;" % normalization_unit)
    out.append("const uint32_t text_case_unit = %d;" % case_unit)
    out.append('const char *const text_icu_version = "%s";' % ICU_VERSION)
    out.append('const char *const text_unicode_version = "%s";' % UNICODE_VERSION)
    out.append("")
    out.append("} // namespace text")
    out.append("} // namespace duckdb")
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print("wrote %s" % OUTPUT)


if __name__ == "__main__":
    main()
