/**
 * XQuery 4.0 grammar for sxml (docs/DECISIONS.md, D12 and D29).
 *
 * Source of truth: XQuery 4.0, W3C QT4 CG draft, Appendix A
 * (https://qt4cg.org/specifications/xquery-40/xquery-40.html#nt-bnf).
 * Inherits the XPath 4.0 grammar and adds the prolog and modules, the full
 * FLWOR clauses, switch, typeswitch, try/catch, validate, extension
 * expressions, direct constructors and string constructors.
 *
 * Also the XQuery Update Facility 3.0 (W3C Recommendation, 24 January
 * 2017, Appendix A): the revalidation declaration, the updating
 * compatibility annotation, the insert, delete, replace, rename,
 * copy-modify and transform-with expressions and updating function calls.
 *
 * Also XQuery and XPath Full Text 3.0 (W3C Recommendation, 24 November
 * 2015, Appendix A; D214): the contains text expression with its
 * full-text selections, positional filters and match options, the
 * ft-option declaration and the score variables of for and let. Full Text
 * is in this grammar only, so XPath hosts do not pay for its tables.
 */

const xpath = require('../xpath/grammar');
const lexical = require('../xpath/lexical');
const fullText = require('../xpath/full_text');

const commaSeparated1 = (rule) => seq(rule, repeat(seq(',', rule)));
const commaSeparated = (rule) => optional(commaSeparated1(rule));
const separated1 = (separator, rule) => seq(rule, repeat(seq(separator, rule)));

const keywordAliases = ($, keywords) =>
  choice(...keywords.map((keyword) => alias(keyword, $.ncname)));

const without = (list, removed) => list.filter((entry) => !removed.includes(entry));

// WindowVars ::= CurrentVar? PositionalVar? PreviousVar? NextVar?
// (a JavaScript helper, as tree-sitter rules may not match the empty string)
const windowVariables = ($) => seq(
  optional(seq('$', field('current', $._eq_name))),
  optional(field('position', $.positional_variable)),
  optional(seq('previous', '$', field('previous', $._eq_name))),
  optional(seq('next', '$', field('next', $._eq_name))),
);

// XQuery Update Facility 3.0, A.1: further literal terminals spelled as
// names (the others, such as "node", "value" and "with", are XPath's).
const UPDATE_KEYWORDS = [
  'after', 'before', 'copy', 'delete', 'first', 'insert', 'into', 'invoke',
  'last', 'modify', 'nodes', 'rename', 'replace', 'revalidation', 'skip',
  'transform', 'updating',
];

// XQuery and XPath Full Text 3.0, A.1: further literal terminals spelled
// as names (the others, such as "at", "case", "end", "option", "ordered",
// "start", "union", "except" and "window", are XPath's or XQuery's). They
// are not reserved words: the lexer returns one only where the parse state
// expects it (tree-sitter's keyword extraction) and an NCName everywhere
// else, which keeps the name positions and the parser tables as they were
// (D214). No position accepts both one of them and a name.
const KEYWORDS = lexical.XPATH_KEYWORDS.concat(lexical.XQUERY_KEYWORDS, UPDATE_KEYWORDS);

// XQuery 4.0 has no namespace axis (A.1 Axis).
const AXES = [
  'ancestor', 'ancestor-or-self', 'attribute', 'child', 'descendant',
  'descendant-or-self', 'following', 'following-or-self', 'following-sibling',
  'following-sibling-or-self', 'item', 'parent', 'preceding',
  'preceding-or-self', 'preceding-sibling', 'preceding-sibling-or-self', 'self',
];

// QuotStringLiteral and AposStringLiteral allow PredefinedEntityRef and
// CharRef, and no other "&" (XQuery 4.0 A.3.1).
const REFERENCE = /&(lt|gt|amp|quot|apos);|&#[0-9]+;|&#x[0-9a-fA-F]+;/;

const xquery = grammar(xpath, {
  name: 'xquery',

  reserved: {
    global: ($) => KEYWORDS,
  },

  // The Full Text choice rules (D214) leave no symbol in the parser
  // tables, which keeps their large-state rows from growing. XPath's
  // inlined choice rules (perf-17) are not taken: here they would grow
  // the tables by 560 KB (5,647 to 9,518 states), and a query is parsed
  // once while a stylesheet's expressions are many.
  inline: ($) => fullText.inline($),

  conflicts: ($, original) => original.concat([
    // "element instance {...}" (a computed constructor named "instance")
    // against "element instance of T" (a step named "element"): the
    // two-word operators instance of, treat as, cast as and castable as are
    // not excluded by xgc: unreserved-name, so the third token decides.
    [$._keyword_name, $.computed_element_constructor],
    [$._keyword_name, $.computed_attribute_constructor],
    [$._keyword_name, $.computed_namespace_constructor],
    [$._keyword_name, $.computed_processing_instruction_constructor],
    // After a prolog declaration, "declare" or "import" may start another
    // declaration of either group or be a name step at the start of the
    // query body.
    [$.prolog],
  ], fullText.conflicts($)),

  rules: {
    // Module ::= VersionDecl? (LibraryModule | MainModule)
    module: ($) => seq(
      optional(field('version', $.version_declaration)),
      choice($.library_module, $.main_module),
    ),

    // VersionDecl ::= "xquery" (("encoding" StringLiteral) | ("version" StringLiteral ("encoding" StringLiteral)?)) Separator
    version_declaration: ($) => seq(
      'xquery',
      choice(
        seq('encoding', field('encoding', $.string_literal)),
        seq('version', field('version', $.string_literal),
          optional(seq('encoding', field('encoding', $.string_literal)))),
      ),
      ';',
    ),

    // MainModule ::= Prolog QueryBody
    main_module: ($) => seq(optional(field('prolog', $.prolog)), field('body', $._expression)),

    // LibraryModule ::= ModuleDecl Prolog
    library_module: ($) => seq(
      field('declaration', $.module_declaration),
      optional(field('prolog', $.prolog)),
    ),

    module_declaration: ($) => seq(
      'module', 'namespace', field('prefix', $._ncname), '=', field('uri', $.string_literal), ';',
    ),

    // Prolog ::= ((DefaultNamespaceDecl | Setter | NamespaceDecl | Import) Separator)*
    //            ((ContextValueDecl | VarDecl | FunctionDecl | ItemTypeDecl | NominativeRecordTypeDecl | OptionDecl) Separator)*
    prolog: ($) => choice(
      seq(
        repeat1(seq(field('declaration', $._prolog_setup_declaration), ';')),
        repeat(seq(field('declaration', $._prolog_body_declaration), ';')),
      ),
      repeat1(seq(field('declaration', $._prolog_body_declaration), ';')),
    ),

    _prolog_setup_declaration: ($) => choice(
      $.default_namespace_declaration,
      $.boundary_space_declaration,
      $.default_collation_declaration,
      $.base_uri_declaration,
      $.construction_declaration,
      $.ordering_mode_declaration,
      $.empty_order_declaration,
      $.copy_namespaces_declaration,
      $.decimal_format_declaration,
      $.namespace_declaration,
      $.schema_import,
      $.module_import,
      $.revalidation_declaration,
      $.ft_option_declaration,
    ),

    _prolog_body_declaration: ($) => choice(
      $.context_value_declaration,
      $.variable_declaration,
      $.function_declaration,
      $.item_type_declaration,
      $.nominative_record_type_declaration,
      $.option_declaration,
    ),

    // DefaultNamespaceDecl ::= "declare" "fixed"? "default" ("element" | "function") "namespace" URILiteral
    default_namespace_declaration: ($) => seq(
      'declare', optional(field('fixed', 'fixed')), 'default',
      field('kind', choice('element', 'function')), 'namespace', field('uri', $.string_literal),
    ),

    boundary_space_declaration: ($) => seq(
      'declare', 'boundary-space', field('mode', choice('preserve', 'strip')),
    ),

    default_collation_declaration: ($) => seq(
      'declare', 'default', 'collation', field('uri', $.string_literal),
    ),

    // RevalidationDecl ::= "declare" "revalidation" ("strict" | "lax" | "skip")
    // (XQuery Update Facility 3.0, 5.2)
    revalidation_declaration: ($) => seq(
      'declare', 'revalidation', field('mode', choice('strict', 'lax', 'skip')),
    ),

    base_uri_declaration: ($) => seq('declare', 'base-uri', field('uri', $.string_literal)),

    construction_declaration: ($) => seq(
      'declare', 'construction', field('mode', choice('strip', 'preserve')),
    ),

    ordering_mode_declaration: ($) => seq(
      'declare', 'ordering', field('mode', choice('ordered', 'unordered')),
    ),

    empty_order_declaration: ($) => seq(
      'declare', 'default', 'order', 'empty', field('mode', choice('greatest', 'least')),
    ),

    // CopyNamespacesDecl ::= "declare" "copy-namespaces" PreserveMode "," InheritMode
    copy_namespaces_declaration: ($) => seq(
      'declare', 'copy-namespaces',
      field('preserve_mode', choice('preserve', 'no-preserve')), ',',
      field('inherit_mode', choice('inherit', 'no-inherit')),
    ),

    // DecimalFormatDecl ::= "declare" (("decimal-format" EQName) | ("default" "decimal-format")) (DFPropertyName "=" StringLiteral)*
    decimal_format_declaration: ($) => seq(
      'declare',
      choice(seq('decimal-format', field('name', $._eq_name)), seq('default', 'decimal-format')),
      repeat(field('property', $.decimal_format_property)),
    ),

    decimal_format_property: ($) => seq(
      field('name', choice(
        'decimal-separator', 'grouping-separator', 'infinity', 'minus-sign', 'NaN', 'percent',
        'per-mille', 'zero-digit', 'digit', 'pattern-separator', 'exponent-separator',
      )),
      '=',
      field('value', $.string_literal),
    ),

    // SchemaImport ::= "import" "schema" SchemaPrefix? URILiteral ("at" (URILiteral ++ ","))?
    schema_import: ($) => seq(
      'import', 'schema',
      optional(field('prefix', $.schema_prefix)),
      field('uri', $.string_literal),
      optional(seq('at', commaSeparated1(field('location', $.string_literal)))),
    ),

    schema_prefix: ($) => choice(
      seq('namespace', field('prefix', $._ncname), '='),
      seq(optional(field('fixed', 'fixed')), 'default', 'element', 'namespace'),
    ),

    // ModuleImport ::= "import" "module" ("namespace" NCName "=")? URILiteral ("at" (URILiteral ++ ","))?
    module_import: ($) => seq(
      'import', 'module',
      optional(seq('namespace', field('prefix', $._ncname), '=')),
      field('uri', $.string_literal),
      optional(seq('at', commaSeparated1(field('location', $.string_literal)))),
    ),

    // ContextValueDecl ::= "declare" "context" (("value" ("as" SequenceType)?) | ("item" ("as" ItemType)?))
    //                      ((":=" VarValue) | ("external" (":=" VarDefaultValue)?))
    context_value_declaration: ($) => seq(
      'declare', 'context',
      choice(
        seq(field('kind', 'value'), optional(seq('as', field('type', $.sequence_type)))),
        seq(field('kind', 'item'), optional(seq('as', field('type', $._item_type)))),
      ),
      $._variable_value,
    ),

    // VarDecl ::= "declare" Annotation* "variable" VarNameAndType ((":=" VarValue) | ("external" (":=" VarDefaultValue)?))
    variable_declaration: ($) => seq(
      'declare', optional(field('annotation', $.compatibility_annotation)),
      repeat(field('annotation', $.annotation)), 'variable',
      field('variable', $.variable_name_and_type),
      $._variable_value,
    ),

    _variable_value: ($) => choice(
      seq(':=', field('value', $._expression_single)),
      seq(field('external', 'external'), optional(seq(':=', field('default', $._expression_single)))),
    ),

    // FunctionDecl ::= "declare" Annotation* "function" EQName "(" ParamListWithDefaults? ")"
    //                  TypeDeclaration? (FunctionBody | "external")
    function_declaration: ($) => seq(
      'declare', optional(field('annotation', $.compatibility_annotation)),
      repeat(field('annotation', $.annotation)), 'function',
      field('name', $._function_eq_name),
      '(', commaSeparated(field('parameter', $.parameter_with_default)), ')',
      optional(field('return_type', $.type_declaration)),
      choice(field('body', $.enclosed_expression), field('external', 'external')),
    ),

    // ParamWithDefault ::= VarNameAndType (":=" ExprSingle)?
    parameter_with_default: ($) => seq(
      field('variable', $.variable_name_and_type),
      optional(seq(':=', field('default', $._expression_single))),
    ),

    // ItemTypeDecl ::= "declare" Annotation* "type" EQName "as" ItemType
    item_type_declaration: ($) => seq(
      'declare', repeat(field('annotation', $.annotation)), 'type',
      field('name', $._eq_name), 'as', field('type', $._item_type),
    ),

    // NominativeRecordTypeDecl ::= "declare" Annotation* "record" EQName "(" (ExtendedFieldDeclaration ** ",") ")"
    nominative_record_type_declaration: ($) => seq(
      'declare', repeat(field('annotation', $.annotation)), 'record',
      field('name', $._eq_name),
      '(', commaSeparated(field('field', $.extended_field_declaration)), ')',
    ),

    // ExtendedFieldDeclaration ::= "%static"? FieldDeclaration (":=" ExprSingle)?
    extended_field_declaration: ($) => seq(
      optional(field('static', '%static')),
      field('name', choice($._ncname, $.string_literal)),
      optional(seq('as', field('type', $.sequence_type))),
      optional(seq(':=', field('default', $._expression_single))),
    ),

    option_declaration: ($) => seq(
      'declare', 'option', field('name', $._eq_name), field('value', $.string_literal),
    ),

    // AnnotatedDecl ::= "declare" (CompatibilityAnnotation | Annotation)* (VarDecl | FunctionDecl)
    // (XQuery Update Facility 3.0, 5.4); the compatibility annotation is
    // accepted first, the place every test and example writes it.
    // CompatibilityAnnotation ::= "updating"
    compatibility_annotation: ($) => 'updating',

    // Annotation ::= "%" EQName ("(" (Constant ++ ",") ")")?
    annotation: ($) => seq(
      '%', field('name', $._eq_name),
      optional(seq('(', commaSeparated1(field('value', $._constant)), ')')),
    ),

    // FTOptionDecl ::= "declare" "ft-option" FTMatchOptions (Full Text 3.0, 2.1)
    ft_option_declaration: ($) => seq(
      'declare', 'ft-option', repeat1(seq('using', field('option', $._ft_match_option))),
    ),

    _expression_single: ($) => choice(
      $.flwor_expression,
      $.quantified_expression,
      $.switch_expression,
      $.typeswitch_expression,
      $.if_expression,
      $.try_catch_expression,
      $.insert_expression,
      $.delete_expression,
      $.rename_expression,
      $.replace_expression,
      $.copy_modify_expression,
      $._or_level,
    ),

    // InsertExpr ::= "insert" ("node" | "nodes") SourceExpr InsertExprTargetChoice TargetExpr
    // InsertExprTargetChoice ::= (("as" ("first" | "last"))? "into") | "after" | "before"
    // (XQuery Update Facility 3.0, 5.4.1)
    insert_expression: ($) => seq(
      'insert', choice('node', 'nodes'),
      field('source', $._expression_single),
      choice(
        seq('as', field('mode', choice('first', 'last')), 'into'),
        field('mode', choice('into', 'after', 'before')),
      ),
      field('target', $._expression_single),
    ),

    // DeleteExpr ::= "delete" ("node" | "nodes") TargetExpr (5.4.2)
    delete_expression: ($) => seq(
      'delete', choice('node', 'nodes'), field('target', $._expression_single),
    ),

    // ReplaceExpr ::= "replace" ("value" "of")? "node" TargetExpr "with" ExprSingle (5.4.3)
    replace_expression: ($) => seq(
      'replace', optional(field('value_of', seq('value', 'of'))), 'node',
      field('target', $._expression_single),
      'with', field('value', $._expression_single),
    ),

    // RenameExpr ::= "rename" "node" TargetExpr "as" NewNameExpr (5.4.4)
    rename_expression: ($) => seq(
      'rename', 'node', field('target', $._expression_single),
      'as', field('name_expression', $._expression_single),
    ),

    // CopyModifyExpr ::= "copy" "$" VarName ":=" ExprSingle ("," "$" VarName ":=" ExprSingle)*
    //                    "modify" ExprSingle "return" ExprSingle (5.4.5)
    copy_modify_expression: ($) => seq(
      'copy', commaSeparated1(field('binding', $.copy_binding)),
      'modify', field('modify', $._expression_single),
      'return', field('return', $._expression_single),
    ),

    copy_binding: ($) => seq(
      '$', field('name', $._eq_name), ':=', field('value', $._expression_single),
    ),

    // TransformWithExpr ::= ArrowExpr ("transform" "with" "{" Expr? "}")? (5.4.6);
    // XQuery 4.0's pipeline level sits above it.
    _arrow_level: ($, original) => choice(original, $.transform_with_expression),

    transform_with_expression: ($) => seq(
      field('value', $._arrow_level), 'transform', 'with',
      '{', optional(field('modify', $._expression)), '}',
    ),

    // UpdatingFunctionCall ::= "invoke" "updating" PrimaryExpr "(" (ExprSingle ++ ",")? ")" (5.3)
    updating_function_call: ($) => seq(
      'invoke', 'updating', field('function', $._primary_expression),
      field('arguments', $.positional_argument_list),
    ),

    // FLWORExpr ::= InitialClause IntermediateClause* ReturnClause
    _initial_clause: ($) => choice($.for_clause, $.let_clause, $._window_clause),

    _intermediate_clause: ($) => choice(
      $.for_clause,
      $.let_clause,
      $._window_clause,
      $.where_clause,
      $.while_clause,
      $.group_by_clause,
      $.order_by_clause,
      $.count_clause,
      $.trace_clause,
    ),

    // ForItemBinding ::= VarNameAndType AllowingEmpty? PositionalVar? "in" ExprSingle
    for_item_binding: ($) => seq(
      field('variable', $.variable_name_and_type),
      optional(field('allowing_empty', $.allowing_empty)),
      optional(field('position', $.positional_variable)),
      optional(field('score', $.ft_score_variable)),
      'in',
      field('sequence', $._expression_single),
    ),

    allowing_empty: ($) => seq('allowing', 'empty'),

    // FTScoreVar ::= "score" "$" VarName (Full Text 3.0, 2.3)
    ft_score_variable: fullText.scoreVariable,

    _let_binding: ($, original) => choice(original, $.let_score_binding),

    // LetBinding ::= ... | FTScoreVar ":=" ExprSingle (Full Text 3.0, 2.3.2)
    let_score_binding: ($) => seq(
      'score', '$', field('name', $._eq_name), ':=', field('value', $._expression_single),
    ),

    // XQuery and XPath Full Text 3.0: FTContainsExpr as the operand of
    // ComparisonExpr, its selections, filters and match options
    // (grammars/xpath/full_text.js, shared with grammars/xpath_full_text).
    ...fullText.rules,

    // WindowClause ::= "for" (TumblingWindowClause | SlidingWindowClause);
    // the "for" is part of each window clause node.
    _window_clause: ($) => choice($.tumbling_window_clause, $.sliding_window_clause),

    // TumblingWindowClause ::= "tumbling" "window" VarNameAndType "in" ExprSingle
    //                          WindowStartCondition? WindowEndCondition?
    tumbling_window_clause: ($) => seq(
      'for', 'tumbling', 'window',
      field('variable', $.variable_name_and_type),
      'in',
      field('sequence', $._expression_single),
      optional(field('start', $.window_start_condition)),
      optional(field('end', $.window_end_condition)),
    ),

    // SlidingWindowClause ::= "sliding" "window" VarNameAndType "in" ExprSingle
    //                         WindowStartCondition? WindowEndCondition
    sliding_window_clause: ($) => seq(
      'for', 'sliding', 'window',
      field('variable', $.variable_name_and_type),
      'in',
      field('sequence', $._expression_single),
      optional(field('start', $.window_start_condition)),
      field('end', $.window_end_condition),
    ),

    window_start_condition: ($) => seq(
      'start', windowVariables($), optional(seq('when', field('condition', $._expression_single))),
    ),

    window_end_condition: ($) => seq(
      optional(field('only', 'only')), 'end', windowVariables($),
      optional(seq('when', field('condition', $._expression_single))),
    ),

    where_clause: ($) => seq('where', field('condition', $._expression_single)),

    while_clause: ($) => seq('while', field('condition', $._expression_single)),

    // GroupByClause ::= "group" "by" (GroupingSpec ++ ",")
    group_by_clause: ($) => seq('group', 'by', commaSeparated1(field('specification', $.grouping_specification))),

    // GroupingSpec ::= VarName (TypeDeclaration? ":=" ExprSingle)? ("collation" URILiteral)?
    grouping_specification: ($) => seq(
      '$', field('name', $._eq_name),
      optional(seq(optional(field('type', $.type_declaration)), ':=', field('value', $._expression_single))),
      optional(seq('collation', field('collation', $.string_literal))),
    ),

    // OrderByClause ::= "stable"? "order" "by" (OrderSpec ++ ",")
    order_by_clause: ($) => seq(
      optional(field('stable', 'stable')), 'order', 'by',
      commaSeparated1(field('specification', $.order_specification)),
    ),

    // OrderSpec ::= ExprSingle OrderModifier
    // OrderModifier ::= ("ascending" | "descending")? ("empty" ("greatest" | "least"))? ("collation" URILiteral)?
    order_specification: ($) => seq(
      field('key', $._expression_single),
      optional(field('direction', choice('ascending', 'descending'))),
      optional(seq('empty', field('empty', choice('greatest', 'least')))),
      optional(seq('collation', field('collation', $.string_literal))),
    ),

    count_clause: ($) => seq('count', '$', field('name', $._eq_name)),

    trace_clause: ($) => seq('trace', field('value', $._expression_single)),

    // SwitchExpr ::= "switch" SwitchComparand (SwitchCases | BracedSwitchCases)
    switch_expression: ($) => seq(
      'switch', '(', optional(field('comparand', $._expression)), ')',
      choice($._switch_cases, seq('{', $._switch_cases, '}')),
    ),

    _switch_cases: ($) => seq(
      repeat1(field('case', $.switch_case_clause)),
      'default', 'return', field('default', $._expression_single),
    ),

    // SwitchCaseClause ::= ("case" SwitchCaseOperand)+ "return" ExprSingle
    switch_case_clause: ($) => seq(
      repeat1(seq('case', field('operand', $._expression))),
      'return', field('value', $._expression_single),
    ),

    // TypeswitchExpr ::= "typeswitch" "(" Expr ")" (TypeswitchCases | BracedTypeswitchCases)
    typeswitch_expression: ($) => seq(
      'typeswitch', '(', field('operand', $._expression), ')',
      choice($._typeswitch_cases, seq('{', $._typeswitch_cases, '}')),
    ),

    _typeswitch_cases: ($) => seq(
      repeat1(field('case', $.typeswitch_case_clause)),
      field('default', $.typeswitch_default_clause),
    ),

    // CaseClause ::= "case" (VarName "as")? SequenceTypeUnion "return" ExprSingle
    typeswitch_case_clause: ($) => seq(
      'case',
      optional(seq('$', field('variable', $._eq_name), 'as')),
      separated1('|', field('type', $.sequence_type)),
      'return', field('value', $._expression_single),
    ),

    typeswitch_default_clause: ($) => seq(
      'default', optional(seq('$', field('variable', $._eq_name))),
      'return', field('value', $._expression_single),
    ),

    // TryCatchExpr ::= TryClause ((CatchClause+ FinallyClause?) | FinallyClause)
    try_catch_expression: ($) => seq(
      'try', field('body', $.enclosed_expression),
      choice(
        seq(repeat1(field('catch', $.catch_clause)), optional(field('finally', $.finally_clause))),
        field('finally', $.finally_clause),
      ),
    ),

    catch_clause: ($) => seq(
      'catch', field('errors', $.name_test_union), field('body', $.enclosed_expression),
    ),

    finally_clause: ($) => seq('finally', field('body', $.enclosed_expression)),

    // ValueExpr ::= ValidateExpr | ExtensionExpr | SimpleMapExpr
    _value_expression: ($) => choice(
      $.validate_expression,
      $.extension_expression,
      $._simple_map_level,
    ),

    // ValidateExpr ::= "validate" (ValidationMode | ("type" TypeName))? "{" Expr "}"
    validate_expression: ($) => seq(
      'validate',
      optional(choice(
        field('mode', choice('lax', 'strict')),
        seq('type', field('type', $._type_name)),
      )),
      '{', field('expression', $._expression), '}',
    ),

    // ExtensionExpr ::= Pragma+ "{" Expr? "}"
    extension_expression: ($) => seq(
      repeat1(field('pragma', $.pragma)),
      '{', optional(field('expression', $._expression)), '}',
    ),

    // Pragma ::= "(#" S EQName (S PragmaContents)? "#)"
    pragma: fullText.pragma,

    _primary_expression: ($) => choice(
      $._literal,
      $.variable_reference,
      $.parenthesized_expression,
      $.context_value_reference,
      $.function_call,
      $.ordered_expression,
      $.unordered_expression,
      $._node_constructor,
      $._function_item_expression,
      $.map_constructor,
      $._array_constructor,
      $.string_template,
      $.string_constructor,
      $.unary_lookup,
      $.updating_function_call,
    ),

    ordered_expression: ($) => seq('ordered', field('content', $.enclosed_expression)),

    unordered_expression: ($) => seq('unordered', field('content', $.enclosed_expression)),

    // StringConstructor ::= "``[" StringConstructorContent "]``"
    string_constructor: ($) => seq(
      '``[',
      repeat(field('part', choice($.string_constructor_characters, $.string_interpolation))),
      ']``',
    ),

    // StringInterpolation ::= "`" EnclosedExpr "`"
    string_interpolation: ($) => seq('`{', optional(field('expression', $._expression)), '}`'),

    // NodeConstructor ::= DirectConstructor | ComputedConstructor
    _node_constructor: ($) => choice($._direct_constructor, $._computed_constructor),

    _direct_constructor: ($) => choice(
      $.direct_element_constructor,
      $.direct_comment_constructor,
      $.direct_processing_instruction_constructor,
    ),

    // DirElemConstructor ::= "<" QName DirAttributeList ("/>" | (">" DirElemContent* "</" QName S? ">"))
    direct_element_constructor: ($) => seq(
      alias($._direct_element_open, '<'),
      field('name', alias($._direct_tag_name, $.qname)),
      repeat(field('attribute', $.direct_attribute)),
      choice(
        alias($._direct_empty_tag_end, '/>'),
        seq(
          alias($._direct_tag_end, '>'),
          repeat(field('content', $._direct_element_content)),
          alias($._direct_end_tag_open, '</'),
          field('end_name', alias($._direct_tag_name, $.qname)),
          alias($._direct_tag_end, '>'),
        ),
      ),
    ),

    // DirAttributeList ::= (S (QName S? "=" S? DirAttributeValue)?)*
    direct_attribute: ($) => seq(
      field('name', alias($._direct_attribute_name, $.qname)),
      alias($._direct_attribute_equals, '='),
      field('value', $.direct_attribute_value),
    ),

    // DirAttributeValue ::= ('"' (EscapeQuot | QuotAttrValueContent)* '"') | ("'" (EscapeApos | AposAttrValueContent)* "'")
    direct_attribute_value: ($) => choice(
      seq(
        alias($._direct_quote_open, '"'),
        repeat(field('part', choice(alias($._direct_quote_content, $.attribute_value_text), $.enclosed_expression))),
        alias($._direct_quote_close, '"'),
      ),
      seq(
        alias($._direct_apostrophe_open, '\''),
        repeat(field('part', choice(alias($._direct_apostrophe_content, $.attribute_value_text), $.enclosed_expression))),
        alias($._direct_apostrophe_close, '\''),
      ),
    ),

    // DirElemContent ::= DirectConstructor | CDataSection | CommonContent | ElementContentChar
    _direct_element_content: ($) => choice(
      $._direct_constructor,
      $.cdata_section,
      $.direct_element_text,
      $.enclosed_expression,
    ),

    // CompNodeName ::= QNameLiteral | UnreservedName | ("{" Expr "}")
    _computed_node_name: ($) => choice(
      field('name', $.qname_literal),
      field('name', $._unreserved_eq_name),
      seq('{', field('name_expression', $._expression), '}'),
    ),

    // CompNodeNCName ::= MarkedNCName | UnreservedNCName | ("{" Expr "}")
    _computed_node_ncname: ($) => choice(
      field('name', $.marked_ncname),
      field('name', $._unreserved_ncname),
      seq('{', field('name_expression', $._expression), '}'),
    ),

    // InlineFunctionExpr ::= Annotation* ("function" | "fn") FunctionSignature? FunctionBody
    inline_function_expression: ($) => seq(
      repeat(field('annotation', $.annotation)),
      field('keyword', choice('function', 'fn')),
      optional($._function_signature),
      field('body', $.enclosed_expression),
    ),

    // FunctionType ::= Annotation* (AnyFunctionType | TypedFunctionType)
    any_function_type: ($) => seq(
      repeat(field('annotation', $.annotation)),
      field('keyword', choice('function', 'fn')), '(', '*', ')',
    ),

    typed_function_type: ($) => seq(
      repeat(field('annotation', $.annotation)),
      field('keyword', choice('function', 'fn')),
      '(', commaSeparated(field('parameter', $.typed_function_parameter)), ')',
      'as', field('return_type', $.sequence_type),
    ),

    axis: ($) => seq(field('name', choice(...AXES)), '::'),

    // Names: XQuery adds its keywords to the names that need no prefix.
    _keyword_name: ($) => keywordAliases($, KEYWORDS),

    _function_keyword_name: ($) => keywordAliases(
      $, without(KEYWORDS, lexical.RESERVED_FUNCTION_NAMES),
    ),

    // xgc: unreserved-name
    _unreserved_eq_name: ($) => choice($._unreserved_ncname, $.prefixed_name, $.uri_qualified_name),

    _unreserved_ncname: ($) => choice(
      $.ncname,
      keywordAliases($, without(KEYWORDS, lexical.CONSTRUCTOR_RESERVED_NAMES)),
    ),

    string_literal: ($) => token(choice(
      seq('"', repeat(choice(/[^"&]/, REFERENCE, '""')), '"'),
      seq('\'', repeat(choice(/[^'&]/, REFERENCE, '\'\'')), '\''),
    )),

    // BracedURILiteral ::= "Q" "{" (PredefinedEntityRef | CharRef | [^&{}])* "}"
    uri_qualified_name: ($) => token(seq(
      'Q{', repeat(choice(/[^&{}]/, REFERENCE)), '}',
      optional(seq(new RustRegex(lexical.NCNAME), ':')),
      new RustRegex(lexical.NCNAME),
    )),
  },
});

// tree-sitter takes the first rule as the start rule; make it `module`.
xquery.grammar.rules = Object.assign({ module: xquery.grammar.rules.module }, xquery.grammar.rules);

module.exports = xquery;
