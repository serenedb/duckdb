import json
import os

scripts_dir = os.path.dirname(os.path.abspath(__file__))
VERSION_MAP_PATH = os.path.join(scripts_dir, "../src/storage/version_map.json")
STORAGE_INFO_PATH = os.path.join(scripts_dir, "../src/storage/storage_info.cpp")
STORAGE_ENUM_PATH = os.path.join(scripts_dir, "../src/include/duckdb/storage/storage_info.hpp")

START_MARKER = "// START OF {type} VERSION INFO"
END_MARKER = "// END OF {type} VERSION INFO"


def to_enum_name(version_name):
    return version_name.upper().replace('.', '_')


# A StorageVersion is <duckdb version number> * 2^32 + <serenedb sequence>, so a plain
# duckdb version has a zero low half and a serenedb version carries the duckdb base it
# was built on. That keeps every ordinary `>=` comparison correct without rewriting it:
# a serenedb version compares above every duckdb version up to its base and below the
# next one, which a flat number could not express. The on-disk form drops the shift when
# the low half is zero, so duckdb files keep duckdb's numbers.
#
# In version_map.json duckdb versions keep their real numbers and a serenedb entry holds
# just its sequence; the shifting happens here. A serenedb entry is built on the duckdb
# version listed above it, so entries interleave in the order they were added:
#     "v2.0.0": 69, "serenedb_v1": 1, "serenedb_v2": 2, "v2.1.0": 70, "serenedb_v3": 3
# Appending never disturbs an earlier entry's value, which matters because those values are
# on disk -- re-basing serenedb_v1 on a duckdb bump would leave every existing datadir
# holding a number that no longer matches any enumerator.
STORAGE_VERSION_SHIFT = 32


def is_serenedb_version(version_name):
    return version_name.startswith('serenedb')


# "the latest version" is two different things, so the enum names both: DUCKDB_LATEST is
# what the "latest" string resolves to (a database duckdb can still read), while
# SERENEDB_LATEST is what our own writers target. Anything meaning "through the newest
# version we support" must name SERENEDB_LATEST.
def shifted_value(duckdb_version):
    return f"{duckdb_version}ULL << {STORAGE_VERSION_SHIFT}"


def generate_storage_enum(storage_versions):
    result = []
    result.append("enum class StorageVersion : uint64_t {")
    # The first enumerator to reach a duckdb version number spells the shift out; every later one
    # with the same number aliases it, and a serenedb version names the base it is built on. So each
    # value appears once and the relationships are visible rather than arithmetic to redo by hand.
    first_with_version = {}
    duckdb_base = ""
    duckdb_latest = ""
    duckdb_latest_at = 1
    serenedb_latest = ""
    # json order, so every enumerator is greater than the one before it, and each "latest"
    # sits right after the newest member of its family and reads as what it points at
    for version_name, number in storage_versions.items():
        if version_name == 'latest':
            continue
        if is_serenedb_version(version_name):
            serenedb_latest = to_enum_name(version_name)
            result.append(f"    {serenedb_latest} = {duckdb_base} + {number},")
            continue
        duckdb_latest = to_enum_name(version_name)
        if number not in first_with_version:
            first_with_version[number] = duckdb_latest
            value = shifted_value(number)
        else:
            value = first_with_version[number]
        duckdb_base = first_with_version[number]
        result.append(f"    {duckdb_latest} = {value},")
        duckdb_latest_at = len(result)
    result.insert(duckdb_latest_at, f"    DUCKDB_LATEST = {duckdb_latest},")
    result.append(f"    SERENEDB_LATEST = {serenedb_latest or 'DUCKDB_LATEST'},")
    result.append(f"    DEPRECATED = {shifted_value(999)},")
    result.append("    INVALID = 0")
    result.append("};")
    return "\n".join(result)


def generate_serialization_enum(serialization_versions):
    result = []
    result.append("enum class SerializationVersionDeprecated : uint64_t {")
    current = ""
    for version_name, serialization_version in serialization_versions.items():
        if version_name == 'latest':
            continue
        result.append(f"    {to_enum_name(version_name)} = {serialization_version},")
        current = serialization_version

    latest = "LATEST"
    result.append(f"    {to_enum_name(latest)} = {current},")
    result.append("    INVALID = UINT64_MAX")
    result.append("};")
    return "\n".join(result)


def generate_storage_array(storage_versions):
    result = []
    result.append("static const StorageVersionInfo storage_version_info[] = {")

    # json order, with each "latest" after the newest member of its family, matching the enum
    latest_at = 1
    for version_name, _ in storage_versions.items():
        if version_name == 'latest':
            continue
        result.append(f'\t{{"{version_name}", StorageVersion::{to_enum_name(version_name)}}},')
        if not is_serenedb_version(version_name):
            latest_at = len(result)
    result.insert(latest_at, '\t{"latest", StorageVersion::DUCKDB_LATEST},')
    result.append('\t{"serenedb_latest", StorageVersion::SERENEDB_LATEST},')
    result.append("\t{nullptr, StorageVersion::INVALID}")
    result.append("};")
    return "\n".join(result)


def generate_serialization_array(serialization_versions):
    result = []
    result.append("static const SerializationVersionInfo serialization_version_info[] = {")

    current = ""
    for version_name, _ in serialization_versions.items():
        if version_name == 'latest':
            continue
        result.append(f'\t{{"{version_name}", SerializationVersionDeprecated::{to_enum_name(version_name)}}},')
        current = version_name

    latest = "latest"
    result.append(f'\t{{"{latest}", SerializationVersionDeprecated::{to_enum_name(current)}}},')
    result.append("\t{nullptr, SerializationVersionDeprecated::INVALID}")
    result.append("};")
    return "\n".join(result)


def update_file(path, marker_type, new_content):
    if not os.path.exists(path):
        print(f"Error: {path} not found.")
        return
    with open(path, "r") as f:
        content = f.read()

    start_marker = START_MARKER.format(type=marker_type.upper())
    end_marker = END_MARKER.format(type=marker_type.upper())

    start_idx = content.find(start_marker)
    end_idx = content.find(end_marker)

    if start_idx == -1 or end_idx == -1:
        print(f"Markers for {marker_type} not found in {path}")
        return

    new_file_content = content[: start_idx + len(start_marker)] + "\n" + new_content + "\n" + content[end_idx:]
    with open(path, "w") as f:
        f.write(new_file_content)


def main():
    if not os.path.exists(VERSION_MAP_PATH):
        print(f"Error: Version map not found at {VERSION_MAP_PATH}")
        return

    with open(VERSION_MAP_PATH, 'r') as json_file:
        version_map = json.load(json_file)

    storage_values = version_map['storage']['values']
    serialization_values = version_map['serialization']['values']

    enum_code = generate_storage_enum(storage_values)
    update_file(STORAGE_ENUM_PATH, "ENUM", enum_code)

    enum_code_serialization = generate_serialization_enum(serialization_values)
    update_file(STORAGE_ENUM_PATH, "SER_ENUM", enum_code_serialization)

    array_code = generate_storage_array(storage_values)
    update_file(STORAGE_INFO_PATH, "STORAGE_ARRAY", array_code)

    ser_array_code = generate_serialization_array(serialization_values)
    update_file(STORAGE_INFO_PATH, "SER_ARRAY", ser_array_code)

    print(f"Successfully updated all version info in {STORAGE_INFO_PATH}")


if __name__ == "__main__":
    main()
