add_library_unity(
  duckdb_formats
  OBJECT
  csv_encoder.cpp
  csv_grammar.cpp
  csv_scan.cpp
  csv_sniff.cpp
  row_lexer.cpp
  row_ranges.cpp
  value_codecs.cpp
  value_parsers.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_formats>
    PARENT_SCOPE)
