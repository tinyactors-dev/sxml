#!/usr/bin/env python3
"""Generates src/ixml_grammar_table.c, the grammar of Invisible XML grammars.

Usage: tools/generate-ixml-grammar.py tools/ixml-grammar.ixml > src/ixml_grammar_table.c

The input is the grammar of the Invisible XML Specification (ixml
notation). The output is its XML form (the Specification, "IXML in
XML") as a static table of elements and attributes in document order,
which src/ixml.c compiles with the same code that compiles a grammar
given as an XML element (packet 7.18, D215). So sxml reads a grammar in
ixml notation by parsing it with this grammar.

The parser below reads the notation directly; it is a tool, not part of
sxml, and it needs to read only this one grammar (it implements the whole
notation, but checks little). Comments are dropped: the table holds what
the compiler reads.
"""
import sys


class Element:
    def __init__(self, name, attributes=None, children=None):
        self.name = name
        self.attributes = attributes or []
        self.children = children or []


class Parser:
    def __init__(self, text):
        self.text = text
        self.position = 0

    def error(self, message):
        raise SystemExit("ixml-grammar.ixml:%d: %s" % (self.position, message))

    def peek(self):
        return self.text[self.position] if self.position < len(self.text) else ""

    def skip_space(self):
        while True:
            character = self.peek()
            if character in (" ", "\t", "\n", "\r"):
                self.position += 1
            elif character == "{":
                depth = 0
                while True:
                    character = self.peek()
                    if character == "":
                        self.error("unclosed comment")
                    self.position += 1
                    if character == "{":
                        depth += 1
                    elif character == "}":
                        depth -= 1
                        if depth == 0:
                            break
            else:
                return

    def expect(self, character):
        if self.peek() != character:
            self.error("expected %r" % character)
        self.position += 1
        self.skip_space()

    def is_name_start(self, character):
        return character == "_" or character.isalpha()

    def is_name_follower(self, character):
        return (self.is_name_start(character) or character.isdigit()
                or character in "-.\u00b7\u203f\u2040")

    def name(self):
        start = self.position
        if not self.is_name_start(self.peek()):
            self.error("expected a name")
        while self.peek() != "" and self.is_name_follower(self.peek()):
            self.position += 1
        # A name may contain "." (namefollower), but no name of this
        # grammar ends with one: a final "." ends the rule.
        while self.text[self.position - 1] == "." and self.position - 1 > start:
            self.position -= 1
        result = self.text[start:self.position]
        self.skip_space()
        return result

    def mark(self, marks):
        if self.peek() != "" and self.peek() in marks:
            result = self.peek()
            self.position += 1
            self.skip_space()
            return result
        return None

    def string(self):
        quote = self.peek()
        if quote not in ("'", '"'):
            self.error("expected a string")
        self.position += 1
        result = ""
        while True:
            character = self.peek()
            if character == "":
                self.error("unclosed string")
            self.position += 1
            if character == quote:
                if self.peek() == quote:
                    self.position += 1
                    result += quote
                    continue
                break
            result += character
        self.skip_space()
        return result

    def hex(self):
        self.expect_raw("#")
        start = self.position
        while self.peek() != "" and self.peek() in "0123456789abcdefABCDEF":
            self.position += 1
        result = self.text[start:self.position]
        self.skip_space()
        return result

    def expect_raw(self, character):
        if self.peek() != character:
            self.error("expected %r" % character)
        self.position += 1

    def grammar(self):
        self.skip_space()
        children = []
        while self.position < len(self.text):
            children.append(self.rule())
        return Element("ixml", [], children)

    def naming(self, element_name):
        attributes = []
        mark = self.mark("@^-")
        if mark is not None:
            attributes.append(("mark", mark))
        attributes.append(("name", self.name()))
        if self.peek() == ">":
            self.expect(">")
            attributes.append(("alias", self.name()))
        return Element(element_name, attributes)

    def rule(self):
        rule = self.naming("rule")
        if self.peek() not in ("=", ":"):
            self.error("expected = or :")
        self.expect(self.peek())
        rule.children = self.alternatives()
        self.expect(".")
        return rule

    def alternatives(self):
        result = [self.alternative()]
        while self.peek() in (";", "|"):
            self.expect(self.peek())
            result.append(self.alternative())
        return result

    def alternative(self):
        terms = []
        if self.peek() not in (";", "|", ".", ")"):
            terms.append(self.term())
            while self.peek() == ",":
                self.expect(",")
                terms.append(self.term())
        return Element("alt", [], terms)

    def term(self):
        factor = self.factor()
        if self.text.startswith("**", self.position) or \
                self.text.startswith("++", self.position):
            kind = "repeat0" if self.peek() == "*" else "repeat1"
            self.position += 2
            self.skip_space()
            return Element(kind, [], [factor, Element("sep", [], [self.factor()])])
        if self.peek() in ("*", "+"):
            kind = "repeat0" if self.peek() == "*" else "repeat1"
            self.expect(self.peek())
            return Element(kind, [], [factor])
        if self.peek() == "?":
            self.expect("?")
            return Element("option", [], [factor])
        return factor

    def factor(self):
        character = self.peek()
        if character == "(":
            self.expect("(")
            alternatives = Element("alts", [], self.alternatives())
            self.expect(")")
            return alternatives
        if character == "+":
            self.expect("+")
            if self.peek() == "#":
                return Element("insertion", [("hex", self.hex())])
            return Element("insertion", [("string", self.string())])
        start = self.position
        tmark = self.mark("^-")
        attributes = [("tmark", tmark)] if tmark is not None else []
        character = self.peek()
        if character in ("'", '"'):
            return Element("literal", attributes + [("string", self.string())])
        if character == "#":
            return Element("literal", attributes + [("hex", self.hex())])
        if character == "[":
            return Element("inclusion", attributes, self.members())
        if character == "~":
            self.expect("~")
            return Element("exclusion", attributes, self.members())
        self.position = start
        return self.naming("nonterminal")

    def character(self):
        if self.peek() == "#":
            return "#" + self.hex()
        value = self.string()
        if len(value) != 1:
            self.error("a range needs single characters")
        return value

    def members(self):
        self.expect("[")
        members = []
        while self.peek() != "]":
            if self.peek() in (";", "|"):
                self.expect(self.peek())
                continue
            if self.peek() in ("'", '"', "#"):
                if self.peek() == "#":
                    first = "#" + self.hex()
                else:
                    first = self.string()
                if self.peek() == "-" and (len(first) == 1 or first.startswith("#")):
                    self.expect("-")
                    members.append(Element("member", [("from", first),
                                                      ("to", self.character())]))
                elif first.startswith("#"):
                    members.append(Element("member", [("hex", first[1:])]))
                else:
                    members.append(Element("member", [("string", first)]))
            else:
                code_start = self.position
                while self.peek().isalpha():
                    self.position += 1
                code = self.text[code_start:self.position]
                if not code:
                    self.error("expected a member")
                self.skip_space()
                members.append(Element("member", [("code", code)]))
        self.expect("]")
        return members


def c_string(text):
    result = ""
    for byte in text.encode("utf-8"):
        character = chr(byte)
        if character in ('"', "\\"):
            result += "\\" + character
        elif 0x20 <= byte < 0x7F and character != "?":
            result += character
        else:
            result += "\\x%02X\"\"" % byte
    # A hex escape is closed by a string split so the next character is
    # not read as part of it.
    return '"' + result + '"'


def emit(element, lines, depth):
    lines.append("    SXML_IXML_START(%s)," % element.name.upper())
    for name, value in element.attributes:
        lines.append("    SXML_IXML_ATTRIBUTE(%s, %s)," % (name.upper(), c_string(value)))
    for child in element.children:
        emit(child, lines, depth + 1)
    lines.append("    SXML_IXML_END,")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as source:
        grammar = Parser(source.read()).grammar()
    lines = []
    emit(grammar, lines, 0)
    print("""/*
 * The grammar of Invisible XML grammars in its XML form (Invisible XML
 * Specification, "IXML in XML"), as entries in document order.
 * Generated by tools/generate-ixml-grammar.py from tools/ixml-grammar.ixml;
 * do not edit. src/ixml.c compiles it (packet 7.18, D215).
 */
#include "ixml_internal.h"

#define SXML_IXML_START(element)                                                \\
    {.entry_kind = SXML_IXML_ENTRY_KIND_START,                                  \\
     .element_kind = SXML_IXML_ELEMENT_KIND_##element,                          \\
     .attribute_kind = SXML_IXML_ATTRIBUTE_KIND_NONE,                           \\
     .attribute_value = {.bytes = NULL, .byte_count = 0}}
#define SXML_IXML_ATTRIBUTE(attribute, value)                                   \\
    {.entry_kind = SXML_IXML_ENTRY_KIND_ATTRIBUTE,                              \\
     .element_kind = SXML_IXML_ELEMENT_KIND_UNKNOWN,                            \\
     .attribute_kind = SXML_IXML_ATTRIBUTE_KIND_##attribute,                    \\
     .attribute_value = SXML_BYTE_VIEW_INITIALIZER(value)}
#define SXML_IXML_END                                                           \\
    {.entry_kind = SXML_IXML_ENTRY_KIND_END,                                    \\
     .element_kind = SXML_IXML_ELEMENT_KIND_UNKNOWN,                            \\
     .attribute_kind = SXML_IXML_ATTRIBUTE_KIND_NONE,                           \\
     .attribute_value = {.bytes = NULL, .byte_count = 0}}

static const sxml_ixml_grammar_entry ixml_grammar_entries[] = {""")
    print("\n".join(lines))
    print("""};

const sxml_ixml_grammar_entry_view sxml_ixml_grammar_of_grammars = {
    .entries = ixml_grammar_entries,
    .entry_count = sizeof(ixml_grammar_entries) / sizeof(ixml_grammar_entries[0]),
};""")


if __name__ == "__main__":
    main()
