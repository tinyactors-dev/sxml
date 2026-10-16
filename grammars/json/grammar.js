/**
 * Strict JSON grammar for sxml (RFC 8259; F&O 4.0, section 17.4).
 *
 * A JSON text is one value with optional whitespace around it (RFC 8259,
 * section 2). Anything outside the RFC 8259 grammar gives an ERROR or
 * MISSING node, which sxml reports as err:FOJS0001; no extension is
 * accepted (the "liberal" option of fn:parse-json accepts nothing more).
 * Escape sequences are tokens of their own so the tree walk decodes them
 * without lexing again; the walk also decides what an escaped surrogate
 * or non-XML character becomes (F&O 4.0, section 17.4.3).
 */

const comma_separated = (rule) => optional(seq(rule, repeat(seq(',', rule))));

module.exports = grammar({
  name: 'json',

  // RFC 8259, section 2: ws = *( %x20 / %x09 / %x0A / %x0D ).
  extras: (_) => [/[ \t\n\r]/],

  word: ($) => $.literal_name,

  rules: {
    // RFC 8259, section 2: JSON-text = ws value ws. A byte order mark is
    // removed by sxml before parsing (F&O 4.0, fn:parse-json).
    document: ($) => field('value', $._value),

    _value: ($) =>
      choice($.object, $.array, $.number, $.string, $.true, $.false, $.null),

    // RFC 8259, section 4.
    object: ($) => seq('{', comma_separated(field('member', $.member)), '}'),
    member: ($) => seq(field('key', $.string), ':', field('value', $._value)),

    // RFC 8259, section 5.
    array: ($) => seq('[', comma_separated(field('member', $._value)), ']'),

    // RFC 8259, section 6: -? int frac? exp?, with no leading zeros.
    number: (_) =>
      token(
        seq(
          optional('-'),
          choice('0', /[1-9][0-9]*/),
          optional(/\.[0-9]+/),
          optional(/[eE][+-]?[0-9]+/),
        ),
      ),

    // RFC 8259, section 7. Unescaped characters are U+0020 and above,
    // except '"' and '\'; whitespace between the tokens of a string is not
    // allowed, so every token after the opening quote is immediate.
    string: ($) =>
      seq(
        '"',
        repeat(choice($.unescaped_text, $.escape_sequence)),
        token.immediate('"'),
      ),
    unescaped_text: (_) => token.immediate(prec(1, /[^"\\\x00-\x1f]+/)),
    escape_sequence: (_) =>
      token.immediate(seq('\\', choice(/["\\/bfnrt]/, /u[0-9a-fA-F]{4}/))),

    // RFC 8259, section 3: the literal names are lowercase.
    true: (_) => 'true',
    false: (_) => 'false',
    null: (_) => 'null',
    literal_name: (_) => /[a-z]+/,
  },
});
