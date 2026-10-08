# Updating the collation data

The collations of the ICU extension are not produced by ICU. `collation/generated/collation_data.cpp`
holds tables that `generate_collation_data.py` derives from the Unicode Consortium data, and
`collation/` implements the Unicode Collation Algorithm over them. The version of the data the
tables were generated from is at the top of the generator and in the header of the generated file.

## When to update

The Unicode Consortium releases CLDR about twice a year and a Unicode version about once a year.
Most releases touch the collation data: new characters are given weights in the root table, and
locale tailorings are corrected. Nothing forces an update - the pinned versions keep the ordering
identical on every platform, which is the main reason the tables are generated instead of read
from ICU - so updating is a deliberate step, usually taken to pick up new scripts or a tailoring
fix that a user reports.

Since the ordering of existing strings can change, an update is a behavioural change for anyone
who stored sort keys or built an index on a collated column. Treat it like one.

## Updating

The generator needs `python3` and the `zstd` command line tool.

1. Record the current behaviour, so that the effect of the update can be reviewed:

   ```bash
   make release
   python3 extension/icu/scripts/collation_snapshot.py record /tmp/collation_before.tsv
   ```

2. Regenerate the tables with the new versions. `CLDR_VERSION` is a tag of the
   [CLDR repository](https://github.com/unicode-org/cldr/tags), `UNICODE_VERSION` a directory of
   [the UCD](https://www.unicode.org/Public/):

   ```bash
   CLDR_VERSION=release-49 UNICODE_VERSION=18.0.0 \
       python3 extension/icu/scripts/generate_collation_data.py
   ```

   The script downloads the data (cached in `~/.cache/duckdb-collation-data/<version>`), prints
   how many mappings and collations it produced, and rewrites `collation_data.cpp`. Update the
   defaults in the generator so that the next run does not need the variables, and format the
   generated file with `make format-fix`.

3. Rebuild and see what changed:

   ```bash
   make release
   python3 extension/icu/scripts/collation_snapshot.py record /tmp/collation_after.tsv
   python3 extension/icu/scripts/collation_snapshot.py compare /tmp/collation_before.tsv /tmp/collation_after.tsv
   ```

   The comparison lists the collations whose sort keys changed and the code point ranges they
   changed in. Check the ranges against the release notes of CLDR and Unicode: a new script or a
   tailoring fix that the notes mention is expected, a change in a range that the notes do not
   mention is a reason to look at the generator before trusting it.

4. Regenerate the ordering test, whose diff is the second review of the update - it covers the
   strings every collation tailors, including the contractions that the snapshot above does not
   reach:

   ```bash
   python3 extension/icu/scripts/generate_collation_tests.py
   git diff test/sql/collate/collation_tailorings.test_slow
   ```

5. Run the tests, and update the ones that encode an ordering that legitimately changed
   (`collation_sort_keys.test` pins sort keys, `test_icu_extensive.test_slow` orderings from the
   CLDR charts):

   ```bash
   make reldebug
   build/reldebug/test/unittest "test/sql/collate/*"
   build/reldebug/test/unittest
   ```

The generator writes one file, so the update is `collation_data.cpp` plus the two version
constants. The size of the compressed tables is printed at the end of the run; a large jump is
worth understanding before committing.

## The other scripts

- `makedata.sh` builds the vendored ICU data package from `filters.json`. It needs a full ICU
  build and is only needed when the *non-collation* ICU data (time zones, calendars) is updated.
- `strip-data.py` removes items from the data package that is already inlined in
  `stubdata.cpp`, without rebuilding ICU. The collation data was removed with it after the
  collator stopped using ICU: `python3 extension/icu/scripts/strip-data.py coll/`.
- `inline-data.py` turns a data package into the C array in `stubdata.cpp`.

# Updating the text and property data

The text functions in `text/` (word and sentence breaking with the Thai, Lao, Burmese, Khmer and Chinese/Japanese dictionaries, normalization, case mapping, locale identifiers) and the Unicode properties that regular expressions name with `\p{...}` (`properties/`) do not use ICU either. `text/generated/text_data.cpp` and `properties/generated/property_data.cpp` hold tables that `generate_text_data.py` and `generate_property_data.py` read from the ICU data package (break rules, break dictionaries, sentence break exceptions, collation resources, normalization data), from a few ICU sources (the Greek uppercasing data, the ISO tables of locale identifiers, the property names) and from the UCD (normalization, case mapping, properties). The versions are `ICU_VERSION` and `UNICODE_VERSION` in `unicode_inputs.py`; the ICU release has to be the one built on that Unicode version.

The generators need `python3` and the `zstd` command line tool:

```bash
python3 extension/icu/scripts/generate_text_data.py
python3 extension/icu/scripts/generate_property_data.py
```

The inputs are downloaded into `~/.cache/duckdb-text-data/<ICU_VERSION>` (`TEXT_DATA_CACHE` overrides it). `generate_text_data.py` maps locales to the collations of `collation/generated/collation_data.cpp`, so regenerate the collation data first when both change (`TEXT_COLLATION_DATA` points it at another file). Both generators stop with an error when the ICU data changes shape (break rules that need 16-bit rows or look-ahead, new data format versions, changed GreekUpper bits); `icu_data.py` holds the readers to extend then. The size of the compressed units is printed at the end of each run.

An update changes segmentation, normalization and case mapping, so it changes the terms of text indexes built before it. The tests that pin the behaviour live in SereneDB: the Unicode conformance tests in `tests/iresearch/analysis/text/unicode_text_tests.cpp` (WordBreakTest, SentenceBreakTest, NormalizationTest, CaseFolding and SpecialCasing of the UCD in `resources/tests/iresearch/unicode`) and the segmentation, normalization, case mapping and property tests in `tests/sqllogic/sdb/pg/simple/unicode`, whose expectations were recorded from ICU 78.3.
