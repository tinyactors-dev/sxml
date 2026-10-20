/**
 * XPath 4.0 with XQuery and XPath Full Text 3.0, for sxml (D12, D29, D214,
 * D235).
 *
 * Source of truth: XQuery and XPath Full Text 3.0 (W3C Recommendation, 24
 * November 2015), Appendix B "EBNF for XPath 3.0 Grammar with Full-Text
 * extensions", applied to XPath 4.0 as grammars/xquery applies Appendix A
 * to XQuery 4.0: FTContainsExpr as the operand of ComparisonExpr and
 * FTScoreVar in the for bindings (SimpleForBinding; XPath's let bindings
 * take none). Pragma comes with FTExtensionSelection.
 *
 * The productions are those of grammars/xpath/full_text.js, which
 * grammars/xquery shares. This grammar is a parser of its own so that
 * grammars/xpath, and with it the core sxml.wasm, does not carry Full
 * Text's tables: only the optional feature full-text links it (D208,
 * D235). Its keywords are not reserved words, as in grammars/xquery.
 */

const xpath = require('../xpath/grammar');
const fullText = require('../xpath/full_text');

module.exports = grammar(xpath, {
  name: 'xpath_full_text',

  inline: ($, original) => original.concat(fullText.inline($)),

  conflicts: ($, original) => original.concat(fullText.conflicts($)),

  rules: {
    // SimpleForBinding ::= "$" VarName PositionalVar? FTScoreVar? "in" ExprSingle
    // (Full Text 3.0, B [10], with XPath 4.0's positional variable)
    for_item_binding: ($) => seq(
      field('variable', $.variable_name_and_type),
      optional(field('position', $.positional_variable)),
      optional(field('score', $.ft_score_variable)),
      'in',
      field('sequence', $._expression_single),
    ),

    // FTScoreVar ::= "score" "$" VarName (Full Text 3.0, 2.3)
    ft_score_variable: fullText.scoreVariable,

    ...fullText.rules,

    // Pragma ::= "(#" S? EQName (S PragmaContents)? "#)" (B [36])
    pragma: fullText.pragma,
  },
});
