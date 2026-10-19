#!/usr/bin/env python3
"""Write the seed corpora of the fuzz targets (tests/fuzz/) from the vendored test suites.

Usage: tools/fuzz-seeds.py OUTPUT-DIRECTORY [--maximum-byte-count N]

Writes one directory per fuzz target under OUTPUT-DIRECTORY, one file per
seed, named by the SHA-1 of its content (the libFuzzer convention):

  xml_parse_fuzz       every XML document of third_party/xmlconf and the
                       QT4 source documents (third_party/qt4tests)
  xpath_compile_fuzz   the inline query of every QT4 test case
  xpath_evaluate_fuzz  the same queries
  xquery_fuzz          the same queries and every .xq file of the QT4
                       test sets
  xslt_fuzz            every .xsl file of the xslt40-test suite
                       (third_party/xslt40-test), streaming tests
                       included
  regex_fuzz           the input, regex and flags of fn:matches,
                       fn:replace, fn:tokenize and fn:analyze-string
                       calls with literal arguments, in the layout of
                       tests/fuzz/regex_fuzz.c (flags byte, regex, zero
                       byte, input)
  cast_fuzz            the argument of xs:TYPE("...") constructor calls,
                       in the layout of tests/fuzz/cast_fuzz.c (target
                       byte, text); the targets are read from that file
  schema_fuzz          the first schema document of every test group of
                       the XSD suite (third_party/xsdtests/suite.xml),
                       alone and, in the layout of tests/fuzz/schema_fuzz.c
                       (schema, zero byte, instance), with each instance
                       document of the group

Seeds longer than --maximum-byte-count (default 16384) are skipped; of
more than --maximum-seed-count (default 4000) seeds for one target, an
evenly spaced sample (by name) is written, so that a fuzz run does not
spend its time reading them.
"""

import argparse
import hashlib
import os
import re
import sys
import xml.etree.ElementTree as element_tree

REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOG_NAMESPACE = "{http://www.w3.org/2010/09/qt-fots-catalog}"
XSD_SUITE_NAMESPACE = "{http://www.w3.org/XML/2004/xml-schema-test-suite/}"
XLINK_HREF = "{http://www.w3.org/1999/xlink}href"
STRING_LITERAL = r'"(?:[^"]|"")*"|\'(?:[^\']|\'\')*\''
REGEX_FLAG_BITS = {"s": 1, "m": 2, "i": 4, "x": 8, "q": 16, "c": 32}


def string_literal_value(literal):
    """Return the value of an XPath string literal, with doubled quotes undone."""
    quote = literal[0]
    return literal[1:-1].replace(quote + quote, quote)


class Corpus:
    def __init__(self, directory, limits):
        self.directory = directory
        self.limits = limits
        self.seeds = {}

    def add(self, content):
        if len(content) <= self.limits.maximum_byte_count:
            self.seeds.setdefault(hashlib.sha1(content).hexdigest(), content)

    def write(self):
        """Write the seeds, an evenly spaced sample of at most maximum_seed_count of them."""
        os.makedirs(self.directory, exist_ok=True)
        names = sorted(self.seeds)
        stride = max(1, -(-len(names) // self.limits.maximum_seed_count))
        written = names[::stride]
        for name in written:
            with open(os.path.join(self.directory, name), "wb") as seed_file:
                seed_file.write(self.seeds[name])
        return len(written), len(names)


def walk_files(directory, extensions):
    for root, _, file_names in os.walk(directory):
        for file_name in sorted(file_names):
            if file_name.endswith(extensions):
                yield os.path.join(root, file_name)


def qt4_test_texts(suite_directory):
    """Yield the inline <test> text of every test case of the QT4 test sets."""
    for path in walk_files(suite_directory, (".xml",)):
        try:
            root = element_tree.parse(path).getroot()
        except (element_tree.ParseError, LookupError, ValueError):
            continue
        if root.tag != CATALOG_NAMESPACE + "test-set":
            continue
        for test in root.iter(CATALOG_NAMESPACE + "test"):
            if test.get("file") is None and test.text:
                yield test.text


def xsd_group_documents(suite_directory):
    """Yield (schema path, instance paths) for each test group of the XSD
    suite whose schema test names a schema document."""
    suite_path = os.path.join(suite_directory, "suite.xml")
    try:
        suite = element_tree.parse(suite_path).getroot()
    except (element_tree.ParseError, OSError):
        return
    for reference in suite.iter(XSD_SUITE_NAMESPACE + "testSetRef"):
        test_set_path = os.path.normpath(os.path.join(suite_directory, reference.get(XLINK_HREF, "")))
        try:
            test_set = element_tree.parse(test_set_path).getroot()
        except (element_tree.ParseError, OSError):
            continue
        base = os.path.dirname(test_set_path)
        for group in test_set.iter(XSD_SUITE_NAMESPACE + "testGroup"):
            schema_documents = [document.get(XLINK_HREF)
                                for test in group.iter(XSD_SUITE_NAMESPACE + "schemaTest")
                                for document in test.iter(XSD_SUITE_NAMESPACE + "schemaDocument")]
            if not schema_documents or schema_documents[0] is None:
                continue
            instances = [os.path.normpath(os.path.join(base, document.get(XLINK_HREF)))
                         for test in group.iter(XSD_SUITE_NAMESPACE + "instanceTest")
                         for document in test.iter(XSD_SUITE_NAMESPACE + "instanceDocument")
                         if document.get(XLINK_HREF) is not None]
            yield os.path.normpath(os.path.join(base, schema_documents[0])), instances


def read_bytes(path):
    try:
        with open(path, "rb") as document:
            return document.read()
    except OSError:
        return None


def cast_target_names():
    """Return the local names of the rows of cast_targets in tests/fuzz/cast_fuzz.c."""
    with open(os.path.join(REPOSITORY_ROOT, "tests", "fuzz", "cast_fuzz.c")) as source:
        text = source.read()
    return re.findall(r'\{SXML_TYPE_[A-Z0-9_]+, "([A-Za-z0-9]+)"\}', text)


def add_regex_seeds(corpus, text):
    call = re.compile(
        r"\b(matches|replace|tokenize|analyze-string)\(\s*(" + STRING_LITERAL + r")\s*,\s*("
        + STRING_LITERAL + r")((?:\s*,\s*(?:" + STRING_LITERAL + r"))*)\s*\)")
    for match in call.finditer(text):
        function_name = match.group(1)
        input_text = string_literal_value(match.group(2))
        regex_text = string_literal_value(match.group(3))
        rest = [string_literal_value(literal)
                for literal in re.findall(STRING_LITERAL, match.group(4))]
        flags_text = ""
        if function_name == "replace":
            flags_text = rest[1] if len(rest) > 1 else ""
        elif rest:
            flags_text = rest[0]
        flags_byte = 0
        for letter in flags_text:
            flags_byte |= REGEX_FLAG_BITS.get(letter, 0)
        corpus.add(bytes([flags_byte]) + regex_text.encode("utf-8") + b"\0"
                   + input_text.encode("utf-8"))


def add_cast_seeds(corpus, text, target_numbers):
    constructor = re.compile(r"\bxs:([A-Za-z0-9]+)\(\s*(" + STRING_LITERAL + r")\s*\)")
    for match in constructor.finditer(text):
        target_number = target_numbers.get(match.group(1))
        if target_number is not None:
            corpus.add(bytes([target_number])
                       + string_literal_value(match.group(2)).encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output_directory")
    parser.add_argument("--maximum-byte-count", type=int, default=16384)
    parser.add_argument("--maximum-seed-count", type=int, default=4000)
    arguments = parser.parse_args()

    def corpus(target):
        return Corpus(os.path.join(arguments.output_directory, target), arguments)

    xml_corpus = corpus("xml_parse_fuzz")
    compile_corpus = corpus("xpath_compile_fuzz")
    evaluate_corpus = corpus("xpath_evaluate_fuzz")
    xquery_corpus = corpus("xquery_fuzz")
    xslt_corpus = corpus("xslt_fuzz")
    regex_corpus = corpus("regex_fuzz")
    cast_corpus = corpus("cast_fuzz")
    schema_corpus = corpus("schema_fuzz")
    target_numbers = {name: number for number, name in enumerate(cast_target_names())}
    if not target_numbers:
        sys.exit("no cast targets found in tests/fuzz/cast_fuzz.c")

    third_party = os.path.join(REPOSITORY_ROOT, "third_party")
    for path in walk_files(os.path.join(third_party, "xmlconf"), (".xml",)):
        with open(path, "rb") as document:
            xml_corpus.add(document.read())
    qt4_directory = os.path.join(third_party, "qt4tests")
    for path in walk_files(os.path.join(qt4_directory, "docs"), (".xml",)):
        with open(path, "rb") as document:
            xml_corpus.add(document.read())
    for text in qt4_test_texts(qt4_directory):
        query = text.strip().encode("utf-8")
        compile_corpus.add(query)
        evaluate_corpus.add(query)
        xquery_corpus.add(query)
        add_regex_seeds(regex_corpus, text)
        add_cast_seeds(cast_corpus, text, target_numbers)

    for path in walk_files(qt4_directory, (".xq",)):
        with open(path, "rb") as module:
            xquery_corpus.add(module.read())

    for path in walk_files(os.path.join(third_party, "xslt40-test"), (".xsl",)):
        with open(path, "rb") as module:
            xslt_corpus.add(module.read())

    for schema_path, instance_paths in xsd_group_documents(os.path.join(third_party, "xsdtests")):
        schema = read_bytes(schema_path)
        if schema is None or b"\0" in schema:
            continue
        schema_corpus.add(schema)
        for instance_path in instance_paths:
            instance = read_bytes(instance_path)
            if instance is not None:
                schema_corpus.add(schema + b"\0" + instance)

    for seeds in (xml_corpus, compile_corpus, evaluate_corpus, xquery_corpus, xslt_corpus,
                  regex_corpus, cast_corpus, schema_corpus):
        written_count, found_count = seeds.write()
        print(f"{seeds.directory}: {written_count} of {found_count} seeds")


if __name__ == "__main__":
    main()
