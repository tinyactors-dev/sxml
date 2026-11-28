"""The -> and ->> operators of sqlite3_xml_operators_init leave JSON as it
was: every JSON case gives what stock SQLite gives, value, type, error and
subtype alike.

    python3 jsonarrow_test.py

Two connections: stock SQLite, and one with the extension's operators,
which answer for JSON through a private stock connection and set the JSON
subtype themselves (subtypes do not cross connections). Subtypes cannot be
read from Python, so each case is compared bare, under typeof(), and
wrapped in json_array(...), json_object('k', ...) and a second ->, where
a value of the JSON subtype is embedded as JSON and any other text as a
string. Left operands are SQL expressions, not bound values, so that the
ones made by json(), json_object() and the like carry their subtype in.
"""
import itertools
import sqlite3
import sys

stock = sqlite3.connect(":memory:")
ours = sqlite3.connect(":memory:")
ours.enable_load_extension(True)
ours.load_extension("./xml.so", entrypoint="sqlite3_xml_operators_init")


def literal(text):
    return "'" + text.replace("'", "''") + "'"


documents = [
    '{"a":{"b":7,"c":[1,2,{"d":null}]},"e":"text","f":true,"g":false,"h":null}',
    '{"n":1.50,"m":1e3,"z":-0,"big":12345678901234567890,"neg":-7,"exp":2.5E-3}',
    '{"s":"quo\\"te","u":"\\u00e9t\\u00e9","esc":"a\\nb\\t\\\\c","emoji":"\\ud83d\\ude00"}',
    '[10,[20,30],{"x":"y"},null,true,"last"]',
    '{"a b":1,"x\\"y":2,"$":3,"[0]":4,"":5,"a.b":6}',
    '"just a string"',
    "42",
    "  {  \"spaced\" :  [ 1 , 2 ]  }  ",
    "{\"dup\":1,\"dup\":2}",
]
lefts = []
for document in documents:
    lefts += [
        literal(document),                         # JSON text
        f"json({literal(document)})",              # JSON text, JSON subtype
        f"jsonb({literal(document)})",             # JSONB blob
    ]
lefts += [
    "json_object('a', json_array(1, 2), 'b', 'x')",
    "json_array(json_object('k', 1), 'v')",
    "json_object('nested', json('{\"deep\":[true]}'))",
    "NULL",
    "5",
    "2.5",
    "x'00ff'",
    literal("not json"),
    literal("{\"broken\":"),
    literal(""),
]
rights = [
    "'$'", "'$.a'", "'$.a.b'", "'$.a.c[2].d'", "'$.a.c[#-1]'", "'$.e'", "'$.f'", "'$.g'", "'$.h'",
    "'$.missing'", "'a'", "'e'", "'h'", "'missing'", "'a.b'", "'a b'", "'x\"y'", "'$'", "'[0]'",
    "''", "'$.\"a b\"'", "'$.\"x\\\"y\"'", "'$.n'", "'$.m'", "'$.z'", "'$.big'", "'$.exp'",
    "'$.s'", "'$.u'", "'$.esc'", "'$.emoji'", "'$.spaced'", "'$.dup'",
    "0", "1", "2", "5", "-1", "-6", "'$[0]'", "'$[1][0]'", "'$[2].x'", "'$[#-1]'", "'$[9]'",
    "1.5", "NULL", "x'61'", "'$.'", "'$[a]'", "'.a'",
]
wraps = [
    "{e}",
    "typeof({e})",
    "json_array({e})",
    "json_object('k', {e})",
    "({e}) -> '$'",
]


def run(database, sql):
    try:
        row = database.execute(sql).fetchone()
        return ("value", type(row[0]).__name__, row[0])
    except sqlite3.Error as error:
        return ("error", str(error))


cases = failures = 0
for left, right, operator, wrap in itertools.product(lefts, rights, ("->", "->>"), wraps):
    sql = "select " + wrap.format(e=f"{left} {operator} {right}")
    expected, actual = run(stock, sql), run(ours, sql)
    cases += 1
    if expected != actual:
        failures += 1
        if failures <= 25:
            print(f"DIFFERS {sql}\n  stock: {expected!r}\n  ours:  {actual!r}")
print(f"jsonarrow: {cases} cases, {failures} differ; SQLite {sqlite3.sqlite_version}")
sys.exit(1 if failures else 0)
