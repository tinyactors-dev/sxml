/**
 * XPath 4.0 grammar for sxml (docs/DECISIONS.md, D12 and D27).
 *
 * Source of truth: XPath 4.0, W3C QT4 CG draft, Appendix A
 * (https://qt4cg.org/specifications/xquery-40/xpath-40.html#nt-bnf).
 * Rule names are the EBNF production names in snake_case with abbreviations
 * spelled out (Expr -> expression, Var -> variable, Comp -> comparison or
 * computed, Decl -> declaration). Hidden rules (leading underscore) are the
 * EBNF choice productions that add no node of their own.
 *
 * The XQuery 4.0 grammar (grammars/xquery) and the XSLT 4.0 pattern grammar
 * (grammars/xslt_pattern) inherit from this one.
 */

const lexical = require('./lexical');

const commaSeparated1 = (rule) => seq(rule, repeat(seq(',', rule)));
const commaSeparated = (rule) => optional(commaSeparated1(rule));
const separated1 = (separator, rule) => seq(rule, repeat(seq(separator, rule)));

const keywordAliases = ($, keywords) =>
  choice(...keywords.map((keyword) => alias(keyword, $.ncname)));

const without = (list, removed) => list.filter((entry) => !removed.includes(entry));

const AXES = [
  'ancestor', 'ancestor-or-self', 'attribute', 'child', 'descendant',
  'descendant-or-self', 'following', 'following-or-self', 'following-sibling',
  'following-sibling-or-self', 'item', 'namespace', 'parent', 'preceding',
  'preceding-or-self', 'preceding-sibling', 'preceding-sibling-or-self', 'self',
];

module.exports = grammar({
  name: 'xpath',

  word: ($) => $.ncname,

  // Every keyword is a reserved word: the lexer always returns the keyword
  // token, even where only a name is valid, matching the context-free
  // tokenization of A.3. Name positions accept each keyword as an NCName
  // (`_keyword_name`), except where A.4 reserves it (function names).
  reserved: {
    global: ($) => lexical.XPATH_KEYWORDS,
  },

  // A.3.4.1 Default Whitespace Handling: whitespace (XML S) and comments
  // separate terminals.
  extras: ($) => [/[\x20\x09\x0D\x0A]/, $.comment],

  // The external scanner (grammars/common/query_scanner.h) is shared by all
  // three grammars, so every grammar declares the full token list in this
  // order. Tokens that a grammar does not use are never valid in it.
  externals: ($) => [
    $.comment,
    $.string_template_fixed_part,
    $.string_constructor_characters,
    $._direct_element_open,
    $._direct_tag_name,
    $._direct_attribute_name,
    $._direct_attribute_equals,
    $._direct_quote_open,
    $._direct_quote_close,
    $._direct_apostrophe_open,
    $._direct_apostrophe_close,
    $._direct_quote_content,
    $._direct_apostrophe_content,
    $._direct_tag_end,
    $._direct_empty_tag_end,
    $._direct_end_tag_open,
    $.direct_element_text,
    $.direct_comment_constructor,
    $.direct_processing_instruction_constructor,
    $.cdata_section,
    $._pragma_open,
    $.pragma_contents,
    $._error_sentinel,
  ],

  conflicts: ($) => [
    // "namespace #x {...}" (computed namespace constructor) against
    // "namespace#1" (named function reference): decided by the token after "#".
    [$._function_keyword_name, $.computed_namespace_constructor],
  ],

  rules: {
    // XPath ::= (DefaultElementNamespaceDecl ";")? (NamespaceDecl ";")* Expr
    xpath: ($) => seq(
      optional(seq(field('declaration', $.default_element_namespace_declaration), ';')),
      repeat(seq(field('declaration', $.namespace_declaration), ';')),
      field('body', $._expression),
    ),

    default_element_namespace_declaration: ($) => seq(
      'declare', 'default', 'element', 'namespace', field('uri', $.string_literal),
    ),

    namespace_declaration: ($) => seq(
      'declare', 'namespace', field('prefix', $._ncname), '=', field('uri', $.string_literal),
    ),

    // Expr ::= (ExprSingle ++ ",")
    _expression: ($) => choice($._expression_single, $.sequence_expression),

    sequence_expression: ($) => seq(
      field('item', $._expression_single),
      repeat1(seq(',', field('item', $._expression_single))),
    ),

    _expression_single: ($) => choice(
      $.flwor_expression,
      $.quantified_expression,
      $.if_expression,
      $._or_level,
    ),

    // ForExpr and LetExpr: ForClause/LetClause chains ending in "return".
    // XQuery replaces the clause set with the full FLWOR clauses.
    flwor_expression: ($) => seq(
      field('clause', $._initial_clause),
      repeat(field('clause', $._intermediate_clause)),
      field('return', $.return_clause),
    ),

    _initial_clause: ($) => choice($.for_clause, $.let_clause),
    _intermediate_clause: ($) => choice($.for_clause, $.let_clause),

    return_clause: ($) => seq('return', field('value', $._expression_single)),

    // ForClause ::= "for" (ForBinding ++ ",")
    for_clause: ($) => seq('for', commaSeparated1(field('binding', $._for_binding))),

    _for_binding: ($) => choice(
      $.for_item_binding,
      $.for_member_binding,
      $.for_entry_binding,
    ),

    for_item_binding: ($) => seq(
      field('variable', $.variable_name_and_type),
      optional(field('position', $.positional_variable)),
      'in',
      field('sequence', $._expression_single),
    ),

    for_member_binding: ($) => seq(
      'member',
      field('variable', $.variable_name_and_type),
      optional(field('position', $.positional_variable)),
      'in',
      field('sequence', $._expression_single),
    ),

    for_entry_binding: ($) => seq(
      choice(
        seq(field('key', $.for_entry_key_binding), optional(field('value', $.for_entry_value_binding))),
        field('value', $.for_entry_value_binding),
      ),
      optional(field('position', $.positional_variable)),
      'in',
      field('sequence', $._expression_single),
    ),

    for_entry_key_binding: ($) => seq('key', field('variable', $.variable_name_and_type)),
    for_entry_value_binding: ($) => seq('value', field('variable', $.variable_name_and_type)),

    positional_variable: ($) => seq('at', '$', field('name', $._eq_name)),

    // LetClause ::= "let" (LetBinding ++ ",")
    let_clause: ($) => seq('let', commaSeparated1(field('binding', $._let_binding))),

    _let_binding: ($) => choice(
      $.let_value_binding,
      $.let_sequence_binding,
      $.let_array_binding,
      $.let_map_binding,
    ),

    let_value_binding: ($) => seq(
      field('variable', $.variable_name_and_type),
      ':=',
      field('value', $._expression_single),
    ),

    let_sequence_binding: ($) => seq(
      '$', '(', commaSeparated1(field('variable', $.variable_name_and_type)), ')',
      optional(field('type', $.type_declaration)),
      ':=',
      field('value', $._expression_single),
    ),

    let_array_binding: ($) => seq(
      '$', '[', commaSeparated1(field('variable', $.variable_name_and_type)), ']',
      optional(field('type', $.type_declaration)),
      ':=',
      field('value', $._expression_single),
    ),

    let_map_binding: ($) => seq(
      '$', '{', commaSeparated1(field('variable', $.variable_name_and_type)), '}',
      optional(field('type', $.type_declaration)),
      ':=',
      field('value', $._expression_single),
    ),

    // VarNameAndType ::= "$" EQName TypeDeclaration?
    variable_name_and_type: ($) => seq(
      '$',
      field('name', $._eq_name),
      optional(field('type', $.type_declaration)),
    ),

    // QuantifiedExpr ::= ("some" | "every") (QuantifierBinding ++ ",") "satisfies" ExprSingle
    quantified_expression: ($) => seq(
      field('quantifier', choice('some', 'every')),
      commaSeparated1(field('binding', $.quantifier_binding)),
      'satisfies',
      field('condition', $._expression_single),
    ),

    quantifier_binding: ($) => seq(
      field('variable', $.variable_name_and_type),
      'in',
      field('sequence', $._expression_single),
    ),

    // IfExpr ::= "if" "(" Expr ")" (UnbracedActions | BracedAction)
    if_expression: ($) => seq(
      'if', '(', field('condition', $._expression), ')',
      choice(
        seq('then', field('then', $._expression_single), 'else', field('else', $._expression_single)),
        field('then', $.enclosed_expression),
      ),
    ),

    // Binary operator levels, lowest precedence first (A.5). Each level is a
    // hidden choice between its own node and the next level, following the
    // EBNF, so that the parse is deterministic without precedence rules.
    _or_level: ($) => choice($.or_expression, $._and_level),
    or_expression: ($) => seq(
      field('left', $._or_level), field('operator', 'or'), field('right', $._and_level),
    ),

    _and_level: ($) => choice($.and_expression, $._comparison_level),
    and_expression: ($) => seq(
      field('left', $._and_level), field('operator', 'and'), field('right', $._comparison_level),
    ),

    // ComparisonExpr ::= OtherwiseExpr ((ValueComp | GeneralComp | NodeComp) OtherwiseExpr)?
    _comparison_level: ($) => choice($.comparison_expression, $._otherwise_level),
    comparison_expression: ($) => seq(
      field('left', $._otherwise_level),
      field('operator', choice(
        'eq', 'ne', 'lt', 'le', 'gt', 'ge',
        '=', '!=', '<', '<=', '>', '>=',
        'is', 'is-not', '<<', 'precedes', '>>', 'follows', 'precedes-or-is', 'follows-or-is',
      )),
      field('right', $._otherwise_level),
    ),

    _otherwise_level: ($) => choice($.otherwise_expression, $._string_concatenation_level),
    otherwise_expression: ($) => seq(
      field('left', $._otherwise_level),
      field('operator', 'otherwise'),
      field('right', $._string_concatenation_level),
    ),

    _string_concatenation_level: ($) => choice($.string_concatenation_expression, $._range_level),
    string_concatenation_expression: ($) => seq(
      field('left', $._string_concatenation_level),
      field('operator', '||'),
      field('right', $._range_level),
    ),

    _range_level: ($) => choice($.range_expression, $._additive_level),
    range_expression: ($) => seq(
      field('left', $._additive_level), field('operator', 'to'), field('right', $._additive_level),
    ),

    _additive_level: ($) => choice($.additive_expression, $._multiplicative_level),
    additive_expression: ($) => seq(
      field('left', $._additive_level),
      field('operator', choice('+', '-')),
      field('right', $._multiplicative_level),
    ),

    _multiplicative_level: ($) => choice($.multiplicative_expression, $._union_level),
    multiplicative_expression: ($) => seq(
      field('left', $._multiplicative_level),
      field('operator', choice('*', '×', 'div', '÷', 'idiv', 'mod')),
      field('right', $._union_level),
    ),

    _union_level: ($) => choice($.union_expression, $._intersect_except_level),
    union_expression: ($) => seq(
      field('left', $._union_level),
      field('operator', choice('union', '|')),
      field('right', $._intersect_except_level),
    ),

    _intersect_except_level: ($) => choice($.intersect_except_expression, $._but_with_level),
    intersect_except_expression: ($) => seq(
      field('left', $._intersect_except_level),
      field('operator', choice('intersect', 'except')),
      field('right', $._but_with_level),
    ),

    // ButWithExpr ::= InstanceofExpr ("but" "with" InstanceofExpr)*
    _but_with_level: ($) => choice($.but_with_expression, $._instance_of_level),
    but_with_expression: ($) => seq(
      field('left', $._but_with_level), 'but', 'with', field('right', $._instance_of_level),
    ),

    _instance_of_level: ($) => choice($.instance_of_expression, $._treat_level),
    instance_of_expression: ($) => seq(
      field('value', $._treat_level), 'instance', 'of', field('type', $.sequence_type),
    ),

    _treat_level: ($) => choice($.treat_expression, $._castable_level),
    treat_expression: ($) => seq(
      field('value', $._castable_level), 'treat', 'as', field('type', $.sequence_type),
    ),

    _castable_level: ($) => choice($.castable_expression, $._cast_level),
    castable_expression: ($) => seq(
      field('value', $._cast_level), 'castable', 'as', field('type', $.cast_target),
    ),

    _cast_level: ($) => choice($.cast_expression, $._pipeline_level),
    cast_expression: ($) => seq(
      field('value', $._pipeline_level), 'cast', 'as', field('type', $.cast_target),
    ),

    // CastTarget ::= (TypeName | ChoiceItemType | EnumerationType | ArrayType | MapType | RecordType) "?"?
    cast_target: ($) => prec.right(seq(
      field('type', choice(
        $._type_name,
        $.choice_item_type,
        $.enumeration_type,
        $.any_array_type,
        $.typed_array_type,
        $.any_map_type,
        $.typed_map_type,
        $.any_record_type,
        $.typed_record_type,
      )),
      optional(field('optional', '?')),
    )),

    // PipelineExpr ::= (ArrowExpr ++ "->")
    _pipeline_level: ($) => choice($.pipeline_expression, $._arrow_level),
    pipeline_expression: ($) => seq(
      field('left', $._pipeline_level), field('operator', '->'), field('right', $._arrow_level),
    ),

    // ArrowExpr ::= UnaryExpr (SequenceArrowTarget | MappingArrowTarget)*
    _arrow_level: ($) => choice($.arrow_expression, $._unary_level),
    arrow_expression: ($) => seq(
      field('value', $._arrow_level),
      field('operator', choice('=>', '=!>')),
      field('target', $._arrow_target),
    ),

    // ArrowTarget ::= FunctionCall | RestrictedDynamicCall
    _arrow_target: ($) => choice($.function_call, $.restricted_dynamic_call),

    restricted_dynamic_call: ($) => seq(
      field('function', choice(
        $.variable_reference,
        $.parenthesized_expression,
        $._function_item_expression,
        $.map_constructor,
        $._array_constructor,
      )),
      field('arguments', $.positional_argument_list),
    ),

    // UnaryExpr ::= ("-" | "+")* ValueExpr
    _unary_level: ($) => choice($.unary_expression, $._value_expression),
    unary_expression: ($) => seq(
      field('operator', choice('-', '+')),
      field('operand', $._unary_level),
    ),

    _value_expression: ($) => $._simple_map_level,

    // SimpleMapExpr ::= PathExpr ("!" PathExpr)*
    _simple_map_level: ($) => choice($.simple_map_expression, $._path_level),
    simple_map_expression: ($) => seq(
      field('left', $._simple_map_level), field('operator', '!'), field('right', $._path_level),
    ),

    // PathExpr ::= AbsolutePathExpr | RelativePathExpr   /* xgc: leading-lone-slash */
    _path_level: ($) => choice($.absolute_path_expression, $._relative_path_level),

    // AbsolutePathExpr ::= ("/" RelativePathExpr?) | ("//" RelativePathExpr)
    // A.1.2 leading-lone-slash: a "/" followed by a terminal that can start a
    // RelativePathExpr begins a path, so the lone "/" has the lowest
    // precedence against every shift.
    absolute_path_expression: ($) => choice(
      prec(-1, field('operator', '/')),
      seq(field('operator', '/'), field('path', $._relative_path_level)),
      seq(field('operator', '//'), field('path', $._relative_path_level)),
    ),

    // RelativePathExpr ::= StepExpr (("/" | "//") StepExpr)*
    _relative_path_level: ($) => choice($.relative_path_expression, $._step_expression),
    relative_path_expression: ($) => seq(
      field('left', $._relative_path_level),
      field('operator', choice('/', '//')),
      field('right', $._step_expression),
    ),

    // StepExpr ::= PostfixExpr | AxisStep
    _step_expression: ($) => choice($._postfix_expression, $.axis_step),

    // AxisStep ::= (AbbreviatedStep | FullStep) Predicate*
    // AbbreviatedStep ::= ".." | ("@" NodeTest) | SimpleNodeTest
    // FullStep ::= Axis NodeTest
    axis_step: ($) => seq(
      choice(
        seq(field('axis', $.axis), field('test', $._node_test)),
        seq(field('abbreviation', '@'), field('test', $._node_test)),
        field('abbreviation', '..'),
        field('test', $._simple_node_test),
      ),
      repeat(field('predicate', $.predicate)),
    ),

    axis: ($) => seq(field('name', choice(...AXES)), '::'),

    // NodeTest ::= UnionNodeTest | SimpleNodeTest | DynamicNodeTest
    _node_test: ($) => choice($.union_node_test, $._simple_node_test, $.dynamic_node_test),

    union_node_test: ($) => seq('(', separated1('|', field('test', $._simple_node_test)), ')'),

    dynamic_node_test: ($) => field('expression', $.enclosed_expression),

    // SimpleNodeTest ::= TypeTest | Selector; Selector ::= EQName | Wildcard
    _simple_node_test: ($) => choice($._type_test, $._eq_name, $.wildcard),

    // Wildcard ::= "*" | (NCName ":*") | ("*:" NCName) | (BracedURILiteral "*")
    wildcard: ($) => choice(
      '*',
      token(seq(new RustRegex(lexical.NCNAME), ':*')),
      token(seq('*:', new RustRegex(lexical.NCNAME))),
      token(seq('Q{', /[^{}]*/, '}*')),
    ),

    // TypeTest ::= GNodeType | XNodeType | JNodeType
    _type_test: ($) => choice($.gnode_type, $._xnode_type, $.jnode_type),

    // PostfixExpr ::= PrimaryExpr | FilterExpr | DynamicFunctionCall | LookupExpr | MethodCall
    _postfix_expression: ($) => choice(
      $._primary_expression,
      $.filter_expression,
      $.dynamic_function_call,
      $.lookup_expression,
      $.method_call,
    ),

    filter_expression: ($) => seq(
      field('value', $._postfix_expression), field('predicate', $.predicate),
    ),

    dynamic_function_call: ($) => seq(
      field('function', $._postfix_expression), field('arguments', $.positional_argument_list),
    ),

    lookup_expression: ($) => seq(
      field('value', $._postfix_expression), field('lookup', $.lookup),
    ),

    // MethodCall ::= PostfixExpr "=?>" NCName PositionalArgumentList
    method_call: ($) => seq(
      field('value', $._postfix_expression),
      '=?>',
      field('name', $._ncname),
      field('arguments', $.positional_argument_list),
    ),

    predicate: ($) => seq('[', field('expression', $._expression), ']'),

    // Lookup ::= "?" KeySpecifier
    lookup: ($) => seq('?', field('key', $._key_specifier)),

    // KeySpecifier ::= NCName | Literal | ContextValueRef | VarRef | ParenthesizedExpr | LookupWildcard
    // A prefixed name is accepted here so that "a:b" stays one token, as the
    // longest-token rule of A.3 requires; the tree builder rejects it
    // (XPST0003), because a KeySpecifier name must be an NCName.
    _key_specifier: ($) => choice(
      $._ncname,
      $.prefixed_name,
      $._literal,
      $.context_value_reference,
      $.variable_reference,
      $.parenthesized_expression,
      alias('*', $.lookup_wildcard),
    ),

    // PrimaryExpr
    _primary_expression: ($) => choice(
      $._literal,
      $.variable_reference,
      $.parenthesized_expression,
      $.context_value_reference,
      $.function_call,
      $._node_constructor,
      $._function_item_expression,
      $.map_constructor,
      $._array_constructor,
      $.string_template,
      $.unary_lookup,
    ),

    _literal: ($) => choice($._numeric_literal, $.string_literal, $.qname_literal),

    _numeric_literal: ($) => choice(
      $.integer_literal,
      $.hex_integer_literal,
      $.binary_integer_literal,
      $.decimal_literal,
      $.double_literal,
    ),

    // QNameLiteral ::= "#" EQName
    qname_literal: ($) => seq('#', field('name', $._eq_name)),

    variable_reference: ($) => seq('$', field('name', $._eq_name)),

    parenthesized_expression: ($) => seq('(', optional(field('expression', $._expression)), ')'),

    context_value_reference: ($) => '.',

    // FunctionCall ::= EQName ArgumentList   /* xgc: reserved-function-names */
    function_call: ($) => seq(
      field('name', $._function_eq_name),
      field('arguments', $.argument_list),
    ),

    // ArgumentList ::= "(" ((PositionalArguments ("," KeywordArguments)?) | KeywordArguments)? ")"
    argument_list: ($) => seq(
      '(',
      optional(choice(
        seq(
          commaSeparated1(field('argument', $._argument)),
          optional(seq(',', commaSeparated1(field('argument', $.keyword_argument)))),
        ),
        commaSeparated1(field('argument', $.keyword_argument)),
      )),
      ')',
    ),

    positional_argument_list: ($) => seq('(', commaSeparated(field('argument', $._argument)), ')'),

    _argument: ($) => choice($._expression_single, alias('?', $.argument_placeholder)),

    keyword_argument: ($) => seq(
      field('name', $._eq_name), ':=', field('value', $._argument),
    ),

    unary_lookup: ($) => seq('?', field('key', $._key_specifier)),

    // NodeConstructor ::= ComputedConstructor (XQuery adds DirectConstructor)
    _node_constructor: ($) => $._computed_constructor,

    _computed_constructor: ($) => choice(
      $.computed_document_constructor,
      $.computed_element_constructor,
      $.computed_attribute_constructor,
      $.computed_namespace_constructor,
      $.computed_text_constructor,
      $.computed_comment_constructor,
      $.computed_processing_instruction_constructor,
    ),

    computed_document_constructor: ($) => seq('document', field('content', $.enclosed_expression)),

    computed_element_constructor: ($) => seq(
      'element', $._computed_node_name, field('content', $.enclosed_expression),
    ),

    computed_attribute_constructor: ($) => seq(
      'attribute', $._computed_node_name, field('content', $.enclosed_expression),
    ),

    computed_namespace_constructor: ($) => seq(
      'namespace', $._computed_node_ncname, field('content', $.enclosed_expression),
    ),

    computed_text_constructor: ($) => seq('text', field('content', $.enclosed_expression)),

    computed_comment_constructor: ($) => seq('comment', field('content', $.enclosed_expression)),

    computed_processing_instruction_constructor: ($) => seq(
      'processing-instruction', $._computed_node_ncname, field('content', $.enclosed_expression),
    ),

    // CompNodeName ::= QNameLiteral | ("{" Expr "}")
    _computed_node_name: ($) => choice(
      field('name', $.qname_literal),
      seq('{', field('name_expression', $._expression), '}'),
    ),

    // CompNodeNCName ::= MarkedNCName | ("{" Expr "}")
    _computed_node_ncname: ($) => choice(
      field('name', $.marked_ncname),
      seq('{', field('name_expression', $._expression), '}'),
    ),

    marked_ncname: ($) => seq('#', field('name', $._ncname)),

    // FunctionItemExpr ::= NamedFunctionRef | InlineFunctionExpr
    _function_item_expression: ($) => choice($.named_function_reference, $.inline_function_expression),

    named_function_reference: ($) => seq(
      field('name', $._function_eq_name), '#', field('arity', $.integer_literal),
    ),

    // InlineFunctionExpr ::= ("function" | "fn") FunctionSignature? FunctionBody
    inline_function_expression: ($) => seq(
      field('keyword', choice('function', 'fn')),
      optional($._function_signature),
      field('body', $.enclosed_expression),
    ),

    // FunctionSignature ::= "(" ParamList ")" TypeDeclaration?
    _function_signature: ($) => seq(
      '(', commaSeparated(field('parameter', $.variable_name_and_type)), ')',
      optional(field('return_type', $.type_declaration)),
    ),

    // MapConstructor ::= "map"? "{" (MapConstructorEntry ** ",") "}"
    map_constructor: ($) => seq(
      optional('map'), '{', commaSeparated(field('entry', $.map_constructor_entry)), '}',
    ),

    map_constructor_entry: ($) => seq(
      field('key', $._expression_single),
      optional(seq(':', field('value', $._expression_single))),
    ),

    _array_constructor: ($) => choice($.square_array_constructor, $.curly_array_constructor),

    square_array_constructor: ($) => seq(
      '[', commaSeparated(field('member', $._expression_single)), ']',
    ),

    curly_array_constructor: ($) => seq('array', field('content', $.enclosed_expression)),

    // StringTemplate ::= "`" (StringTemplateFixedPart | StringTemplateVariablePart)* "`"
    string_template: ($) => seq(
      '`',
      repeat(field('part', choice($.string_template_fixed_part, $.enclosed_expression))),
      '`',
    ),

    enclosed_expression: ($) => seq('{', optional(field('expression', $._expression)), '}'),

    // Sequence types (XPath 4.0 3.1)
    type_declaration: ($) => seq('as', field('type', $.sequence_type)),

    // SequenceType ::= ("empty-sequence" "(" ")") | (ItemType OccurrenceIndicator?)
    // A.1.2 occurrence-indicators: an indicator binds to the last ItemType.
    sequence_type: ($) => choice(
      seq(field('empty', 'empty-sequence'), '(', ')'),
      prec.right(seq(field('item_type', $._item_type), optional(field('occurrence', $.occurrence_indicator)))),
    ),

    occurrence_indicator: ($) => choice('?', '*', '+'),

    // ItemType ::= RegularItemType | FunctionType | TypeName | ChoiceItemType
    _item_type: ($) => choice(
      $._regular_item_type,
      $._function_type,
      $._type_name,
      $.choice_item_type,
    ),

    _type_name: ($) => $._eq_name,

    _regular_item_type: ($) => choice(
      $.any_item_type,
      $._xnode_type,
      $.gnode_type,
      $.jnode_type,
      $.any_map_type,
      $.typed_map_type,
      $.any_array_type,
      $.typed_array_type,
      $.any_record_type,
      $.typed_record_type,
      $.enumeration_type,
    ),

    any_item_type: ($) => seq('item', '(', ')'),

    gnode_type: ($) => seq('node', '(', ')'),

    // JNodeType ::= "jnode" "(" (("*" | JRootSelector | NCName | Constant) ("," SequenceType)?)? ")"
    jnode_type: ($) => seq(
      'jnode', '(',
      optional(seq(
        field('selector', choice(
          alias('*', $.wildcard),
          alias(seq('(', ')'), $.jroot_selector),
          $._ncname,
          $._constant,
        )),
        optional(seq(',', field('type', $.sequence_type))),
      )),
      ')',
    ),

    // Constant ::= StringLiteral | ("-"? NumericLiteral) | QNameLiteral | ("true" "(" ")") | ("false" "(" ")")
    _constant: ($) => choice(
      $.string_literal,
      $._numeric_literal,
      $.negative_numeric_literal,
      $.qname_literal,
      $.boolean_constant,
    ),

    negative_numeric_literal: ($) => seq('-', field('value', $._numeric_literal)),

    boolean_constant: ($) => seq(field('value', choice('true', 'false')), '(', ')'),

    _xnode_type: ($) => choice(
      $.document_node_type,
      $.element_node_type,
      $.attribute_node_type,
      $.schema_element_node_type,
      $.schema_attribute_node_type,
      $.processing_instruction_node_type,
      $.comment_node_type,
      $.text_node_type,
      $.namespace_node_type,
      $.any_xnode_type,
    ),

    document_node_type: ($) => seq(
      'document-node', '(',
      optional(field('content', choice($.element_node_type, $.schema_element_node_type, $.name_test_union))),
      ')',
    ),

    element_node_type: ($) => seq(
      'element', '(',
      optional(seq(
        field('name', $.name_test_union),
        optional(seq(',', field('type', $._type_name), optional(field('nillable', '?')))),
      )),
      ')',
    ),

    attribute_node_type: ($) => seq(
      'attribute', '(',
      optional(seq(
        field('name', $.name_test_union),
        optional(seq(',', field('type', $._type_name))),
      )),
      ')',
    ),

    schema_element_node_type: ($) => seq('schema-element', '(', field('name', $._eq_name), ')'),

    schema_attribute_node_type: ($) => seq('schema-attribute', '(', field('name', $._eq_name), ')'),

    processing_instruction_node_type: ($) => seq(
      'processing-instruction', '(', optional(field('name', choice($._ncname, $.string_literal))), ')',
    ),

    comment_node_type: ($) => seq('comment', '(', ')'),

    text_node_type: ($) => seq('text', '(', ')'),

    namespace_node_type: ($) => seq('namespace-node', '(', ')'),

    any_xnode_type: ($) => seq('xnode', '(', ')'),

    // NameTestUnion ::= (NameTest ++ "|"); NameTest ::= EQName | Wildcard
    name_test_union: ($) => separated1('|', field('test', choice($._eq_name, $.wildcard))),

    any_map_type: ($) => seq('map', '(', '*', ')'),

    typed_map_type: ($) => seq(
      'map', '(', field('key', $._item_type), ',', field('value', $.sequence_type), ')',
    ),

    any_array_type: ($) => seq('array', '(', '*', ')'),

    typed_array_type: ($) => seq('array', '(', field('member', $.sequence_type), ')'),

    any_record_type: ($) => seq('record', '(', '*', ')'),

    typed_record_type: ($) => seq(
      'record', '(', commaSeparated(field('field', $.field_declaration)), ')',
    ),

    // FieldDeclaration ::= FieldName ("as" SequenceType)?
    field_declaration: ($) => seq(
      field('name', choice($._ncname, $.string_literal)),
      optional(seq('as', field('type', $.sequence_type))),
    ),

    enumeration_type: ($) => seq(
      'enum', '(', commaSeparated1(field('value', $.string_literal)), ')',
    ),

    _function_type: ($) => choice($.any_function_type, $.typed_function_type),

    any_function_type: ($) => seq(field('keyword', choice('function', 'fn')), '(', '*', ')'),

    typed_function_type: ($) => seq(
      field('keyword', choice('function', 'fn')),
      '(', commaSeparated(field('parameter', $.typed_function_parameter)), ')',
      'as', field('return_type', $.sequence_type),
    ),

    // TypedFunctionParam ::= ("$" EQName "as")? SequenceType
    typed_function_parameter: ($) => seq(
      optional(seq('$', field('name', $._eq_name), 'as')),
      field('type', $.sequence_type),
    ),

    // ChoiceItemType ::= "(" (ItemType ++ "|") ")"
    choice_item_type: ($) => seq('(', separated1('|', field('alternative', $._item_type)), ')'),

    // Names. A.3: keywords are not reserved, so each keyword is also an
    // NCName; A.4 excludes the reserved function names from function calls.
    _eq_name: ($) => choice($._ncname, $.prefixed_name, $.uri_qualified_name),

    _function_eq_name: ($) => choice($._function_ncname, $.prefixed_name, $.uri_qualified_name),

    _ncname: ($) => choice($.ncname, $._keyword_name),

    _function_ncname: ($) => choice($.ncname, $._function_keyword_name),

    _keyword_name: ($) => keywordAliases($, lexical.XPATH_KEYWORDS),

    _function_keyword_name: ($) => keywordAliases(
      $, without(lexical.XPATH_KEYWORDS, lexical.RESERVED_FUNCTION_NAMES),
    ),

    // Terminal symbols (A.3.1)
    ncname: ($) => new RustRegex(lexical.NCNAME),

    prefixed_name: ($) => token(seq(new RustRegex(lexical.NCNAME), ':', new RustRegex(lexical.NCNAME))),

    // URIQualifiedName ::= BracedURILiteral (NCName ":")? NCName
    uri_qualified_name: ($) => token(seq(
      'Q{', /[^{}]*/, '}',
      optional(seq(new RustRegex(lexical.NCNAME), ':')),
      new RustRegex(lexical.NCNAME),
    )),

    integer_literal: ($) => new RustRegex(lexical.DIGITS),

    hex_integer_literal: ($) => new RustRegex('0x' + lexical.HEX_DIGITS),

    binary_integer_literal: ($) => new RustRegex('0b' + lexical.BINARY_DIGITS),

    decimal_literal: ($) => new RustRegex(`\\.${lexical.DIGITS}|${lexical.DIGITS}\\.(?:${lexical.DIGITS})?`),

    double_literal: ($) => new RustRegex(
      `(?:\\.${lexical.DIGITS}|${lexical.DIGITS}(?:\\.(?:${lexical.DIGITS})?)?)[eE][+-]?${lexical.DIGITS}`,
    ),

    string_literal: ($) => token(choice(
      seq('"', repeat(choice(/[^"]/, '""')), '"'),
      seq('\'', repeat(choice(/[^']/, '\'\'')), '\''),
    )),
  },
});
