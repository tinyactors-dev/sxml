#!/usr/bin/env python3
"""Generates src/full_text_stem_table.c, the Snowball stemmers of Full Text.

Usage: tools/generate-snowball-stemmers.py third_party/snowball > src/full_text_stem_table.c

The input is the directory of the vendored Snowball algorithms (BSD
licensed, third_party/snowball/COPYING): english.sbl, german.sbl,
french.sbl, italian.sbl, spanish.sbl and dutch.sbl, the languages of
D208. Each is compiled, as a regex compiles (D53), into a program for the
stemming machine of src/full_text_stem.c (packet 7.12c, D214): a flat
array of instructions with jumps, the strings and groupings it names and
its among tables. The machine runs it over a word's code points in one
loop with an explicit return stack, so no C recursion is involved (D71).

The parser reads the subset of the Snowball language the six algorithms
use and refuses anything else: commands, among with actions (no routine
conditions), groupings with + and -, integers, booleans and string
variables, stringdef and the {} escapes. The semantics follow the Snowball
manual and its runtimes (the Python runtime of snowballstemmer 3.1.1 was
the reference: every word of the snowball-data vocabularies stems as it
does). This is a tool, not part of sxml.
"""
import os
import sys

LANGUAGES = ["english", "german", "french", "italian", "spanish", "dutch"]

# Instructions, each an opcode and its operands (16-bit words). Keep in step
# with the enum in src/full_text_internal.h.
OPCODES = [
    ("JUMP", 1), ("JUMP_IF_TRUE", 1), ("JUMP_IF_FALSE", 1), ("TRUE", 0),
    ("FALSE", 0), ("SAVE", 1), ("RESTORE", 1), ("LIMIT_SAVE", 1),
    ("LIMIT_RESTORE", 1), ("BACKWARD_BEGIN", 0), ("BACKWARD_END", 0),
    ("LITERAL", 1), ("IN_GROUPING", 1), ("OUT_GROUPING", 1), ("NEXT", 0),
    ("HOP", 1), ("AT_LIMIT", 0), ("OPEN_SLICE", 0), ("CLOSE_SLICE", 0),
    ("REPLACE", 1), ("SLICE_TO", 1), ("INSERT", 1), ("INSERT_VARIABLE", 1),
    ("ATTACH", 1), ("ATTACH_VARIABLE", 1), ("SET_MARK", 1), ("TO_MARK", 1),
    ("AT_MARK", 1), ("INTEGER_ASSIGN", 3), ("INTEGER_COMPARE", 4),
    ("BOOLEAN_SET", 2), ("BOOLEAN_TEST", 1), ("CALL", 1), ("RETURN", 0),
    ("AMONG_FIND", 1), ("AMONG_DISPATCH", 2),
]
OPCODE = {name: number for number, (name, _) in enumerate(OPCODES)}
OPERAND_COUNT = {name: count for name, count in OPCODES}

# Integer expression kinds and relations (src/full_text_internal.h).
EXPRESSION_KINDS = {"constant": 0, "cursor": 1, "limit": 2, "variable": 3}
RELATIONS = {"==": 0, "!=": 1, "<": 2, "<=": 3, ">": 4, ">=": 5}

PREFIX_COMMANDS = {"not", "try", "test", "do", "fail", "repeat", "goto",
                   "gopast", "backwards"}


class SnowballError(Exception):
    pass


# ------------------------------------------------------------------ Lexer

def tokenize(text, file_name):
    tokens = []
    position = 0
    length = len(text)
    while position < length:
        character = text[position]
        if character.isspace():
            position += 1
        elif text.startswith("//", position):
            end = text.find("\n", position)
            position = length if end < 0 else end
        elif text.startswith("/*", position):
            end = text.find("*/", position)
            if end < 0:
                raise SnowballError("%s: unclosed comment" % file_name)
            position = end + 2
        elif character == "'":
            # A string literal; {name} escapes are kept raw and resolved
            # by the parser (stringdef may define them later in the file).
            end = position + 1
            parts = []
            while True:
                if end >= length:
                    raise SnowballError("%s: unclosed string" % file_name)
                if text[end] == "{":
                    close = text.find("}", end + 1)
                    if close < 0:
                        raise SnowballError("%s: unclosed escape" % file_name)
                    parts.append(("escape", text[end + 1:close]))
                    end = close + 1
                elif text[end] == "'":
                    break
                else:
                    parts.append(("text", text[end]))
                    end += 1
            tokens.append(("string", parts))
            position = end + 1
        elif character.isalnum() or character == "_":
            end = position
            while end < length and (text[end].isalnum() or text[end] == "_"):
                end += 1
            word = text[position:end]
            tokens.append(("number" if word.isdigit() else "name", word))
            position = end
            if word == "stringdef":
                # The name of a stringdef is any run of non-space
                # characters (a", e', ss).
                while position < length and text[position].isspace():
                    position += 1
                end = position
                while end < length and not text[end].isspace():
                    end += 1
                tokens.append(("stringdef_name", text[position:end]))
                position = end
        else:
            for symbol in ("<-", "->", "<=", ">=", "==", "!=", "(", ")", "[",
                           "]", "$", "=", "<", ">", "+", "-", "{", "}", "*",
                           "/"):
                if text.startswith(symbol, position):
                    tokens.append(("symbol", symbol))
                    position += len(symbol)
                    break
            else:
                raise SnowballError("%s: unexpected %r" % (file_name, character))
    return tokens


# ----------------------------------------------------------------- Parser

class Parser:
    def __init__(self, tokens, file_name):
        self.tokens = tokens
        self.position = 0
        self.file_name = file_name
        self.stringdefs = {"'": "'"}
        self.routines = set()
        self.externals = set()
        self.integers = []
        self.booleans = []
        self.strings = []
        self.groupings = {}
        self.grouping_names = set()
        self.definitions = {}
        self.is_backward_definition = {}
        self.among_count = 0
        self.pending_substrings = []

    def error(self, message):
        raise SnowballError("%s: token %d: %s" % (self.file_name, self.position, message))

    def peek(self, offset=0):
        index = self.position + offset
        return self.tokens[index] if index < len(self.tokens) else ("end", None)

    def take(self):
        token = self.peek()
        self.position += 1
        return token

    def is_symbol(self, symbol):
        return self.peek() == ("symbol", symbol)

    def is_name(self, name):
        return self.peek() == ("name", name)

    def expect_symbol(self, symbol):
        if not self.is_symbol(symbol):
            self.error("expected %r, found %r" % (symbol, self.peek()))
        self.take()

    def expect_name(self):
        kind, value = self.take()
        if kind != "name":
            self.error("expected a name, found %r" % (value,))
        return value

    def expect_number(self):
        kind, value = self.take()
        if kind != "number":
            self.error("expected a number, found %r" % (value,))
        return int(value)

    def string_value(self, parts):
        characters = []
        for kind, value in parts:
            if kind == "text":
                characters.append(value)
            elif value.startswith("U+"):
                characters.append(chr(int(value[2:], 16)))
            elif value in self.stringdefs:
                characters.append(self.stringdefs[value])
            else:
                self.error("unknown escape {%s}" % value)
        return "".join(characters)

    def expect_string(self):
        kind, value = self.take()
        if kind != "string":
            self.error("expected a string, found %r" % (value,))
        return self.string_value(value)

    def name_list(self):
        self.expect_symbol("(")
        names = []
        while not self.is_symbol(")"):
            names.append(self.expect_name())
        self.take()
        return names

    # Declarations ---------------------------------------------------

    def parse_program(self):
        while self.peek()[0] != "end":
            self.parse_declaration(False)

    def parse_declaration(self, is_backward):
        word = self.expect_name()
        if word == "routines":
            self.routines.update(self.name_list())
        elif word == "externals":
            self.externals.update(self.name_list())
        elif word == "integers":
            self.integers.extend(self.name_list())
        elif word == "booleans":
            self.booleans.extend(self.name_list())
        elif word == "strings":
            self.strings.extend(self.name_list())
        elif word == "groupings":
            self.grouping_names.update(self.name_list())
        elif word == "stringescapes":
            self.expect_symbol("{")
            self.expect_symbol("}")
        elif word == "stringdef":
            kind, name = self.take()
            if kind != "stringdef_name":
                self.error("expected a stringdef name")
            self.stringdefs[name] = self.expect_string()
        elif word == "backwardmode":
            self.expect_symbol("(")
            while not self.is_symbol(")"):
                self.parse_declaration(True)
            self.take()
        elif word == "define":
            name = self.expect_name()
            if name in self.grouping_names:
                self.groupings[name] = self.parse_grouping()
            else:
                if not self.is_name("as"):
                    self.error("expected 'as' after define %s" % name)
                self.take()
                self.definitions[name] = self.parse_command()
                self.is_backward_definition[name] = is_backward
                if self.pending_substrings:
                    self.error("a substring without its among in %s" % name)
        else:
            self.error("unknown declaration %r" % word)

    def parse_grouping(self):
        characters = self.grouping_operand()
        while self.is_symbol("+") or self.is_symbol("-"):
            operator = self.take()[1]
            operand = self.grouping_operand()
            if operator == "+":
                characters = characters | operand
            else:
                characters = characters - operand
        return characters

    def grouping_operand(self):
        kind, value = self.peek()
        if kind == "string":
            return set(self.expect_string())
        name = self.expect_name()
        if name not in self.groupings:
            self.error("unknown grouping %s" % name)
        return set(self.groupings[name])

    # Commands ----------------------------------------------------

    def parse_command(self):
        left = self.parse_unary()
        while self.is_name("or") or self.is_name("and"):
            operator = self.take()[1]
            right = self.parse_unary()
            left = (operator, left, right)
        return left

    def parse_integer_expression(self):
        kind, value = self.take()
        if kind == "number":
            return ("constant", int(value))
        if kind == "name" and value in ("cursor", "limit"):
            return (value, 0)
        if kind == "name" and value in self.integers:
            return ("variable", self.integers.index(value))
        self.error("unsupported integer expression %r" % (value,))

    def parse_unary(self):
        kind, value = self.peek()
        if kind == "symbol" and value == "(":
            self.take()
            commands = []
            while not self.is_symbol(")"):
                commands.append(self.parse_command())
            self.take()
            return ("sequence", commands)
        if kind == "string":
            return ("literal", self.expect_string())
        if kind == "symbol" and value == "[":
            self.take()
            return ("open_slice",)
        if kind == "symbol" and value == "]":
            self.take()
            return ("close_slice",)
        if kind == "symbol" and value == "<-":
            self.take()
            return ("replace", self.expect_string())
        if kind == "symbol" and value == "->":
            self.take()
            return ("slice_to", self.string_variable(self.expect_name()))
        if kind == "symbol" and value == "$":
            self.take()
            variable = self.expect_name()
            if variable not in self.integers:
                self.error("unknown integer %s" % variable)
            operator = self.take()
            if operator[0] != "symbol":
                self.error("expected an integer operator")
            expression = self.parse_integer_expression()
            if operator[1] == "=":
                return ("integer_assign", self.integers.index(variable), expression)
            if operator[1] in RELATIONS:
                return ("integer_compare", self.integers.index(variable),
                        operator[1], expression)
            self.error("unsupported integer operator %r" % operator[1])
        if kind != "name":
            self.error("unexpected %r" % (value,))
        self.take()
        if value in PREFIX_COMMANDS:
            return (value, self.parse_unary())
        if value in ("atleast", "loop"):
            count = self.expect_number()
            return (value, count, self.parse_unary())
        if value == "setlimit":
            first = self.parse_unary()
            if not self.is_name("for"):
                self.error("expected 'for' in setlimit")
            self.take()
            return ("setlimit", first, self.parse_unary())
        if value == "hop":
            return ("hop", self.expect_number())
        if value in ("next", "atlimit", "true", "false", "delete"):
            return (value,)
        if value in ("insert", "attach"):
            if self.peek()[0] == "string":
                return (value, self.expect_string())
            return (value + "_variable", self.string_variable(self.expect_name()))
        if value in ("setmark", "tomark", "atmark"):
            variable = self.expect_name()
            if variable not in self.integers:
                self.error("unknown integer %s" % variable)
            return (value, self.integers.index(variable))
        if value in ("set", "unset"):
            variable = self.expect_name()
            if variable not in self.booleans:
                self.error("unknown boolean %s" % variable)
            return ("boolean_set", self.booleans.index(variable), value == "set")
        if value == "substring":
            node = ["substring", None]
            self.pending_substrings.append(node)
            return node
        if value == "among":
            return self.parse_among()
        if value == "non":
            if self.is_symbol("-"):
                self.take()
            name = self.expect_name()
            if name not in self.grouping_names:
                self.error("non of an unknown grouping %s" % name)
            return ("out_grouping", name)
        if value in self.grouping_names:
            return ("in_grouping", value)
        if value in self.booleans:
            return ("boolean_test", self.booleans.index(value))
        if value in self.routines or value in self.externals:
            return ("call", value)
        self.error("unsupported command %r" % value)

    def string_variable(self, name):
        if name not in self.strings:
            self.error("unknown string variable %s" % name)
        return self.strings.index(name)

    def parse_among(self):
        # The among takes the pending substring of its routine, if any:
        # then the substring finds and the among only dispatches.
        among_number = self.among_count
        self.among_count += 1
        has_substring = bool(self.pending_substrings)
        if has_substring:
            self.pending_substrings.pop()[1] = among_number
        self.expect_symbol("(")
        entries = []
        actions = []
        waiting = []
        while not self.is_symbol(")"):
            kind, value = self.peek()
            if kind == "string":
                waiting.append(self.expect_string())
            elif kind == "symbol" and value == "(":
                actions.append(self.parse_unary())
                entries.extend((text, len(actions)) for text in waiting)
                waiting = []
            else:
                self.error("among: routine conditions are not supported (%r)" % (value,))
        self.take()
        if waiting:
            actions.append(("sequence", []))
            entries.extend((text, len(actions)) for text in waiting)
        if not actions:
            actions.append(("sequence", []))
        return ("among", among_number, has_substring, entries, actions)


# -------------------------------------------------------------- Compiler

class Compiler:
    def __init__(self, parser):
        self.parser = parser
        self.code = []
        self.slot_count = 0
        self.limit_slot_count = 0
        self.string_values = [""]
        self.string_numbers = {"": 0}
        self.grouping_order = []
        self.amongs = {}
        self.routine_addresses = {}
        self.calls = []

    def string_number(self, text):
        if text not in self.string_numbers:
            self.string_numbers[text] = len(self.string_values)
            self.string_values.append(text)
        return self.string_numbers[text]

    def grouping_number(self, name):
        if name not in self.grouping_order:
            self.grouping_order.append(name)
        return self.grouping_order.index(name)

    def new_slot(self):
        self.slot_count += 1
        return self.slot_count - 1

    def emit(self, name, *operands):
        if len(operands) != OPERAND_COUNT[name]:
            raise SnowballError("%s takes %d operands" % (name, OPERAND_COUNT[name]))
        self.code.append(OPCODE[name])
        self.code.extend(operands)
        return len(self.code)

    def emit_jump(self, name):
        """Emits a jump whose target is patched later; returns its operand index."""
        self.emit(name, 0)
        return len(self.code) - 1

    def patch(self, operand_index, target=None):
        self.code[operand_index] = len(self.code) if target is None else target

    def compile_program(self):
        stem = "stem"
        if stem not in self.parser.definitions:
            raise SnowballError("no stem routine")
        order = [stem] + [name for name in self.parser.definitions if name != stem]
        for name in order:
            self.routine_addresses[name] = len(self.code)
            self.compile(self.parser.definitions[name])
            self.emit("RETURN")
        for operand_index, name in self.calls:
            if name not in self.routine_addresses:
                raise SnowballError("routine %s is not defined" % name)
            self.code[operand_index] = self.routine_addresses[name]
        self.check_no_recursion()
        if len(self.code) > 0xFFFF:
            raise SnowballError("program too long")

    def check_no_recursion(self):
        # A routine never calls itself, directly or not: the machine's
        # return stack is bounded by the call depth.
        calls = {}
        for name, definition in self.parser.definitions.items():
            calls[name] = set()
            pending = [definition]
            while pending:
                node = pending.pop()
                if isinstance(node, (tuple, list)):
                    if node and node[0] == "call":
                        calls[name].add(node[1])
                    for part in node:
                        if isinstance(part, (tuple, list)):
                            pending.append(part)
        depth = {}
        for name in calls:
            stack = [(name, iter(sorted(calls[name])))]
            seen = {name}
            while stack:
                current, children = stack[-1]
                child = next(children, None)
                if child is None:
                    stack.pop()
                    seen.discard(current)
                    continue
                if child in seen:
                    raise SnowballError("routine %s is recursive" % child)
                seen.add(child)
                stack.append((child, iter(sorted(calls.get(child, ())))))
                depth[name] = max(depth.get(name, 0), len(stack))
        self.call_depth = max(depth.values(), default=1) + 1

    def compile(self, node):
        kind = node[0]
        if kind == "sequence":
            exits = []
            for command in node[1]:
                self.compile(command)
                exits.append(self.emit_jump("JUMP_IF_FALSE"))
            if not node[1]:
                self.emit("TRUE")
            for operand_index in exits:
                self.patch(operand_index)
        elif kind in ("or", "and"):
            slot = self.new_slot()
            self.emit("SAVE", slot)
            self.compile(node[1])
            exit_jump = self.emit_jump("JUMP_IF_TRUE" if kind == "or" else "JUMP_IF_FALSE")
            self.emit("RESTORE", slot)
            self.compile(node[2])
            self.patch(exit_jump)
        elif kind == "not":
            slot = self.new_slot()
            self.emit("SAVE", slot)
            self.compile(node[1])
            failed = self.emit_jump("JUMP_IF_FALSE")
            self.emit("FALSE")
            done = self.emit_jump("JUMP")
            self.patch(failed)
            self.emit("RESTORE", slot)
            self.emit("TRUE")
            self.patch(done)
        elif kind == "try":
            slot = self.new_slot()
            self.emit("SAVE", slot)
            self.compile(node[1])
            done = self.emit_jump("JUMP_IF_TRUE")
            self.emit("RESTORE", slot)
            self.emit("TRUE")
            self.patch(done)
        elif kind == "test":
            slot = self.new_slot()
            self.emit("SAVE", slot)
            self.compile(node[1])
            self.emit("RESTORE", slot)
        elif kind == "do":
            slot = self.new_slot()
            self.emit("SAVE", slot)
            self.compile(node[1])
            self.emit("RESTORE", slot)
            self.emit("TRUE")
        elif kind == "fail":
            self.compile(node[1])
            self.emit("FALSE")
        elif kind == "repeat":
            self.compile_repeat(node[1])
        elif kind == "atleast":
            exits = []
            for _ in range(node[1]):
                self.compile(node[2])
                exits.append(self.emit_jump("JUMP_IF_FALSE"))
            self.compile_repeat(node[2])
            for operand_index in exits:
                self.patch(operand_index)
        elif kind == "loop":
            exits = []
            for _ in range(node[1]):
                self.compile(node[2])
                exits.append(self.emit_jump("JUMP_IF_FALSE"))
            self.emit("TRUE")
            for operand_index in exits:
                self.patch(operand_index)
        elif kind in ("goto", "gopast"):
            slot = self.new_slot()
            start = len(self.code)
            self.emit("SAVE", slot)
            self.compile(node[1])
            if kind == "goto":
                advance = self.emit_jump("JUMP_IF_FALSE")
                self.emit("RESTORE", slot)
                done = self.emit_jump("JUMP")
                self.patch(advance)
            else:
                done = self.emit_jump("JUMP_IF_TRUE")
            self.emit("RESTORE", slot)
            self.emit("NEXT")
            self.emit("JUMP_IF_TRUE", start)
            self.patch(done)
        elif kind == "backwards":
            self.emit("BACKWARD_BEGIN")
            self.compile(node[1])
            self.emit("BACKWARD_END")
        elif kind == "setlimit":
            slot = self.new_slot()
            limit_slot = self.limit_slot_count
            self.limit_slot_count += 1
            self.emit("SAVE", slot)
            self.compile(node[1])
            failed = self.emit_jump("JUMP_IF_FALSE")
            self.emit("LIMIT_SAVE", limit_slot)
            self.emit("RESTORE", slot)
            self.compile(node[2])
            self.emit("LIMIT_RESTORE", limit_slot)
            self.patch(failed)
        elif kind == "literal":
            self.emit("LITERAL", self.string_number(node[1]))
        elif kind == "in_grouping":
            self.emit("IN_GROUPING", self.grouping_number(node[1]))
        elif kind == "out_grouping":
            self.emit("OUT_GROUPING", self.grouping_number(node[1]))
        elif kind == "next":
            self.emit("NEXT")
        elif kind == "hop":
            self.emit("HOP", node[1])
        elif kind == "atlimit":
            self.emit("AT_LIMIT")
        elif kind == "true":
            self.emit("TRUE")
        elif kind == "false":
            self.emit("FALSE")
        elif kind == "open_slice":
            self.emit("OPEN_SLICE")
        elif kind == "close_slice":
            self.emit("CLOSE_SLICE")
        elif kind == "replace":
            self.emit("REPLACE", self.string_number(node[1]))
        elif kind == "delete":
            self.emit("REPLACE", 0)
        elif kind == "slice_to":
            self.emit("SLICE_TO", node[1])
        elif kind == "insert":
            self.emit("INSERT", self.string_number(node[1]))
        elif kind == "insert_variable":
            self.emit("INSERT_VARIABLE", node[1])
        elif kind == "attach":
            self.emit("ATTACH", self.string_number(node[1]))
        elif kind == "attach_variable":
            self.emit("ATTACH_VARIABLE", node[1])
        elif kind == "setmark":
            self.emit("SET_MARK", node[1])
        elif kind == "tomark":
            self.emit("TO_MARK", node[1])
        elif kind == "atmark":
            self.emit("AT_MARK", node[1])
        elif kind == "integer_assign":
            expression = node[2]
            self.emit("INTEGER_ASSIGN", node[1], EXPRESSION_KINDS[expression[0]],
                      expression[1])
        elif kind == "integer_compare":
            expression = node[3]
            self.emit("INTEGER_COMPARE", node[1], RELATIONS[node[2]],
                      EXPRESSION_KINDS[expression[0]], expression[1])
        elif kind == "boolean_set":
            self.emit("BOOLEAN_SET", node[1], 1 if node[2] else 0)
        elif kind == "boolean_test":
            self.emit("BOOLEAN_TEST", node[1])
        elif kind == "call":
            self.emit("CALL", 0)
            self.calls.append((len(self.code) - 1, node[1]))
        elif kind == "substring":
            if node[1] is None:
                raise SnowballError("a substring without its among")
            self.emit("AMONG_FIND", node[1])
        elif kind == "among":
            self.compile_among(node)
        else:
            raise SnowballError("cannot compile %r" % (kind,))

    def compile_repeat(self, command):
        slot = self.new_slot()
        start = len(self.code)
        self.emit("SAVE", slot)
        self.compile(command)
        self.emit("JUMP_IF_TRUE", start)
        self.emit("RESTORE", slot)
        self.emit("TRUE")

    def compile_among(self, node):
        _, number, has_substring, entries, actions = node
        self.amongs[number] = entries
        failed = None
        if not has_substring:
            self.emit("AMONG_FIND", number)
            failed = self.emit_jump("JUMP_IF_FALSE")
        # AMONG_DISPATCH among count, then count action addresses.
        self.emit("AMONG_DISPATCH", number, len(actions))
        table = len(self.code)
        self.code.extend([0] * len(actions))
        exits = []
        for action_index, action in enumerate(actions):
            self.code[table + action_index] = len(self.code)
            self.compile(action)
            exits.append(self.emit_jump("JUMP"))
        for operand_index in exits:
            self.patch(operand_index)
        if failed is not None:
            self.patch(failed)


# ---------------------------------------------------------------- Output

def c_array(name, element_type, values, per_line=12):
    lines = ["static const %s %s[] = {" % (element_type, name)]
    for start in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(value) for value in values[start:start + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def generate(directory):
    output = []
    output.append("""/*
 * The Snowball stemmers of Full Text (Full Text 3.0, 3.4.4; packet 7.12c,
 * D214): english, german, french, italian, spanish and dutch, the
 * algorithms of the Snowball project (snowballstem.org, version 3.1.1,
 * BSD licensed: third_party/snowball/COPYING), compiled for the stemming
 * machine of src/full_text_stem.c. Generated by
 * tools/generate-snowball-stemmers.py from the .sbl files of third_party/snowball;
 * do not edit.
 *
 * Copyright (c) 2001, Dr Martin Porter; Copyright (c) 2004,2005, Richard
 * Boulton; Copyright (c) 2013, Yoshiki Shibukawa; Copyright (c)
 * 2006-2025, Olly Betts. All rights reserved. Redistribution and use in
 * source and binary forms, with or without modification, are permitted
 * provided that the conditions of third_party/snowball/COPYING are met.
 */
#include "full_text_internal.h"
""")
    programs = []
    for language in LANGUAGES:
        path = os.path.join(directory, language + ".sbl")
        with open(path, encoding="utf-8") as source:
            text = source.read()
        parser = Parser(tokenize(text, path), path)
        parser.parse_program()
        compiler = Compiler(parser)
        compiler.compile_program()
        characters = []
        strings = []
        for value in compiler.string_values:
            code_points = [ord(character) for character in value]
            if any(code_point > 0xFFFF for code_point in code_points):
                raise SnowballError("a character beyond the BMP")
            strings.append((len(characters), len(code_points)))
            characters.extend(code_points)
        groupings = []
        for name in compiler.grouping_order:
            members = sorted(ord(character) for character in parser.groupings[name])
            groupings.append((len(characters), len(members)))
            characters.extend(members)
        among_entries = []
        amongs = []
        for number in range(parser.among_count):
            entries = compiler.amongs.get(number, [])
            # Longest first: the machine takes the first entry that matches
            # (Snowball's among finds the longest).
            ordered = sorted(entries, key=lambda entry: -len(entry[0]))
            amongs.append((len(among_entries), len(ordered)))
            for value, result in ordered:
                among_entries.append((compiler.string_number(value), result))
        # Strings an among added after the pool was laid out.
        while len(strings) < len(compiler.string_values):
            value = compiler.string_values[len(strings)]
            strings.append((len(characters), len(value)))
            characters.extend(ord(character) for character in value)
        if max(len(characters), len(among_entries), len(strings)) > 0xFFFF:
            raise SnowballError("%s: tables too large for 16-bit spans" % language)
        output.append("/* %s.sbl */" % language)
        output.append(c_array(language + "_code", "uint16_t", compiler.code))
        output.append(c_array(language + "_characters", "uint16_t", characters))
        output.append("static const sxml_full_text_stem_span %s_strings[] = {" % language)
        for start in range(0, len(strings), 6):
            output.append("    " + " ".join("{%d, %d}," % span for span in strings[start:start + 6]))
        output.append("};")
        output.append("static const sxml_full_text_stem_span %s_groupings[] = {" % language)
        output.append("    " + " ".join("{%d, %d}," % span for span in groupings))
        output.append("};")
        output.append("static const sxml_full_text_stem_among_entry %s_among_entries[] = {"
                      % language)
        for start in range(0, len(among_entries), 6):
            output.append("    " + " ".join("{%d, %d}," % entry
                                            for entry in among_entries[start:start + 6]))
        output.append("};")
        output.append("static const sxml_full_text_stem_span %s_amongs[] = {" % language)
        for start in range(0, len(amongs), 6):
            output.append("    " + " ".join("{%d, %d}," % span for span in amongs[start:start + 6]))
        output.append("};")
        output.append("")
        programs.append((language, compiler, parser))
    output.append("const sxml_full_text_stemmer_program sxml_full_text_stemmer_programs[] = {")
    for language, compiler, parser in programs:
        output.append("    {.code = %s_code," % language)
        output.append("     .code_count = sizeof %s_code / sizeof %s_code[0]," % (language, language))
        output.append("     .characters = %s_characters," % language)
        output.append("     .strings = %s_strings," % language)
        output.append("     .groupings = %s_groupings," % language)
        output.append("     .among_entries = %s_among_entries," % language)
        output.append("     .amongs = %s_amongs," % language)
        output.append("     .among_count = sizeof %s_amongs / sizeof %s_amongs[0]," % (language, language))
        output.append("     .slot_count = %d," % max(compiler.slot_count, 1))
        output.append("     .limit_slot_count = %d," % max(compiler.limit_slot_count, 1))
        output.append("     .integer_count = %d," % max(len(parser.integers), 1))
        output.append("     .boolean_count = %d," % max(len(parser.booleans), 1))
        output.append("     .string_variable_count = %d," % len(parser.strings))
        output.append("     .call_depth = %d}," % compiler.call_depth)
    output.append("};")
    return "\n".join(output) + "\n"


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    try:
        sys.stdout.write(generate(sys.argv[1]))
    except SnowballError as error:
        raise SystemExit("generate-snowball-stemmers: %s" % error)


if __name__ == "__main__":
    main()
