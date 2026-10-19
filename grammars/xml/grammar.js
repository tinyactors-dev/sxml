/**
 * @file tree-sitter grammar for XML 1.0 documents (sxml, docs/DECISIONS.md D12).
 * @license MIT
 *
 * Follows Extensible Markup Language (XML) 1.0 (Fifth Edition),
 * https://www.w3.org/TR/xml/. Production numbers in comments ([1], [39], ...)
 * refer to that specification. The grammar is strict: any input that it
 * cannot parse without ERROR or MISSING nodes is not well-formed. What it
 * does not check is listed in grammars/xml/README.md and is the job of
 * sxml's tree builder.
 *
 * Node names spell out the specification's production names in snake_case
 * (STag -> start_tag, CharData -> character_data).
 *
 * The structure follows tree-sitter-grammars/tree-sitter-xml (MIT), which
 * was studied but not copied; see grammars/xml/NOTICE.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// [2] Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
const CHARACTER_RANGES = [
  [0x9, 0xa],
  [0xd, 0xd],
  [0x20, 0xd7ff],
  [0xe000, 0xfffd],
  [0x10000, 0x10ffff],
];

// [4] NameStartChar
const NAME_START_RANGES = [
  [0x3a, 0x3a], // ':'
  [0x41, 0x5a], // A-Z
  [0x5f, 0x5f], // '_'
  [0x61, 0x7a], // a-z
  [0xc0, 0xd6],
  [0xd8, 0xf6],
  [0xf8, 0x2ff],
  [0x370, 0x37d],
  [0x37f, 0x1fff],
  [0x200c, 0x200d],
  [0x2070, 0x218f],
  [0x2c00, 0x2fef],
  [0x3001, 0xd7ff],
  [0xf900, 0xfdcf],
  [0xfdf0, 0xfffd],
  [0x10000, 0xeffff],
];

// [4a] NameChar ::= NameStartChar | "-" | "." | [0-9] | #xB7 | [#x0300-#x036F] | [#x203F-#x2040]
const NAME_RANGES = NAME_START_RANGES.concat([
  [0x2d, 0x2e], // '-' '.'
  [0x30, 0x39], // 0-9
  [0xb7, 0xb7],
  [0x300, 0x36f],
  [0x203f, 0x2040],
]);

/**
 * Removes the given characters from a list of codepoint ranges.
 * @param {number[][]} ranges
 * @param {string} excluded_characters
 * @returns {number[][]}
 */
function without(ranges, excluded_characters) {
  const excluded = [...excluded_characters].map((character) => character.codePointAt(0) ?? 0);
  let result = ranges;
  for (const codepoint of excluded) {
    result = result.flatMap(([first, last]) => {
      if (codepoint < first || codepoint > last) return [[first, last]];
      const pieces = [];
      if (first < codepoint) pieces.push([first, codepoint - 1]);
      if (codepoint < last) pieces.push([codepoint + 1, last]);
      return pieces;
    });
  }
  return result;
}

/** @param {number} codepoint */
const escape = (codepoint) => `\\u{${codepoint.toString(16)}}`;

/**
 * A regular-expression character class matching the given ranges.
 * @param {number[][]} ranges
 */
function character_class(ranges) {
  const parts = ranges.map(([first, last]) =>
    first === last ? escape(first) : `${escape(first)}-${escape(last)}`);
  return `[${parts.join('')}]`;
}

/**
 * One or more characters from the given ranges.
 * @param {number[][]} ranges
 */
const one_or_more = (ranges) => new RegExp(`${character_class(ranges)}+`, 'u');

// [5] Name ::= NameStartChar (NameChar)*
const NAME = new RegExp(
  `${character_class(NAME_START_RANGES)}${character_class(NAME_RANGES)}*`, 'u');

// [7] Nmtoken ::= (NameChar)+
const NAME_TOKEN = one_or_more(NAME_RANGES);

// [13] PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] | [-'()+,./:=?;!*#@$_%]
const PUBLIC_ID_CHARACTERS = "\\u{20}\\u{d}\\u{a}a-zA-Z0-9\\-()+,./:=?;!*#@$_%";

/**
 * Both quoting styles of a quoted value: '"' ... '"' | "'" ... "'".
 * @param {(quote: string) => RuleOrLiteral} body
 */
const quoted = (body) => choice(seq('"', body('"'), '"'), seq("'", body("'"), "'"));

/**
 * [71] GEDecl ::= '<!ENTITY' S Name S EntityDef S? '>'
 * [73] EntityDef ::= EntityValue | (ExternalID NDataDecl?)
 * [76] NDataDecl ::= S 'NDATA' S Name
 * @param {GrammarSymbols<string>} $
 * @param {RuleOrLiteral} entity_value
 */
const general_entity_declaration = ($, entity_value) => seq(
  '<!ENTITY',
  $._whitespace,
  field('name', $.name),
  $._whitespace,
  choice(
    field('value', entity_value),
    seq(
      field('external_id', $.external_identifier),
      optional(seq($._whitespace, 'NDATA', $._whitespace, field('notation', $.name))),
    ),
  ),
  optional($._whitespace),
  '>',
);

/**
 * [72] PEDecl ::= '<!ENTITY' S '%' S Name S PEDef S? '>'
 * [74] PEDef ::= EntityValue | ExternalID
 * @param {GrammarSymbols<string>} $
 * @param {RuleOrLiteral} entity_value
 */
const parameter_entity_declaration = ($, entity_value) => seq(
  '<!ENTITY',
  $._whitespace,
  '%',
  $._whitespace,
  field('name', $.name),
  $._whitespace,
  choice(
    field('value', entity_value),
    field('external_id', $.external_identifier),
  ),
  optional($._whitespace),
  '>',
);

module.exports = grammar({
  name: 'xml',

  // Markup is whitespace-sensitive: every [3] S is explicit.
  extras: (_) => [],

  // Tokens produced by src/scanner.c. The order must match its token_type enum.
  externals: ($) => [
    $._start_tag_name,
    $._end_tag_name,
    $._empty_element_tag_close,
    $.character_data,
    $.cdata_text,
    $.comment_text,
    $._processing_instruction_target,
    $.processing_instruction_content,
    $.unexpanded_declaration,
    $.ignored_text,
    $._error_sentinel,
  ],

  rules: {
    // [1] document ::= prolog element Misc*
    // [22] prolog ::= XMLDecl? Misc* (doctypedecl Misc*)?
    // The XML declaration, if present, starts at the first character. A
    // byte order mark before it (4.3.3) is not part of the document.
    //
    // The same start rule also reads the external entities of a document,
    // each after a marker that the tree builder puts in front of the
    // entity's text and that cannot start a document: U+0001, which is no
    // [2] Char, and a letter. A document never yields these alternatives:
    // the tree builder accepts them only for the entities it asks for.
    document: ($) => choice(
      seq(
        optional($._byte_order_mark),
        optional(field('declaration', $.xml_declaration)),
        repeat($._misc),
        optional(seq(field('doctype', $.doctype_declaration), repeat($._misc))),
        field('root', $.element),
        repeat($._misc),
      ),
      seq($._external_subset_marker, optional(field('external_subset', $.external_subset))),
      seq($._external_entity_marker, optional(field('external_entity', $.external_parsed_entity))),
    ),

    _external_subset_marker: (_) => '\u0001S',
    _external_entity_marker: (_) => '\u0001E',

    // [30] extSubset ::= TextDecl? extSubsetDecl
    // [31] extSubsetDecl ::= ( markupdecl | conditionalSect | DeclSep)*
    // Also the replacement text of a parameter entity that is read in the
    // external subset or in an external parameter entity (2.8), where
    // parameter-entity references may occur inside declarations; the tree
    // builder refuses a text declaration there.
    external_subset: ($) => choice(
      seq(field('declaration', $.text_declaration), repeat($._external_subset_item)),
      repeat1($._external_subset_item),
    ),

    _external_subset_item: ($) => choice(
      $._external_markup_declaration,
      $.include_section,
      $.conditional_section,
      $.parameter_entity_reference,
      $._whitespace,
    ),

    // [78] extParsedEnt ::= TextDecl? content
    // An external parsed general entity referenced in content (4.3.2).
    // Element Type Match holds within it: the tag stack starts empty.
    external_parsed_entity: ($) => choice(
      seq(field('declaration', $.text_declaration), repeat($._content)),
      repeat1($._content),
    ),

    // [77] TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'
    text_declaration: ($) => seq(
      '<?',
      'xml',
      optional($._version_information),
      $._encoding_declaration,
      optional($._whitespace),
      '?>',
    ),

    // [29] markupdecl in the external subset and in external parameter
    // entities: entity values may hold parameter-entity references
    // (4.4.5), and a declaration with a parameter-entity reference outside
    // its literals is one unexpanded_declaration token from src/scanner.c,
    // which the tree builder expands (4.4.8, Included as PE) and parses
    // again.
    _external_markup_declaration: ($) => choice(
      $.element_declaration,
      $.attribute_list_declaration,
      alias($._external_general_entity_declaration, $.general_entity_declaration),
      alias($._external_parameter_entity_declaration, $.parameter_entity_declaration),
      $.notation_declaration,
      $.processing_instruction,
      $.comment,
      $.unexpanded_declaration,
    ),

    // [61] conditionalSect ::= includeSect | ignoreSect
    // [62] includeSect ::= '<![' S? 'INCLUDE' S? '[' extSubsetDecl ']]>'
    include_section: ($) => seq(
      '<![',
      optional($._whitespace),
      'INCLUDE',
      optional($._whitespace),
      '[',
      repeat($._external_subset_item),
      ']]>',
    ),

    // [63] ignoreSect ::= '<![' S? 'IGNORE' S? '[' ignoreSectContents* ']]>'
    // The keyword may also be a parameter-entity reference (2.8); its
    // replacement text decides, so everything after the reference up to the
    // section's end is read as ignored text here, and the tree builder reads
    // the keyword, the '[' and the contents from the replacement text and
    // that text (the '[' may come from the entity: VC: Proper Conditional
    // Section/PE Nesting is a validity constraint only), parsing the contents
    // again when the keyword turns out to be INCLUDE.
    conditional_section: ($) => seq(
      '<![',
      optional($._whitespace),
      choice(
        seq(
          field('keyword', 'IGNORE'),
          optional($._whitespace),
          '[',
          optional(field('contents', $.ignored_section_contents)),
        ),
        seq(
          field('keyword', $.parameter_entity_reference),
          optional(field('contents', $.ignored_section_contents)),
        ),
      ),
      ']]>',
    ),

    // [64] ignoreSectContents ::= Ignore ('<![' ignoreSectContents ']]>' Ignore)*
    // [65] Ignore ::= Char* - (Char* ('<![' | ']]>') Char*)
    ignored_section_contents: ($) => repeat1(choice(
      $.ignored_text,
      seq('<![', optional($.ignored_section_contents), ']]>'),
    )),

    _byte_order_mark: (_) => /\u{feff}/u,

    // [27] Misc ::= Comment | PI | S
    _misc: ($) => choice($.comment, $.processing_instruction, $._whitespace),

    // [23] XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'
    xml_declaration: ($) => seq(
      '<?',
      'xml',
      $._version_information,
      optional($._encoding_declaration),
      optional($._standalone_declaration),
      optional($._whitespace),
      '?>',
    ),

    // [24] VersionInfo ::= S 'version' Eq ("'" VersionNum "'" | '"' VersionNum '"')
    _version_information: ($) => seq(
      $._whitespace,
      'version',
      $._equals,
      quoted(() => field('version', $.version_number)),
    ),

    // [26] VersionNum ::= '1.' [0-9]+
    version_number: (_) => /1\.[0-9]+/,

    // [80] EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' | "'" EncName "'" )
    _encoding_declaration: ($) => seq(
      $._whitespace,
      'encoding',
      $._equals,
      quoted(() => field('encoding', $.encoding_name)),
    ),

    // [81] EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*
    encoding_name: (_) => /[A-Za-z][A-Za-z0-9._\-]*/,

    // [32] SDDecl ::= S 'standalone' Eq (("'" ('yes' | 'no') "'") | ('"' ('yes' | 'no') '"'))
    _standalone_declaration: ($) => seq(
      $._whitespace,
      'standalone',
      $._equals,
      quoted(() => field('standalone', $.standalone_value)),
    ),

    standalone_value: (_) => token(choice('yes', 'no')),

    // [25] Eq ::= S? '=' S?
    _equals: ($) => seq(optional($._whitespace), '=', optional($._whitespace)),

    // [28] doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? ('[' intSubset ']' S?)? '>'
    doctype_declaration: ($) => seq(
      '<!DOCTYPE',
      $._whitespace,
      field('name', $.name),
      optional(seq($._whitespace, field('external_id', $.external_identifier))),
      optional($._whitespace),
      optional(seq(
        '[',
        optional(field('internal_subset', $.internal_subset)),
        ']',
        optional($._whitespace),
      )),
      '>',
    ),

    // [28b] intSubset ::= (markupdecl | DeclSep)*
    // [28a] DeclSep ::= PEReference | S
    // WFC: PEs in Internal Subset (2.8): parameter-entity references occur
    // only between declarations, never inside one, so no rule below the
    // declarations accepts a PEReference.
    internal_subset: ($) => repeat1(choice(
      $._markup_declaration,
      $.parameter_entity_reference,
      $._whitespace,
    )),

    // [29] markupdecl ::= elementdecl | AttlistDecl | EntityDecl | NotationDecl | PI | Comment
    _markup_declaration: ($) => choice(
      $.element_declaration,
      $.attribute_list_declaration,
      $.general_entity_declaration,
      $.parameter_entity_declaration,
      $.notation_declaration,
      $.processing_instruction,
      $.comment,
    ),

    // [45] elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>'
    element_declaration: ($) => seq(
      '<!ELEMENT',
      $._whitespace,
      field('name', $.name),
      $._whitespace,
      field('content', $.content_specification),
      optional($._whitespace),
      '>',
    ),

    // [46] contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
    content_specification: ($) => choice(
      'EMPTY',
      'ANY',
      $.mixed_content,
      $.element_content,
    ),

    // [51] Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*'
    //              | '(' S? '#PCDATA' S? ')'
    mixed_content: ($) => choice(
      seq(
        '(',
        optional($._whitespace),
        '#PCDATA',
        repeat1(seq(optional($._whitespace), '|', optional($._whitespace), $.name)),
        optional($._whitespace),
        ')',
        '*',
      ),
      seq(
        '(',
        optional($._whitespace),
        '#PCDATA',
        optional($._whitespace),
        ')',
        optional('*'),
      ),
    ),

    // [47] children ::= (choice | seq) ('?' | '*' | '+')?
    element_content: ($) => seq(
      choice($.choice_group, $.sequence_group),
      optional($._quantifier),
    ),

    // [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
    content_particle: ($) => seq(
      choice($.name, $.choice_group, $.sequence_group),
      optional($._quantifier),
    ),

    _quantifier: (_) => choice('?', '*', '+'),

    // [49] choice ::= '(' S? cp ( S? '|' S? cp )+ S? ')'
    choice_group: ($) => seq(
      '(',
      optional($._whitespace),
      $.content_particle,
      repeat1(seq(optional($._whitespace), '|', optional($._whitespace), $.content_particle)),
      optional($._whitespace),
      ')',
    ),

    // [50] seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
    sequence_group: ($) => seq(
      '(',
      optional($._whitespace),
      $.content_particle,
      repeat(seq(optional($._whitespace), ',', optional($._whitespace), $.content_particle)),
      optional($._whitespace),
      ')',
    ),

    // [52] AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
    attribute_list_declaration: ($) => seq(
      '<!ATTLIST',
      $._whitespace,
      field('name', $.name),
      repeat($.attribute_definition),
      optional($._whitespace),
      '>',
    ),

    // [53] AttDef ::= S Name S AttType S DefaultDecl
    attribute_definition: ($) => seq(
      $._whitespace,
      field('name', $.name),
      $._whitespace,
      field('type', $._attribute_type),
      $._whitespace,
      field('default', $.default_declaration),
    ),

    // [54] AttType ::= StringType | TokenizedType | EnumeratedType
    // [57] EnumeratedType ::= NotationType | Enumeration
    _attribute_type: ($) => choice(
      $.string_type,
      $.tokenized_type,
      $.notation_type,
      $.enumeration,
    ),

    // [55] StringType ::= 'CDATA'
    string_type: (_) => 'CDATA',

    // [56] TokenizedType ::= 'ID' | 'IDREF' | 'IDREFS' | 'ENTITY' | 'ENTITIES' | 'NMTOKEN' | 'NMTOKENS'
    tokenized_type: (_) => token(choice(
      'ID', 'IDREF', 'IDREFS', 'ENTITY', 'ENTITIES', 'NMTOKEN', 'NMTOKENS',
    )),

    // [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
    notation_type: ($) => seq(
      'NOTATION',
      $._whitespace,
      '(',
      optional($._whitespace),
      $.name,
      repeat(seq(optional($._whitespace), '|', optional($._whitespace), $.name)),
      optional($._whitespace),
      ')',
    ),

    // [59] Enumeration ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')'
    enumeration: ($) => seq(
      '(',
      optional($._whitespace),
      $.name_token,
      repeat(seq(optional($._whitespace), '|', optional($._whitespace), $.name_token)),
      optional($._whitespace),
      ')',
    ),

    // [60] DefaultDecl ::= '#REQUIRED' | '#IMPLIED' | (('#FIXED' S)? AttValue)
    default_declaration: ($) => choice(
      '#REQUIRED',
      '#IMPLIED',
      seq(optional(seq('#FIXED', $._whitespace)), field('value', $.attribute_value)),
    ),

    // [71] GEDecl, [72] PEDecl: see the functions above.
    general_entity_declaration: ($) => general_entity_declaration($, $.entity_value),

    _external_general_entity_declaration: ($) =>
      general_entity_declaration($, alias($._external_entity_value, $.entity_value)),

    parameter_entity_declaration: ($) => parameter_entity_declaration($, $.entity_value),

    _external_parameter_entity_declaration: ($) =>
      parameter_entity_declaration($, alias($._external_entity_value, $.entity_value)),

    // [9] EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"'
    //                   | "'" ([^%&'] | PEReference | Reference)* "'"
    // No PEReference: WFC PEs in Internal Subset (2.8) forbids it inside a
    // declaration of the internal subset.
    entity_value: ($) => quoted((quote) => repeat(choice(
      alias(
        quote === '"' ? $._entity_value_text_in_double_quotes : $._entity_value_text_in_single_quotes,
        $.entity_value_text),
      $.entity_reference,
      $.character_reference,
    ))),

    // The same in the external subset and external parameter entities,
    // where a parameter-entity reference in an entity value is included in
    // the literal (4.4.5).
    _external_entity_value: ($) => quoted((quote) => repeat(choice(
      alias(
        quote === '"' ? $._entity_value_text_in_double_quotes : $._entity_value_text_in_single_quotes,
        $.entity_value_text),
      $.entity_reference,
      $.character_reference,
      $.parameter_entity_reference,
    ))),

    _entity_value_text_in_double_quotes: (_) => one_or_more(without(CHARACTER_RANGES, '%&"')),
    _entity_value_text_in_single_quotes: (_) => one_or_more(without(CHARACTER_RANGES, "%&'")),

    // [75] ExternalID ::= 'SYSTEM' S SystemLiteral | 'PUBLIC' S PubidLiteral S SystemLiteral
    external_identifier: ($) => choice(
      seq('SYSTEM', $._whitespace, field('system_id', $.system_literal)),
      seq(
        'PUBLIC',
        $._whitespace,
        field('public_id', $.public_id_literal),
        $._whitespace,
        field('system_id', $.system_literal),
      ),
    ),

    // [82] NotationDecl ::= '<!NOTATION' S Name S (ExternalID | PublicID) S? '>'
    // [83] PublicID ::= 'PUBLIC' S PubidLiteral
    // ExternalID and PublicID are merged here so that the parser needs no
    // decision after the public identifier.
    notation_declaration: ($) => seq(
      '<!NOTATION',
      $._whitespace,
      field('name', $.name),
      $._whitespace,
      choice(
        seq('SYSTEM', $._whitespace, field('system_id', $.system_literal)),
        seq(
          'PUBLIC',
          $._whitespace,
          field('public_id', $.public_id_literal),
          optional(seq($._whitespace, field('system_id', $.system_literal))),
        ),
      ),
      optional($._whitespace),
      '>',
    ),

    // [11] SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'")
    // The token includes the quotes.
    system_literal: (_) => token(choice(
      new RegExp(`"${character_class(without(CHARACTER_RANGES, '"'))}*"`, 'u'),
      new RegExp(`'${character_class(without(CHARACTER_RANGES, "'"))}*'`, 'u'),
    )),

    // [12] PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"
    // The token includes the quotes.
    public_id_literal: (_) => token(choice(
      new RegExp(`"[${PUBLIC_ID_CHARACTERS}']*"`, 'u'),
      new RegExp(`'[${PUBLIC_ID_CHARACTERS}]*'`, 'u'),
    )),

    // [69] PEReference ::= '%' Name ';'
    parameter_entity_reference: ($) => seq('%', field('name', $.name), ';'),

    // [39] element ::= EmptyElemTag | STag content ETag
    // [43] content ::= CharData? ((element | Reference | CDSect | PI | Comment) CharData?)*
    // The content items are children of the element, between its tags.
    element: ($) => choice(
      $.empty_element_tag,
      seq($.start_tag, repeat($._content), $.end_tag),
    ),

    _content: ($) => choice(
      $.character_data,
      $.element,
      $.entity_reference,
      $.character_reference,
      $.cdata_section,
      $.processing_instruction,
      $.comment,
    ),

    // [40] STag ::= '<' Name (S Attribute)* S? '>'
    // The external scanner pushes the name on its tag stack.
    start_tag: ($) => seq(
      '<',
      field('name', alias($._start_tag_name, $.name)),
      repeat(seq($._whitespace, $.attribute)),
      optional($._whitespace),
      '>',
    ),

    // [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
    // The external scanner pops the name again at '/>'.
    empty_element_tag: ($) => seq(
      '<',
      field('name', alias($._start_tag_name, $.name)),
      repeat(seq($._whitespace, $.attribute)),
      optional($._whitespace),
      alias($._empty_element_tag_close, '/>'),
    ),

    // [42] ETag ::= '</' Name S? '>'
    // WFC: Element Type Match (3): the external scanner accepts the name only
    // if it equals the name of the innermost open start tag.
    end_tag: ($) => seq(
      '</',
      field('name', alias($._end_tag_name, $.name)),
      optional($._whitespace),
      '>',
    ),

    // [41] Attribute ::= Name Eq AttValue
    attribute: ($) => seq(
      field('name', $.name),
      $._equals,
      field('value', $.attribute_value),
    ),

    // [10] AttValue ::= '"' ([^<&"] | Reference)* '"' | "'" ([^<&'] | Reference)* "'"
    attribute_value: ($) => quoted((quote) => repeat(choice(
      alias(
        quote === '"' ? $._attribute_value_text_in_double_quotes : $._attribute_value_text_in_single_quotes,
        $.attribute_value_text),
      $.entity_reference,
      $.character_reference,
    ))),

    _attribute_value_text_in_double_quotes: (_) => one_or_more(without(CHARACTER_RANGES, '<&"')),
    _attribute_value_text_in_single_quotes: (_) => one_or_more(without(CHARACTER_RANGES, "<&'")),

    // [18] CDSect ::= CDStart CData CDEnd
    // [20] CData ::= (Char* - (Char* ']]>' Char*))
    // CData may arrive as several adjacent cdata_text nodes (see src/scanner.c).
    cdata_section: ($) => seq('<![CDATA[', repeat($.cdata_text), ']]>'),

    // [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
    comment: ($) => seq('<!--', optional($.comment_text), '-->'),

    // [16] PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
    // [17] PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))
    processing_instruction: ($) => seq(
      '<?',
      field('target', alias($._processing_instruction_target, $.name)),
      optional(seq($._whitespace, optional(field('content', $.processing_instruction_content)))),
      '?>',
    ),

    // [67] Reference ::= EntityRef | CharRef
    // [68] EntityRef ::= '&' Name ';'
    entity_reference: ($) => seq('&', field('name', $.name), ';'),

    // [66] CharRef ::= '&#' [0-9]+ ';' | '&#x' [0-9a-fA-F]+ ';'
    character_reference: (_) => token(choice(/&#[0-9]+;/, /&#x[0-9a-fA-F]+;/)),

    name: (_) => NAME,

    name_token: (_) => NAME_TOKEN,

    // [3] S ::= (#x20 | #x9 | #xD | #xA)+
    _whitespace: (_) => /[\u{20}\u{9}\u{d}\u{a}]+/u,
  },
});
