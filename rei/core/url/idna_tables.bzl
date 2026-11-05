"""Generates the Unicode tables behind IDNA processing in core::url.

This is a repository rule so the tables can be generated in Starlark straight from the Unicode data files,
which it downloads. MODULE.bazel pins the Unicode version and the hash of every file.
"""

_MAPPING_TABLE = "idna/IdnaMappingTable.txt"
_UNICODE_DATA = "ucd/UnicodeData.txt"
_NORMALIZATION_PROPS = "ucd/DerivedNormalizationProps.txt"
_BIDI_CLASS = "ucd/extracted/DerivedBidiClass.txt"
_COMBINING_CLASS = "ucd/extracted/DerivedCombiningClass.txt"
_GENERAL_CATEGORY = "ucd/extracted/DerivedGeneralCategory.txt"
_JOINING_TYPE = "ucd/extracted/DerivedJoiningType.txt"

# RFC 5893 tells these classes apart and disallows the rest in every label, so they become `other`. L is the
# default and isn't stored.
_BIDI_CLASSES = {
    "R": "right_to_left",
    "AL": "arabic_letter",
    "AN": "arabic_number",
    "EN": "european_number",
    "ES": "european_separator",
    "ET": "european_terminator",
    "CS": "common_separator",
    "ON": "other_neutral",
    "BN": "boundary_neutral",
    "NSM": "nonspacing_mark",
    "B": "other",
    "S": "other",
    "WS": "other",
    "LRE": "other",
    "LRO": "other",
    "RLE": "other",
    "RLO": "other",
    "PDF": "other",
    "LRI": "other",
    "RLI": "other",
    "FSI": "other",
    "PDI": "other",
}

_JOINING_TYPES = {
    "C": "join_causing",
    "D": "dual",
    "L": "left",
    "R": "right",
    "T": "transparent",
}

def _data_lines(text):
    """Returns the semicolon-separated fields of every line that isn't blank or a comment."""
    lines = []
    for line in text.splitlines():
        line = line.partition("#")[0].strip()
        if line:
            lines.append([field.strip() for field in line.split(";")])
    return lines

def _code_points(text):
    """Returns the first and last code point of a "XXXX" or "XXXX..YYYY" field."""
    first, _, last = text.partition("..")
    return int(first, 16), int(last or first, 16)

def _merge(rows):
    """Sorts [first, last, value...] rows and merges neighbors whose values are equal."""
    merged = []
    for row in sorted(rows):
        if merged and merged[-1][1] + 1 == row[0] and merged[-1][2:] == row[2:]:
            merged[-1][1] = row[1]
        else:
            merged.append(row)
    return merged

def _mapping_table(text):
    """Returns the mapping rows and the code points their mappings point into."""
    rows = []
    mapped = []
    for fields in _data_lines(text):
        first, last = _code_points(fields[0])
        status = fields[1]
        if status == "mapped":
            values = [int(value, 16) for value in fields[2].split(" ") if value]
            rows.append([first, last, "mapped", len(mapped), len(values)])
            mapped += values
        else:
            # Nontransitional processing leaves deviations as they are, just like valid code points.
            rows.append([first, last, "valid" if status == "deviation" else status, 0, 0])

    # Mapped rows each point at their own mapping, so only rows without one merge.
    rows = _merge(rows)
    if rows[0][0] != 0 or rows[-1][1] != 0x10FFFF or [r for r, n in zip(rows, rows[1:]) if r[1] + 1 != n[0]]:
        fail("%s doesn't cover every code point" % _MAPPING_TABLE)

    return rows, mapped

def _property_table(text, values):
    """Returns merged [first, last, value] rows for the code points whose value is a key of `values`."""
    rows = []
    for fields in _data_lines(text):
        if fields[1] in values:
            first, last = _code_points(fields[0])
            rows.append([first, last, values[fields[1]]])
    return _merge(rows)

def _normalization_tables(unicode_data, normalization_props):
    """Returns the full canonical decompositions and the primary composites as [first, second, composite]."""
    decompositions = {}
    for line in unicode_data.splitlines():
        fields = line.split(";")
        if fields[5] and not fields[5].startswith("<"):
            decompositions[int(fields[0], 16)] = [int(value, 16) for value in fields[5].split(" ") if value]

    excluded = {}
    for fields in _data_lines(normalization_props):
        if fields[1] == "Full_Composition_Exclusion":
            first, last = _code_points(fields[0])
            for code_point in range(first, last + 1):
                excluded[code_point] = True

    compositions = [
        [parts[0], parts[1], code_point]
        for code_point, parts in decompositions.items()
        if len(parts) == 2 and code_point not in excluded
    ]

    # Starlark has no recursion or while loops, so expand every decomposition one level per pass. Canonical
    # decompositions only nest a few levels deep.
    for _ in range(8):
        changed = False
        for code_point, parts in decompositions.items():
            expanded = []
            for part in parts:
                expanded += decompositions.get(part, [part])
            if expanded != parts:
                decompositions[code_point] = expanded
                changed = True
        if not changed:
            break
    if changed:
        fail("canonical decompositions in %s nest deeper than expected" % _UNICODE_DATA)

    return decompositions, sorted(compositions)

def _array(element, name, rows, format):
    lines = ["        constexpr std::array<%s, %d> %s{{" % (element, len(rows), name)]
    lines += ["            " + format(row) + "," for row in rows]
    lines.append("        }};")
    return "\n".join(lines)

def _code_point_array(name, code_points):
    lines = ["        constexpr std::array<char32_t, %d> %s{" % (len(code_points), name)]
    for i in range(0, len(code_points), 12):
        lines.append("            " + " ".join(["0x%X," % value for value in code_points[i:i + 12]]))
    lines.append("        };")
    return "\n".join(lines)

def _range(row):
    return "{0x%X, 0x%X}" % (row[0], row[1])

def _property(row):
    return "{0x%X, 0x%X, %s}" % (row[0], row[1], row[2])

def _mapping(row):
    return "{0x%X, 0x%X, status::%s, %d, %d}" % tuple(row)

def _decomposition(row):
    return "{0x%X, %d, %d}" % tuple(row)

def _composition(row):
    return "{0x%X, 0x%X, 0x%X}" % tuple(row)

def _idna_tables_impl(rctx):
    data = {}
    for path in [_MAPPING_TABLE, _UNICODE_DATA, _NORMALIZATION_PROPS, _BIDI_CLASS, _COMBINING_CLASS,
                 _GENERAL_CATEGORY, _JOINING_TYPE]:
        if path not in rctx.attr.sha256:
            fail("idna_tables needs a sha256 for " + path)

        rctx.download(
            url = "https://www.unicode.org/Public/%s/%s" % (rctx.attr.version, path),
            output = path,
            sha256 = rctx.attr.sha256[path],
        )
        data[path] = rctx.read(path)

    mappings, mapped = _mapping_table(data[_MAPPING_TABLE])
    bidi_classes = _property_table(
        data[_BIDI_CLASS],
        {name: "bidi_class::" + value for name, value in _BIDI_CLASSES.items()},
    )
    joining_types = _property_table(
        data[_JOINING_TYPE],
        {name: "joining_type::" + value for name, value in _JOINING_TYPES.items()},
    )
    combining_classes = _property_table(data[_COMBINING_CLASS], {str(value): str(value) for value in range(1, 256)})
    combining_marks = _property_table(data[_GENERAL_CATEGORY], {"Mn": "", "Mc": "", "Me": ""})
    decompositions, compositions = _normalization_tables(data[_UNICODE_DATA], data[_NORMALIZATION_PROPS])

    decomposition_rows = []
    decomposed = []
    for code_point, parts in sorted(decompositions.items()):
        decomposition_rows.append([code_point, len(decomposed), len(parts)])
        decomposed += parts

    rctx.file("BUILD.bazel", 'exports_files(["idna_tables.cpp"])\n')
    rctx.file("idna_tables.cpp", """\
// Generated by rei/core/url/idna_tables.bzl from Unicode {version} data. Do not edit.

#include <array>
#include <cstdint>
#include <span>

#include "rei/core/url/idna.hpp"

namespace rei::core::detail::idna {{
    namespace {{
{arrays}
    }} // namespace

    const std::span<const idna::mapping> mappings = mapping_data;
    const std::span<const char32_t> mapped_code_points = mapped_data;
    const std::span<const idna::property<idna::bidi_class>> bidi_classes = bidi_class_data;
    const std::span<const idna::property<idna::joining_type>> joining_types = joining_type_data;
    const std::span<const idna::property<std::uint8_t>> combining_classes = combining_class_data;
    const std::span<const idna::code_point_range> combining_marks = combining_mark_data;
    const std::span<const idna::decomposition> decompositions = decomposition_data;
    const std::span<const char32_t> decomposed_code_points = decomposed_data;
    const std::span<const idna::composition> compositions = composition_data;
}} // namespace rei::core::detail::idna
""".format(
        version = rctx.attr.version,
        arrays = "\n\n".join([
            _array("idna::mapping", "mapping_data", mappings, _mapping),
            _code_point_array("mapped_data", mapped),
            _array("idna::property<idna::bidi_class>", "bidi_class_data", bidi_classes, _property),
            _array("idna::property<idna::joining_type>", "joining_type_data", joining_types, _property),
            _array("idna::property<std::uint8_t>", "combining_class_data", combining_classes, _property),
            _array("idna::code_point_range", "combining_mark_data", combining_marks, _range),
            _array("idna::decomposition", "decomposition_data", decomposition_rows, _decomposition),
            _code_point_array("decomposed_data", decomposed),
            _array("idna::composition", "composition_data", compositions, _composition),
        ]),
    ))

idna_tables = repository_rule(
    implementation = _idna_tables_impl,
    attrs = {
        "version": attr.string(mandatory = True),
        "sha256": attr.string_dict(mandatory = True),
    },
    doc = "Downloads pinned Unicode data and generates idna_tables.cpp for //rei/core:idna.",
)
