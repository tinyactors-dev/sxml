#!/usr/bin/env python3
"""Generates src/unicode_tables.c, the Unicode data of src/unicode.c.

Usage: tools/generate-unicode-tables.py UCD-DIRECTORY entities.json \\
           > src/unicode_tables.c

UCD-DIRECTORY holds these files of the Unicode Character Database of the
version named in UNICODE_VERSION (https://www.unicode.org/Public/<version>/
ucd/, the last two from its auxiliary/ and emoji/ directories):
UnicodeData.txt, CompositionExclusions.txt, DerivedNormalizationProps.txt,
DerivedCoreProperties.txt, GraphemeBreakProperty.txt and emoji-data.txt.
entities.json is the list of HTML named character references
(https://html.spec.whatwg.org/entities.json).

The output holds (docs/DECISIONS.md D107):
  - the canonical combining class of every codepoint, as sorted runs;
  - the full canonical and full compatibility decompositions of UAX #15
    (the mappings of UnicodeData.txt applied until nothing changes),
    except the Hangul syllables, which src/unicode.c decomposes by the
    arithmetic of The Unicode Standard, section 3.12;
  - the primary composites: the canonical pairs whose composite is not
    Full_Composition_Exclusion (UAX #15, section 1.3);
  - the starters (combining class 0) that are the second codepoint of a
    canonical decomposition of a character not in CompositionExclusions.txt
    (the "composing characters" of F&O 4.0, fn:normalize-unicode, that
    the combining class does not already give; the Hangul vowel and
    trailing jamo are composing too, by section 3.12);
  - per codepoint, the Grapheme_Cluster_Break value, Extended_Pictographic
    and Indic_Conjunct_Break of UAX #29, as sorted runs;
  - the HTML named character references without "&" and ";", sorted by
    name, for fn:char.

The regex tables (tools/generate-regex-tables.py: general categories,
blocks, case variants) are separate data; nothing here repeats them.
"""
import hashlib
import json
import os
import sys

UNICODE_VERSION = "16.0.0"

# The order of sxml_unicode_grapheme_break in src/unicode_internal.h.
GRAPHEME_BREAKS = [
    "Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
    "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT",
]
# The order of sxml_unicode_conjunct_break in src/unicode_internal.h.
CONJUNCT_BREAKS = ["None", "Linker", "Consonant", "Extend"]

HANGUL_SYLLABLE_FIRST = 0xAC00
HANGUL_SYLLABLE_LAST = 0xD7A3
HANGUL_L_FIRST, HANGUL_V_FIRST, HANGUL_T_FIRST = 0x1100, 0x1161, 0x11A7
HANGUL_V_COUNT, HANGUL_T_COUNT = 21, 28

DATA_FILES = [
    "UnicodeData.txt", "CompositionExclusions.txt",
    "DerivedNormalizationProps.txt", "DerivedCoreProperties.txt",
    "GraphemeBreakProperty.txt", "emoji-data.txt",
]


def property_lines(path):
    """(first, last, fields) of each data line of a UCD property file."""
    with open(path, encoding="utf-8") as property_file:
        for line in property_file:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [field.strip() for field in line.split(";")]
            codepoints = fields[0].split("..")
            first = int(codepoints[0], 16)
            last = int(codepoints[-1], 16)
            yield first, last, fields[1:]


def read_unicode_data(path):
    """Combining classes (list by codepoint) and decomposition mappings
    (codepoint to (is_compatibility, codepoints))."""
    classes = [0] * 0x110000
    mappings = {}
    with open(path, encoding="utf-8") as data_file:
        pending_first = None
        for line in data_file:
            fields = line.rstrip("\n").split(";")
            codepoint = int(fields[0], 16)
            name = fields[1]
            combining_class = int(fields[3])
            if name.endswith(", First>"):
                pending_first = codepoint
                continue
            if name.endswith(", Last>"):
                for member in range(pending_first, codepoint + 1):
                    classes[member] = combining_class
                pending_first = None
                continue
            classes[codepoint] = combining_class
            decomposition = fields[5].strip()
            if decomposition:
                is_compatibility = decomposition.startswith("<")
                if is_compatibility:
                    decomposition = decomposition.split(">", 1)[1]
                mappings[codepoint] = (
                    is_compatibility,
                    [int(part, 16) for part in decomposition.split()])
    return classes, mappings


def hangul_decomposition(codepoint):
    """The full decomposition of a Hangul syllable (section 3.12)."""
    index = codepoint - HANGUL_SYLLABLE_FIRST
    result = [HANGUL_L_FIRST + index // (HANGUL_V_COUNT * HANGUL_T_COUNT),
              HANGUL_V_FIRST + (index % (HANGUL_V_COUNT * HANGUL_T_COUNT))
              // HANGUL_T_COUNT]
    if index % HANGUL_T_COUNT:
        result.append(HANGUL_T_FIRST + index % HANGUL_T_COUNT)
    return result


def full_decomposition(codepoint, mappings, with_compatibility):
    """Applies the mappings until no codepoint has one (a work list, as
    src/unicode.c reads the result, not recursion)."""
    pending = [codepoint]
    result = []
    while pending:
        current = pending.pop(0)
        if HANGUL_SYLLABLE_FIRST <= current <= HANGUL_SYLLABLE_LAST:
            result.extend(hangul_decomposition(current))
            continue
        mapping = mappings.get(current)
        if mapping and (with_compatibility or not mapping[0]):
            pending = list(mapping[1]) + pending
            continue
        result.append(current)
    return result


def runs_of(values):
    runs = []
    for codepoint, value in enumerate(values):
        if not runs or runs[-1][1] != value:
            runs.append((codepoint, value))
    return runs


def read_exclusions(path):
    excluded = set()
    for first, last, _ in property_lines(path):
        excluded.update(range(first, last + 1))
    return excluded


def read_full_composition_exclusion(path):
    excluded = set()
    for first, last, fields in property_lines(path):
        if fields[0] == "Full_Composition_Exclusion":
            excluded.update(range(first, last + 1))
    return excluded


def read_grapheme_properties(directory):
    """The packed grapheme value of every codepoint: break value in bits
    0-3, conjunct break in bits 4-5, Extended_Pictographic in bit 6."""
    values = [0] * 0x110000
    for first, last, fields in property_lines(
            os.path.join(directory, "GraphemeBreakProperty.txt")):
        for codepoint in range(first, last + 1):
            values[codepoint] |= GRAPHEME_BREAKS.index(fields[0])
    for first, last, fields in property_lines(
            os.path.join(directory, "DerivedCoreProperties.txt")):
        if fields[0] == "InCB":
            for codepoint in range(first, last + 1):
                values[codepoint] |= CONJUNCT_BREAKS.index(fields[1]) << 4
    for first, last, fields in property_lines(
            os.path.join(directory, "emoji-data.txt")):
        if fields[0] == "Extended_Pictographic":
            for codepoint in range(first, last + 1):
                values[codepoint] |= 1 << 6
    return values


def read_references(path):
    """(name, codepoints) sorted by the UTF-8 bytes of the name."""
    with open(path, encoding="utf-8") as entities_file:
        entities = json.load(entities_file)
    references = {}
    for key, entity in entities.items():
        name = key[1:].rstrip(";")
        codepoints = entity["codepoints"]
        assert len(codepoints) in (1, 2)
        assert references.get(name, codepoints) == codepoints
        references[name] = codepoints
    return sorted(references.items(), key=lambda item: item[0].encode())


def digest_of(path):
    with open(path, "rb") as digest_file:
        return hashlib.sha256(digest_file.read()).hexdigest()


def array_with_count(type_name, name, count_name, rows):
    lines = [f"const {type_name} {name}[] = {{"]
    lines.extend(rows)
    lines.append("};")
    lines.append(f"const size_t {count_name} = sizeof({name}) / "
                 f"sizeof({name}[0]);")
    return "\n".join(lines)


def packed_rows(words, per_line=6):
    rows = []
    for index in range(0, len(words), per_line):
        rows.append("    " + ", ".join(
            f"0x{word:08X}U" for word in words[index:index + per_line]) + ",")
    return rows


def main():
    directory, entities_path = sys.argv[1], sys.argv[2]
    classes, mappings = read_unicode_data(
        os.path.join(directory, "UnicodeData.txt"))
    exclusions = read_exclusions(
        os.path.join(directory, "CompositionExclusions.txt"))
    full_exclusions = read_full_composition_exclusion(
        os.path.join(directory, "DerivedNormalizationProps.txt"))

    pool = []
    pool_index = {}

    def slice_of(codepoints):
        key = tuple(codepoints)
        if key not in pool_index:
            # Reuse an equal run already in the pool.
            pool_index[key] = len(pool)
            pool.extend(codepoints)
        assert len(codepoints) <= 18 and pool_index[key] < (1 << 27)
        return pool_index[key] << 5 | len(codepoints)

    canonical = []
    compatibility = []
    for codepoint in sorted(mappings):
        canonical_full = full_decomposition(codepoint, mappings, False)
        compatibility_full = full_decomposition(codepoint, mappings, True)
        if canonical_full != [codepoint]:
            canonical.append((codepoint, slice_of(canonical_full)))
        if compatibility_full != canonical_full:
            compatibility.append((codepoint, slice_of(compatibility_full)))

    pairs = []
    composing_starters = set()
    for codepoint, (is_compatibility, codepoints) in mappings.items():
        if is_compatibility or len(codepoints) != 2:
            continue
        if codepoint not in full_exclusions:
            pairs.append((codepoints[0], codepoints[1], codepoint))
        if codepoint not in exclusions and classes[codepoints[1]] == 0:
            composing_starters.add(codepoints[1])
    pairs.sort()

    combining_runs = runs_of(classes)
    grapheme_runs = runs_of(read_grapheme_properties(directory))
    references = read_references(entities_path)

    name_text = "".join(name for name, _ in references)
    assert len(name_text) < 65536
    # The few second codepoints are numbered from 1 (0: none), so that an
    # entry takes eight bytes.
    seconds = sorted({codepoints[1] for _, codepoints in references
                      if len(codepoints) == 2})
    assert len(seconds) < 256
    reference_rows = []
    offset = 0
    for name, codepoints in references:
        second_index = (seconds.index(codepoints[1]) + 1
                        if len(codepoints) == 2 else 0)
        reference_rows.append(
            f"    {{{offset}, {len(name)}, {second_index}, "
            f"0x{codepoints[0]:05X}}}, /* {name} */")
        offset += len(name)
    # A brace list, not a string literal: ISO C limits literals to 4,095
    # characters (-Woverlength-strings).
    name_lines = []
    for index in range(0, len(name_text), 16):
        name_lines.append("    " + ", ".join(
            f"'{character}'" for character in name_text[index:index + 16]) + ",")

    digests = [(name, digest_of(os.path.join(directory, name)))
               for name in DATA_FILES]
    digests.append(("entities.json", digest_of(entities_path)))
    digest_lines = []
    for name, digest in digests:
        digest_lines.append(f" *   {name}")
        digest_lines.append(f" *     {digest}")

    output = []
    output.append(f"""/*
 * Unicode data of src/unicode.c (src/unicode_internal.h), generated by
 * tools/generate-unicode-tables.py from Unicode {UNICODE_VERSION} and the
 * HTML named character references. SHA-256 of the inputs:
""" + "\n".join(digest_lines) + """
 * Do not edit; regenerate.
 *
 * UAX #15 (Unicode Normalization Forms): combining classes, full
 * decompositions, primary composites. UAX #29 (Unicode Text
 * Segmentation), section 3.1: Grapheme_Cluster_Break,
 * Extended_Pictographic, Indic_Conjunct_Break. HTML Living Standard,
 * section 13.5 "Named character references" (F&O 4.0, fn:char).
 */
#include "unicode_internal.h"

/* clang-format off */""")
    output.append(array_with_count(
        "uint32_t", "sxml_unicode_combining_class_runs",
        "sxml_unicode_combining_class_run_count",
        packed_rows([codepoint << 8 | value
                     for codepoint, value in combining_runs])))
    output.append(array_with_count(
        "uint32_t", "sxml_unicode_decomposition_codepoints",
        "sxml_unicode_decomposition_codepoint_count", packed_rows(pool)))
    for table_name, entries in (("canonical", canonical),
                                ("compatibility", compatibility)):
        output.append(array_with_count(
            "sxml_unicode_decomposition_entry",
            f"sxml_unicode_{table_name}_decompositions",
            f"sxml_unicode_{table_name}_decomposition_count",
            [f"    {{0x{codepoint:05X}, 0x{packed:08X}U}},"
             for codepoint, packed in entries]))
    output.append(array_with_count(
        "sxml_unicode_composition", "sxml_unicode_compositions",
        "sxml_unicode_composition_count",
        [f"    {{0x{first:05X}, 0x{second:05X}, 0x{composite:05X}}},"
         for first, second, composite in pairs]))
    output.append(array_with_count(
        "uint32_t", "sxml_unicode_composing_starters",
        "sxml_unicode_composing_starter_count",
        packed_rows(sorted(composing_starters))))
    output.append(array_with_count(
        "uint32_t", "sxml_unicode_grapheme_runs",
        "sxml_unicode_grapheme_run_count",
        packed_rows([codepoint << 8 | value
                     for codepoint, value in grapheme_runs])))
    output.append("const char sxml_unicode_reference_name_text[] = {\n" +
                  "\n".join(name_lines) + "\n};")
    output.append(array_with_count(
        "uint32_t", "sxml_unicode_reference_second_codepoints",
        "sxml_unicode_reference_second_codepoint_count", packed_rows(seconds)))
    output.append(array_with_count(
        "sxml_unicode_character_reference",
        "sxml_unicode_character_references",
        "sxml_unicode_character_reference_count", reference_rows))
    output.append("/* clang-format on */\n")
    sys.stdout.write("\n\n".join(output))


if __name__ == "__main__":
    main()
