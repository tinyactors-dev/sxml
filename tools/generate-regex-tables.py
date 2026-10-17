#!/usr/bin/env python3
"""Generates src/regex_unicode.c, the Unicode data of the regex engine.

Usage: tools/generate-regex-tables.py UnicodeData.txt SpecialCasing.txt \\
           Blocks.txt > src/regex_unicode.c

The three files are from the Unicode Character Database of the version
named in UNICODE_VERSION (https://www.unicode.org/Public/<version>/ucd/).
The output holds:
  - the general category of every codepoint, as sorted runs of one
    category (unassigned codepoints are Cn), for \\p{..} and \\P{..}
    (XSD 1.1 Part 2, appendix G, "Category escapes"), \\d (= \\p{Nd}) and
    \\w (= [#x0000-#x10FFFF]-[\\p{P}\\p{Z}\\p{C}]);
  - the blocks of Blocks.txt, named without their spaces, for \\p{IsX}
    ("Block escapes"), with the four XSD 1.0 names that Unicode renamed;
  - the case variants of F&O 4.0, section 6.2 (flag i) that the case
    mappings of src/function_library_case.c do not reach: C2 is a case
    variant of C1 when lower-case(C1) = lower-case(C2) or upper-case(C1) =
    upper-case(C2). The mappings are read with the code of
    tools/generate-case-tables.py, so the two tables agree; the matcher
    tries X, lower(X), upper(X), upper(lower(X)), lower(upper(X)) and the
    listed extra variants of X.
"""
import hashlib
import importlib.util
import os
import sys
from collections import defaultdict

UNICODE_VERSION = "16.0.0"

# The order of sxml_regex_general_category in src/regex_internal.h.
CATEGORIES = [
    "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No",
    "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Zs", "Zl", "Zp",
    "Sm", "Sc", "Sk", "So", "Cc", "Cf", "Cs", "Co", "Cn",
]

# XSD 1.0 block names (XML Schema Part 2, appendix F) that later Unicode
# versions renamed; kept so that regular expressions written for them work.
BLOCK_ALIASES = {
    "Greek": "GreekandCoptic",
    "CombiningMarksforSymbols": "CombiningDiacriticalMarksforSymbols",
    "CyrillicSupplementary": "CyrillicSupplement",
    "PrivateUse": "PrivateUseArea",
}

MAXIMUM_VARIANT_COUNT = 8


def load_case_module():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "generate-case-tables.py")
    spec = importlib.util.spec_from_file_location("case_tables", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_categories(path):
    """The category of every codepoint, as a list indexed by codepoint."""
    categories = ["Cn"] * 0x110000
    with open(path, encoding="utf-8") as data_file:
        pending_first = None
        for line in data_file:
            fields = line.rstrip("\n").split(";")
            codepoint = int(fields[0], 16)
            name = fields[1]
            category = fields[2]
            if name.endswith(", First>"):
                pending_first = codepoint
                continue
            if name.endswith(", Last>"):
                for member in range(pending_first, codepoint + 1):
                    categories[member] = category
                pending_first = None
                continue
            categories[codepoint] = category
    return categories


def category_runs(categories):
    runs = []
    for codepoint, category in enumerate(categories):
        if not runs or runs[-1][1] != category:
            runs.append((codepoint, category))
    return runs


def read_blocks(path):
    blocks = []
    with open(path, encoding="utf-8") as blocks_file:
        for line in blocks_file:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            codepoints, name = line.split(";")
            first, last = codepoints.strip().split("..")
            blocks.append((name.strip().replace(" ", ""), int(first, 16),
                           int(last, 16)))
    by_name = {name: (first, last) for name, first, last in blocks}
    for alias, name in BLOCK_ALIASES.items():
        assert alias not in by_name
        first, last = by_name[name]
        blocks.append((alias, first, last))
    return sorted(blocks)


def extra_case_variants(case_module, data_path, special_path):
    upper, lower = case_module.read_simple(data_path)
    case_module.read_special(special_path, upper, lower)

    def lower_of(codepoint):
        return tuple(lower.get(codepoint, [codepoint]))

    def upper_of(codepoint):
        return tuple(upper.get(codepoint, [codepoint]))

    candidates = set(upper) | set(lower)
    for mapping in list(upper.values()) + list(lower.values()):
        if len(mapping) == 1:
            candidates.add(mapping[0])
    lower_groups = defaultdict(set)
    upper_groups = defaultdict(set)
    for codepoint in candidates:
        lower_groups[lower_of(codepoint)].add(codepoint)
        upper_groups[upper_of(codepoint)].add(codepoint)
    extras = []
    for codepoint in sorted(candidates):
        variants = (lower_groups[lower_of(codepoint)]
                    | upper_groups[upper_of(codepoint)])
        reached = {codepoint}
        for mapping in (lower_of(codepoint), upper_of(codepoint)):
            if len(mapping) == 1:
                reached.add(mapping[0])
        if len(lower_of(codepoint)) == 1:
            again = upper_of(lower_of(codepoint)[0])
            if len(again) == 1:
                reached.add(again[0])
        if len(upper_of(codepoint)) == 1:
            again = lower_of(upper_of(codepoint)[0])
            if len(again) == 1:
                reached.add(again[0])
        assert len(variants | reached) <= MAXIMUM_VARIANT_COUNT
        for variant in sorted(variants - reached):
            extras.append((codepoint, variant))
    return extras


def digest_of(path):
    with open(path, "rb") as data_file:
        return hashlib.sha256(data_file.read()).hexdigest()


def emit_category_runs(runs):
    lines = ["static const uint32_t sxml_regex_category_runs[] = {"]
    for codepoint, category in runs:
        lines.append(f"    0x{codepoint:05X}U << 5U | {CATEGORIES.index(category)}U,"
                     f" /* {category} */")
    lines.append("};")
    return "\n".join(lines)


def emit_blocks(blocks):
    lines = ["static const sxml_regex_block sxml_regex_blocks[] = {"]
    for name, first, last in blocks:
        lines.append(f'    {{0x{first:05X}, 0x{last:05X}, "{name}", {len(name)}}},')
    lines.append("};")
    return "\n".join(lines)


def emit_extras(extras):
    lines = ["static const sxml_regex_case_variant_pair "
             "sxml_regex_extra_case_variants[] = {"]
    for codepoint, variant in extras:
        lines.append(f"    {{0x{codepoint:05X}, 0x{variant:05X}}},")
    lines.append("};")
    return "\n".join(lines)


TEMPLATE = """/*
 * Unicode data of the regex engine (src/regex_internal.h), generated by
 * tools/generate-regex-tables.py from Unicode @VERSION@: UnicodeData.txt
 * (SHA-256 @DATA0@
 * @DATA1@), SpecialCasing.txt (SHA-256
 * @SPECIAL0@
 * @SPECIAL1@) and Blocks.txt (SHA-256
 * @BLOCKS0@
 * @BLOCKS1@). Do not edit; regenerate.
 *
 * XSD 1.1 Part 2, appendix G, "Character Class Escapes": category escapes
 * \\p{X} use the general categories, block escapes \\p{IsX} the blocks of
 * Blocks.txt named without spaces; \\d is \\p{Nd} and \\w is
 * [#x0000-#x10FFFF]-[\\p{P}\\p{Z}\\p{C}]. F&O 4.0, section 6.2, flag i: the
 * case variants, read through the case mappings of fn:lower-case and
 * fn:upper-case (src/function_library_case.c).
 */
#include "regex_internal.h"

#include <string.h>

#include "function_library_core_internal.h"

/* A block of Blocks.txt: its range and its name without spaces. */
typedef struct sxml_regex_block {
    uint32_t first_codepoint;
    uint32_t last_codepoint;
    const char *name;
    size_t name_byte_count;
} sxml_regex_block;

/* A case variant the case mappings do not reach from codepoint. */
typedef struct sxml_regex_case_variant_pair {
    uint32_t codepoint;
    uint32_t variant;
} sxml_regex_case_variant_pair;

/* The category runs hold the first codepoint of a run above five bits of
   its category. */
#define SXML_REGEX_CATEGORY_BIT_COUNT 5U
#define SXML_REGEX_CATEGORY_MASK 0x1FU

/* clang-format off */
@CATEGORIES@

@BLOCKS@

@EXTRAS@
/* clang-format on */

#define SXML_REGEX_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

sxml_regex_general_category sxml_regex_get_general_category(
    uint32_t codepoint) {
    /* The last run that starts at or before codepoint; the first run
       starts at 0. */
    size_t low_index = 0;
    size_t high_index = SXML_REGEX_ARRAY_COUNT(sxml_regex_category_runs);
    while (high_index - low_index > 1) {
        size_t middle_index = low_index + (high_index - low_index) / 2;
        uint32_t first_codepoint = sxml_regex_category_runs[middle_index] >>
                                   SXML_REGEX_CATEGORY_BIT_COUNT;
        if (first_codepoint <= codepoint) {
            low_index = middle_index;
        } else {
            high_index = middle_index;
        }
    }
    return (sxml_regex_general_category)(sxml_regex_category_runs[low_index] &
                                         SXML_REGEX_CATEGORY_MASK);
}

bool sxml_regex_is_decimal_digit(uint32_t codepoint) {
    return sxml_regex_get_general_category(codepoint) ==
           SXML_REGEX_GENERAL_CATEGORY_ND;
}

bool sxml_regex_is_word_character(uint32_t codepoint) {
    uint32_t category_bit =
        (uint32_t)1 << (uint32_t)sxml_regex_get_general_category(codepoint);
    return (category_bit & (SXML_REGEX_CATEGORY_MASK_P |
                            SXML_REGEX_CATEGORY_MASK_Z |
                            SXML_REGEX_CATEGORY_MASK_C)) == 0;
}

bool sxml_regex_find_block(sxml_byte_view name,
                           sxml_regex_codepoint_range *range) {
    for (size_t block_index = 0;
         block_index < SXML_REGEX_ARRAY_COUNT(sxml_regex_blocks);
         block_index++) {
        const sxml_regex_block *block = &sxml_regex_blocks[block_index];
        if (block->name_byte_count == name.byte_count &&
            memcmp(block->name, name.bytes, name.byte_count) == 0) {
            *range = (sxml_regex_codepoint_range){
                .first_codepoint = block->first_codepoint,
                .last_codepoint = block->last_codepoint,
            };
            return true;
        }
    }
    return false;
}

/* The single codepoint codepoint maps to, or false when its mapping is
   several codepoints (which no single character equals). */
static bool sxml_regex_map_case_to_single(uint32_t codepoint,
                                          sxml_case_mapping_kind mapping_kind,
                                          uint32_t *mapped_codepoint) {
    sxml_case_mapping mapping = {.codepoint_count = 0};
    sxml_function_library_map_case(
        (sxml_case_mapping_request){.codepoint = codepoint,
                                    .mapping_kind = mapping_kind},
        &mapping);
    if (mapping.codepoint_count != 1) {
        return false;
    }
    *mapped_codepoint = mapping.codepoints[0];
    return true;
}

/* Adds codepoint to variants unless it is there already. */
static void sxml_regex_add_case_variant(sxml_regex_case_variants *variants,
                                        uint32_t codepoint) {
    for (size_t variant_index = 0; variant_index < variants->codepoint_count;
         variant_index++) {
        if (variants->codepoints[variant_index] == codepoint) {
            return;
        }
    }
    if (variants->codepoint_count < SXML_REGEX_MAXIMUM_CASE_VARIANT_COUNT) {
        variants->codepoints[variants->codepoint_count] = codepoint;
        variants->codepoint_count += 1;
    }
}

bool sxml_regex_are_case_variants(uint32_t first_codepoint,
                                  uint32_t second_codepoint) {
    if (first_codepoint == second_codepoint) {
        return true;
    }
    static const sxml_case_mapping_kind mapping_kinds[] = {
        SXML_CASE_MAPPING_KIND_LOWER,
        SXML_CASE_MAPPING_KIND_UPPER,
    };
    for (size_t kind_index = 0;
         kind_index < SXML_REGEX_ARRAY_COUNT(mapping_kinds); kind_index++) {
        sxml_case_mapping first_mapping = {.codepoint_count = 0};
        sxml_case_mapping second_mapping = {.codepoint_count = 0};
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = first_codepoint,
                                        .mapping_kind = mapping_kinds[kind_index]},
            &first_mapping);
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = second_codepoint,
                                        .mapping_kind = mapping_kinds[kind_index]},
            &second_mapping);
        if (first_mapping.codepoint_count == second_mapping.codepoint_count &&
            memcmp(first_mapping.codepoints, second_mapping.codepoints,
                   first_mapping.codepoint_count * sizeof(uint32_t)) == 0) {
            return true;
        }
    }
    return false;
}

void sxml_regex_find_case_variants(uint32_t codepoint,
                                   sxml_regex_case_variants *variants) {
    sxml_regex_case_variants found = {.codepoint_count = 0};
    sxml_regex_add_case_variant(&found, codepoint);
    uint32_t candidates[4] = {0, 0, 0, 0};
    size_t candidate_count = 0;
    uint32_t lower_codepoint = 0;
    uint32_t upper_codepoint = 0;
    uint32_t again_codepoint = 0;
    if (sxml_regex_map_case_to_single(codepoint, SXML_CASE_MAPPING_KIND_LOWER,
                                      &lower_codepoint)) {
        candidates[candidate_count++] = lower_codepoint;
        if (sxml_regex_map_case_to_single(lower_codepoint,
                                          SXML_CASE_MAPPING_KIND_UPPER,
                                          &again_codepoint)) {
            candidates[candidate_count++] = again_codepoint;
        }
    }
    if (sxml_regex_map_case_to_single(codepoint, SXML_CASE_MAPPING_KIND_UPPER,
                                      &upper_codepoint)) {
        candidates[candidate_count++] = upper_codepoint;
        if (sxml_regex_map_case_to_single(upper_codepoint,
                                          SXML_CASE_MAPPING_KIND_LOWER,
                                          &again_codepoint)) {
            candidates[candidate_count++] = again_codepoint;
        }
    }
    for (size_t candidate_index = 0; candidate_index < candidate_count;
         candidate_index++) {
        if (sxml_regex_are_case_variants(codepoint,
                                         candidates[candidate_index])) {
            sxml_regex_add_case_variant(&found, candidates[candidate_index]);
        }
    }
    /* The pairs are sorted by codepoint: find the first of codepoint. */
    size_t low_index = 0;
    size_t high_index = SXML_REGEX_ARRAY_COUNT(sxml_regex_extra_case_variants);
    while (low_index < high_index) {
        size_t middle_index = low_index + (high_index - low_index) / 2;
        if (sxml_regex_extra_case_variants[middle_index].codepoint < codepoint) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    for (size_t pair_index = low_index;
         pair_index < SXML_REGEX_ARRAY_COUNT(sxml_regex_extra_case_variants) &&
         sxml_regex_extra_case_variants[pair_index].codepoint == codepoint;
         pair_index++) {
        sxml_regex_add_case_variant(
            &found, sxml_regex_extra_case_variants[pair_index].variant);
    }
    *variants = found;
}
"""


def main():
    data_path, special_path, blocks_path = sys.argv[1], sys.argv[2], sys.argv[3]
    case_module = load_case_module()
    runs = category_runs(read_categories(data_path))
    blocks = read_blocks(blocks_path)
    extras = extra_case_variants(case_module, data_path, special_path)
    data_digest = digest_of(data_path)
    special_digest = digest_of(special_path)
    blocks_digest = digest_of(blocks_path)
    output = TEMPLATE
    for key, value in (
        ("@VERSION@", UNICODE_VERSION),
        ("@DATA0@", data_digest[:32]), ("@DATA1@", data_digest[32:]),
        ("@SPECIAL0@", special_digest[:32]), ("@SPECIAL1@", special_digest[32:]),
        ("@BLOCKS0@", blocks_digest[:32]), ("@BLOCKS1@", blocks_digest[32:]),
        ("@CATEGORIES@", emit_category_runs(runs)),
        ("@BLOCKS@", emit_blocks(blocks)),
        ("@EXTRAS@", emit_extras(extras)),
    ):
        output = output.replace(key, value)
    sys.stdout.write(output)


if __name__ == "__main__":
    main()
