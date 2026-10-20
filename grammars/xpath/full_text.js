/**
 * XQuery and XPath Full Text 3.0 (W3C Recommendation, 24 November 2015,
 * Appendices A and B; D214, D235): the productions both grammars add,
 * shared by grammars/xquery and grammars/xpath_full_text so that Full
 * Text has one definition.
 *
 * - `rules`: FTContainsExpr as the operand of ComparisonExpr, with its
 *   full-text selections, positional filters and match options.
 * - `scoreVariable`: FTScoreVar, for the for bindings of both languages
 *   (XQuery's let bindings take one too; XPath's do not, Appendix B).
 * - `pragma`: Pragma, which FTExtensionSelection needs (XQuery's
 *   extension expressions use it too).
 * - `inline` and `conflicts`: what each grammar adds to its own lists.
 *
 * Its 43 keywords are not reserved words: tree-sitter's keyword
 * extraction returns one only where the parse state expects it and an
 * NCName everywhere else, which keeps the name positions and the parser
 * tables as they were (D214). No position accepts both one of them and a
 * name.
 */

const commaSeparated1 = (rule) => seq(rule, repeat(seq(',', rule)));

// FTRange ::= ("exactly" E) | ("at" "least" E) | ("at" "most" E) | ("from" E "to" E)
// (Full Text 3.0, 3.3: AdditiveExpr; FTLiteralRange, 3.4.4: IntegerLiteral).
// "exactly" gives field value, the others minimum and maximum.
const fullTextRange = (operand) => choice(
  seq('exactly', field('value', operand)),
  seq('at', 'least', field('minimum', operand)),
  seq('at', 'most', field('maximum', operand)),
  seq('from', field('minimum', operand), 'to', field('maximum', operand)),
);

// The choice rules leave no symbol in the parser tables, which keeps
// their large-state rows from growing.
const inline = ($) => [
  $._ft_contains_level,
  $._ft_unary_not_level,
  $._ft_primary_with_options_level,
  $._ft_primary,
  $._ft_positional_filter,
  $._ft_match_option,
  $._ft_thesaurus_first,
];

const conflicts = ($) => [
  // After a thesaurus URI, "at" starts "at least" or "at most" of its
  // levels or a following "at start" or "at end" (Full Text 3.0, 3.4.3
  // and 3.5.5): the token after "at" decides.
  [$.ft_thesaurus_id],
];

// FTScoreVar ::= "score" "$" VarName (Full Text 3.0, 2.3)
const scoreVariable = ($) => seq('score', '$', field('name', $._eq_name));

// Pragma ::= "(#" S EQName (S PragmaContents)? "#)"
const pragma = ($) => seq(
  alias($._pragma_open, '(#'),
  field('name', $._eq_name),
  optional(field('contents', $.pragma_contents)),
  '#)',
);

const rules = {
  // ComparisonExpr ::= FTContainsExpr ((ValueComp | GeneralComp | NodeComp) FTContainsExpr)?
  // (Full Text 3.0, 2.2; XQuery 4.0's OtherwiseExpr is the operand of
  // FTContainsExpr, as StringConcatExpr is in XQuery 3.0)
  comparison_expression: ($) => seq(
    field('left', $._ft_contains_level),
    field('operator', choice(
      'eq', 'ne', 'lt', 'le', 'gt', 'ge',
      '=', '!=', '<', '<=', '>', '>=',
      'is', 'is-not', '<<', 'precedes', '>>', 'follows', 'precedes-or-is', 'follows-or-is',
    )),
    field('right', $._ft_contains_level),
  ),

  _comparison_level: ($) => choice($.comparison_expression, $._ft_contains_level),

  _ft_contains_level: ($) => choice($.ft_contains_expression, $._otherwise_level),

  // FTContainsExpr ::= StringConcatExpr ("contains" "text" FTSelection FTIgnoreOption?)?
  // FTIgnoreOption ::= "without" "content" UnionExpr (3.7)
  ft_contains_expression: ($) => seq(
    field('value', $._otherwise_level), 'contains', 'text',
    field('selection', $.ft_selection),
    optional(seq('without', 'content', field('ignore', $._union_level))),
  ),

  // FTSelection ::= FTOr FTPosFilter* (3.1)
  ft_selection: ($) => seq(
    field('operand', $._ft_or_level),
    repeat(field('filter', $._ft_positional_filter)),
  ),

  // FTOr ::= FTAnd ("ftor" FTAnd)*, FTAnd ::= FTMildNot ("ftand" FTMildNot)*,
  // FTMildNot ::= FTUnaryNot ("not" "in" FTUnaryNot)*, left-recursive as
  // XPath's operator levels (3.1.1 to 3.1.4)
  _ft_or_level: ($) => choice($.ft_or, $._ft_and_level),
  ft_or: ($) => seq(field('left', $._ft_or_level), 'ftor', field('right', $._ft_and_level)),

  _ft_and_level: ($) => choice($.ft_and, $._ft_mild_not_level),
  ft_and: ($) => seq(field('left', $._ft_and_level), 'ftand', field('right', $._ft_mild_not_level)),

  _ft_mild_not_level: ($) => choice($.ft_mild_not, $._ft_unary_not_level),
  ft_mild_not: ($) => seq(
    field('left', $._ft_mild_not_level), 'not', 'in', field('right', $._ft_unary_not_level),
  ),

  // FTUnaryNot ::= ("ftnot")? FTPrimaryWithOptions
  _ft_unary_not_level: ($) => choice($.ft_unary_not, $._ft_primary_with_options_level),
  ft_unary_not: ($) => seq('ftnot', field('operand', $._ft_primary_with_options_level)),

  // FTPrimaryWithOptions ::= FTPrimary FTMatchOptions? FTWeight?
  // FTMatchOptions ::= ("using" FTMatchOption)+, FTWeight ::= "weight" "{" Expr "}"
  _ft_primary_with_options_level: ($) => choice(
    $.ft_primary_with_options, $._ft_primary,
  ),
  ft_primary_with_options: ($) => seq(
    field('operand', $._ft_primary),
    choice(
      seq(repeat1(seq('using', field('option', $._ft_match_option))),
        optional(seq('weight', '{', field('weight', $._expression), '}'))),
      seq('weight', '{', field('weight', $._expression), '}'),
    ),
  ),

  // FTPrimary ::= (FTWords FTTimes?) | ("(" FTSelection ")") | FTExtensionSelection
  _ft_primary: ($) => choice(
    $.ft_words,
    seq('(', $.ft_selection, ')'),
    $.ft_extension_selection,
  ),

  // FTWords ::= FTWordsValue FTAnyallOption?, FTWordsValue ::= StringLiteral | ("{" Expr "}"),
  // FTAnyallOption ::= ("any" "word"?) | ("all" "words"?) | "phrase",
  // FTTimes ::= "occurs" FTRange "times" (3.2, 3.3)
  ft_words: ($) => seq(
    choice(field('value', $.string_literal), seq('{', field('value', $._expression), '}')),
    optional(choice(
      seq(field('any_all', 'any'), optional(field('separate_words', 'word'))),
      seq(field('any_all', 'all'), optional(field('separate_words', 'words'))),
      field('any_all', 'phrase'),
    )),
    optional(seq('occurs', field('times', $.ft_range), 'times')),
  ),

  ft_range: ($) => fullTextRange($._additive_level),

  // FTExtensionSelection ::= Pragma+ "{" FTSelection? "}" (3.6)
  ft_extension_selection: ($) => seq(
    repeat1(field('pragma', $.pragma)), '{', optional(field('selection', $.ft_selection)), '}',
  ),

  // FTPosFilter ::= FTOrder | FTWindow | FTDistance | FTScope | FTContent (3.5)
  _ft_positional_filter: ($) => choice(
    $.ft_order, $.ft_window, $.ft_distance, $.ft_scope, $.ft_content,
  ),

  ft_order: ($) => 'ordered',

  // FTWindow ::= "window" AdditiveExpr FTUnit, FTUnit ::= "words" | "sentences" | "paragraphs"
  ft_window: ($) => seq(
    'window', field('value', $._additive_level),
    field('unit', choice('words', 'sentences', 'paragraphs')),
  ),

  // FTDistance ::= "distance" FTRange FTUnit
  ft_distance: ($) => seq(
    'distance', field('range', $.ft_range),
    field('unit', choice('words', 'sentences', 'paragraphs')),
  ),

  // FTScope ::= ("same" | "different") FTBigUnit, FTBigUnit ::= "sentence" | "paragraph"
  ft_scope: ($) => seq(
    choice('same', field('different', 'different')),
    field('unit', choice('sentence', 'paragraph')),
  ),

  // FTContent ::= ("at" "start") | ("at" "end") | ("entire" "content")
  ft_content: ($) => choice(
    seq('at', field('content', choice('start', 'end'))),
    seq(field('content', 'entire'), 'content'),
  ),

  // FTMatchOption ::= FTLanguageOption | FTWildCardOption | FTThesaurusOption
  //   | FTStemOption | FTCaseOption | FTDiacriticsOption | FTStopWordOption
  //   | FTExtensionOption (3.4)
  _ft_match_option: ($) => choice(
    $.ft_language_option,
    $.ft_wildcard_option,
    $.ft_thesaurus_option,
    $.ft_stem_option,
    $.ft_case_option,
    $.ft_diacritics_option,
    $.ft_stop_word_option,
    $.ft_extension_option,
  ),

  // FTLanguageOption ::= "language" StringLiteral (3.4.1)
  ft_language_option: ($) => seq('language', field('language', $.string_literal)),

  // FTWildCardOption ::= "wildcards" | ("no" "wildcards") (3.4.2)
  ft_wildcard_option: ($) => seq(optional(field('no', 'no')), 'wildcards'),

  // FTThesaurusOption ::= ("thesaurus" (FTThesaurusID | "default"))
  //   | ("thesaurus" "(" (FTThesaurusID | "default") ("," FTThesaurusID)* ")")
  //   | ("no" "thesaurus") (3.4.3)
  ft_thesaurus_option: ($) => choice(
    seq('thesaurus', $._ft_thesaurus_first),
    seq('thesaurus', '(', $._ft_thesaurus_first,
      repeat(seq(',', field('thesaurus', $.ft_thesaurus_id))), ')'),
    seq(field('no', 'no'), 'thesaurus'),
  ),

  _ft_thesaurus_first: ($) => choice(
    field('thesaurus', $.ft_thesaurus_id), field('uses_default', 'default'),
  ),

  // FTThesaurusID ::= "at" URILiteral ("relationship" StringLiteral)? (FTLiteralRange "levels")?
  ft_thesaurus_id: ($) => seq(
    'at', field('uri', $.string_literal),
    optional(seq('relationship', field('relationship', $.string_literal))),
    optional(seq(field('range', alias($._ft_literal_range, $.ft_range)), 'levels')),
  ),

  _ft_literal_range: ($) => fullTextRange($.integer_literal),

  // FTStemOption ::= "stemming" | ("no" "stemming") (3.4.4)
  ft_stem_option: ($) => seq(optional(field('no', 'no')), 'stemming'),

  // FTCaseOption ::= ("case" "insensitive") | ("case" "sensitive") | "lowercase" | "uppercase" (3.4.5)
  ft_case_option: ($) => choice(
    seq('case', field('mode', choice('insensitive', 'sensitive'))),
    field('mode', choice('lowercase', 'uppercase')),
  ),

  // FTDiacriticsOption ::= ("diacritics" "insensitive") | ("diacritics" "sensitive") (3.4.6)
  ft_diacritics_option: ($) => seq('diacritics', field('mode', choice('insensitive', 'sensitive'))),

  // FTStopWordOption ::= ("stop" "words" FTStopWords FTStopWordsInclExcl*)
  //   | ("stop" "words" "default" FTStopWordsInclExcl*) | ("no" "stop" "words") (3.4.7)
  ft_stop_word_option: ($) => choice(
    seq('stop', 'words',
      choice(field('words', $.ft_stop_words), field('uses_default', 'default')),
      repeat(field('inclusion', $.ft_stop_words_inclusion))),
    seq(field('no', 'no'), 'stop', 'words'),
  ),

  // FTStopWords ::= ("at" URILiteral) | ("(" StringLiteral ("," StringLiteral)* ")")
  ft_stop_words: ($) => choice(
    seq('at', field('uri', $.string_literal)),
    seq('(', commaSeparated1(field('word', $.string_literal)), ')'),
  ),

  // FTStopWordsInclExcl ::= ("union" | "except") FTStopWords
  ft_stop_words_inclusion: ($) => seq(
    field('operator', choice('union', 'except')), field('words', $.ft_stop_words),
  ),

  // FTExtensionOption ::= "option" EQName StringLiteral (3.4.8)
  ft_extension_option: ($) => seq('option', field('name', $._eq_name), field('value', $.string_literal)),
};

module.exports = { inline, conflicts, scoreVariable, pragma, rules };
