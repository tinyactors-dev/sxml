#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * XPath 4.0: the static context, compilation, the dynamic context and
 * evaluation (docs/DECISIONS.md, D47, D51).
 *
 * An expression is compiled once against a static context into an
 * sxml_xpath_expression and evaluated any number of times, each time
 * against a dynamic context, giving a sequence in the processor's item
 * heap:
 *
 *     sxml_xpath_expression *expression = NULL;
 *     status = sxml_xpath_expression_create(
 *         allocator,
 *         &(sxml_xpath_expression_configuration){
 *             .processor = processor,
 *             .expression_text = SXML_BYTE_VIEW_FROM_LITERAL("$x + 1"),
 *             .static_context = {.variable_declarations = declarations},
 *             .error_record = &error_record,
 *         },
 *         &expression);
 *     ...
 *     status = sxml_xpath_expression_evaluate(
 *         expression,
 *         &(sxml_xpath_evaluation_request){
 *             .dynamic_context = {.variable_bindings = bindings},
 *             .error_record = &error_record,
 *         },
 *         &result);
 *
 * Names in both contexts (variables, namespace prefixes and URIs) are
 * identifiers of the processor's name pool (sxml_processor_get_name_pool);
 * types are identifiers of its type registry.
 *
 * The context configurations are not XPath-only: the XQuery and XSLT
 * headers of later phases reuse sxml_static_context_configuration and
 * sxml_dynamic_context_configuration and add what those languages need.
 *
 * Errors: static errors (XPST0003 for syntax, XPST0008, XPST0017,
 * XPST0081, ...), type errors and dynamic errors are reported through the
 * caller's sxml_error_record with their error code, description and
 * location in the expression text (D21); a computed description or error
 * value (fn:error) is kept by the item heap until the next error raised on
 * it (D41).
 *
 * Thread safety: the thread of the processor (D35).
 */

/* -------------------------------------------------------- Static context */

/* An external variable the expression may refer to, and its type. */
typedef struct sxml_variable_declaration {
    sxml_name_identifier variable_name;
    /* The value bound at evaluation is coerced to this type (XPath 4.0,
       section 3.4 "Coercion Rules"). item()* accepts any value:
       {.item_type = {.index = SXML_TYPE_ITEM},
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}. */
    sxml_sequence_type declared_type;
} sxml_variable_declaration;

typedef struct sxml_variable_declaration_view {
    const sxml_variable_declaration *variable_declarations;
    size_t variable_declaration_count;
} sxml_variable_declaration_view;

/* ------------------------------------------------------- Decimal formats */

/*
 * A property of a decimal format (XPath 4.0, section 2.2.1, "statically
 * known decimal formats"; F&O 4.0, "Defining a decimal format"). The list
 * only grows.
 */
typedef enum sxml_decimal_format_property_kind {
    /* Marker and rendition ("." or ".:R"); default ".". */
    SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR,
    /* Marker and rendition; default ",". */
    SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR,
    /* Marker and rendition; default "e". */
    SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR,
    /* Marker and rendition; default "%". */
    SXML_DECIMAL_FORMAT_PROPERTY_PERCENT,
    /* Marker and rendition; default U+2030. */
    SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE,
    /* A marker: a character of Unicode category Nd with digit value 0;
       default "0". */
    SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT,
    /* A marker; default "#". */
    SXML_DECIMAL_FORMAT_PROPERTY_DIGIT,
    /* A marker; default ";". */
    SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR,
    /* A rendition, any string; default "Infinity". */
    SXML_DECIMAL_FORMAT_PROPERTY_INFINITY,
    /* A rendition, any string; default "NaN". */
    SXML_DECIMAL_FORMAT_PROPERTY_NAN,
    /* A rendition, any string; default "-". */
    SXML_DECIMAL_FORMAT_PROPERTY_MINUS_SIGN,
} sxml_decimal_format_property_kind;

/*
 * One property of a decimal format, as text: the value of an attribute of
 * xsl:decimal-format or of a QT4 decimal-format environment, or of a
 * property in an XQuery decimal format declaration. A marker property is
 * one character; a property with marker and rendition is one character
 * (marker and rendition alike) or one character, ":" and a rendition of
 * any length, empty included (F&O 4.0, "fn:format-number", type
 * xs:string (: matching '.|.:.*' :)), so "%:pc" is the marker "%" with the
 * rendition "pc".
 */
typedef struct sxml_decimal_format_property {
    sxml_decimal_format_property_kind property_kind;
    /* UTF-8. Borrowed for the call; compilation copies it. */
    sxml_byte_view property_value;
} sxml_decimal_format_property;

typedef struct sxml_decimal_format_property_view {
    const sxml_decimal_format_property *decimal_format_properties;
    size_t decimal_format_property_count;
} sxml_decimal_format_property_view;

/*
 * A decimal format of the static context: the properties it sets; every
 * other property has the default listed above.
 */
typedef struct sxml_decimal_format {
    /* The format's name; index 0 for the unnamed decimal format, which
       fn:format-number uses when it names none. */
    sxml_name_identifier format_name;
    /* Each kind at most once. */
    sxml_decimal_format_property_view decimal_format_properties;
} sxml_decimal_format;

typedef struct sxml_decimal_format_view {
    const sxml_decimal_format *decimal_formats;
    size_t decimal_format_count;
} sxml_decimal_format_view;

/* ------------------------------------------------------------ Collations */

/*
 * A collation URI the host adds to the statically known collations,
 * standing for a collation sxml knows (XPath 4.0, section 2.2.1:
 * "statically known collations" is a mapping from URI to collation).
 */
typedef struct sxml_collation_binding {
    /* An absolute URI; must not be one sxml knows already. */
    sxml_byte_view collation_uri;
    /* An absolute URI for which sxml_collation_is_supported is true. */
    sxml_byte_view target_collation_uri;
} sxml_collation_binding;

typedef struct sxml_collation_binding_view {
    const sxml_collation_binding *collation_bindings;
    size_t collation_binding_count;
} sxml_collation_binding_view;

/*
 * Whether sxml implements the collation collation_uri names, an absolute
 * URI (D97): the Unicode codepoint collation
 * (http://www.w3.org/2005/xpath-functions/collation/codepoint), the HTML
 * ASCII case-insensitive collation
 * (http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive)
 * and UCA collation URIs (http://www.w3.org/2013/collation/UCA with
 * parameters) that sxml can serve with fallback; false for fallback=no
 * with parameters it cannot honour, and for any other URI. These and the
 * collation bindings of a static context are its statically known
 * collations. Compares bytes; a relative URI is false.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT bool sxml_collation_is_supported(sxml_byte_view collation_uri);

/* -------------------------------------------------------- Static context */

/*
 * What the host puts in the static context (XPath 4.0, section 2.2.1
 * "Static Context"). Every view is borrowed for the call that receives it;
 * what compilation keeps, it copies. A zero-initialized configuration is
 * the default static context: only the predeclared namespaces, no
 * variables, no static base URI, the Unicode codepoint collation as the
 * default collation, and only the unnamed decimal format, with the
 * default properties.
 *
 * The other components have fixed values: the default function namespace
 * is fn, the function signatures are those of the built-in library, and
 * XPath 1.0 compatibility mode is off.
 */
typedef struct sxml_static_context_configuration {
    /*
     * Statically known namespaces, added to the predeclared prefixes xml,
     * xs, xsi, fn, map, array, math and err (sxml_well_known_namespace);
     * a binding may rebind any of them except xml. A binding with prefix 0
     * sets the default namespace for elements and types (none by default).
     * Prefixes must be distinct; namespace URI 0 is allowed only with
     * prefix 0.
     */
    sxml_namespace_binding_view namespace_bindings;
    /* The static base URI, an absolute URI; empty when absent. fn:doc
       resolves relative URIs against it. */
    sxml_byte_view static_base_uri;
    /* The external variables in scope. Names must be distinct. */
    sxml_variable_declaration_view variable_declarations;
    /* The statically known decimal formats. Names must be distinct; a
       property value must have the form its kind takes, and the markers
       decimal-separator, grouping-separator, exponent-separator, percent,
       per-mille, digit and pattern-separator of one format must differ
       from each other and from the ten digits starting at zero-digit
       (F&O 4.0, "Defining a decimal format"). XQuery and XSLT check their
       own declarations first and raise their codes (XQST0097, XQST0098,
       XTSE1300); here a breach is an invalid configuration. */
    sxml_decimal_format_view decimal_formats;
    /* The default collation, an absolute URI among the statically known
       collations; empty means the Unicode codepoint collation. XPath 4.0
       places it in the dynamic context (section 2.2.2) but every host
       knows it statically; evaluations take it from here. */
    sxml_byte_view default_collation_uri;
    /* Collation URIs added to the statically known collations. URIs must
       be distinct. */
    sxml_collation_binding_view collation_bindings;
} sxml_static_context_configuration;

/* ------------------------------------------------------- Dynamic context */

/* The value of an external variable for one evaluation. */
typedef struct sxml_variable_binding {
    sxml_name_identifier variable_name;
    /* Borrowed for the call; the evaluation retains what it keeps. */
    sxml_sequence value;
} sxml_variable_binding;

typedef struct sxml_variable_binding_view {
    const sxml_variable_binding *variable_bindings;
    size_t variable_binding_count;
} sxml_variable_binding_view;

/*
 * A document fn:doc returns without asking the resolver (XPath 4.0,
 * section 2.2.2, "available documents"): fn:doc(uri) is document_node
 * itself, so it is the same node as a variable or context item bound to
 * it.
 */
typedef struct sxml_available_document {
    /* Compared byte for byte with fn:doc's argument resolved against the
       static base URI. */
    sxml_byte_view document_uri;
    /* A document node; borrowed for the call. */
    sxml_item document_node;
} sxml_available_document;

typedef struct sxml_available_document_view {
    const sxml_available_document *available_documents;
    size_t available_document_count;
} sxml_available_document_view;

/*
 * An environment variable of the dynamic context (XPath 4.0, section
 * 2.2.2, "environment variables"), for fn:environment-variable and
 * fn:available-environment-variables. sxml never reads the platform's
 * environment: a host that wants to expose it lists what it chooses here.
 */
typedef struct sxml_environment_variable {
    /* UTF-8; compared byte for byte. */
    sxml_byte_view variable_name;
    /* UTF-8. */
    sxml_byte_view variable_value;
} sxml_environment_variable;

typedef struct sxml_environment_variable_view {
    const sxml_environment_variable *environment_variables;
    size_t environment_variable_count;
} sxml_environment_variable_view;

/*
 * What the host puts in the dynamic context of one evaluation (XPath 4.0,
 * section 2.2.2 "Dynamic Context"). Views and values are borrowed for the
 * call. A zero-initialized configuration is the default: no context value
 * (the focus is absent), no variables, no available documents, the
 * processor's resolver, clock and trace sink, default language "en" and
 * no environment variables.
 */
typedef struct sxml_dynamic_context_configuration {
    /* The context value; when has_context_value is false the focus is
       absent and an expression that needs it raises XPDY0002. A single
       item (a document node, typically) is the context item, with
       position and size 1. */
    bool has_context_value;
    sxml_sequence context_value;
    /* One value for each declared external variable. A declared variable
       without a binding raises XPDY0002 when evaluated; a binding of a
       name not declared is ignored. Names must be distinct. */
    sxml_variable_binding_view variable_bindings;
    /* URIs must be distinct. */
    sxml_available_document_view available_documents;
    /* The null resolver means the processor's. */
    sxml_resource_resolver resource_resolver;
    /* The null clock means the processor's. */
    sxml_clock clock;
    /* The null sink means the processor's. */
    sxml_trace_sink trace_sink;
    /* The default language, an xs:language value such as "en" or "fr-CA",
       for fn:default-language and the formatting functions; empty means
       "en". */
    sxml_byte_view default_language;
    /* Names must be distinct. */
    sxml_environment_variable_view environment_variables;
} sxml_dynamic_context_configuration;

/* ------------------------------------------------------------ Expressions */

/*
 * An XPath expression compiled for one processor. Its fixed part lives in
 * the memory given to initialize (or allocated by create); its compiled
 * form is allocated from the processor's allocator.
 *
 * Function items that an evaluation returns may refer to the compiled form
 * (an inline function's body): the compiled form lives until the expression
 * is finalized and every such item is released, whichever comes last (D38).
 */
typedef struct sxml_xpath_expression sxml_xpath_expression;

/* An expression to compile. */
typedef struct sxml_xpath_expression_configuration {
    /* Kept by reference; must outlive the expression. */
    sxml_processor *processor;
    /* The expression, UTF-8. Borrowed for the call. */
    sxml_byte_view expression_text;
    /* The name to report in error locations; may be empty. Copied. */
    sxml_byte_view source_name;
    sxml_static_context_configuration static_context;
    /* Receives a static or type error found by compilation, or the
       location of a limit reached; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_xpath_expression_configuration;

/*
 * Writes the memory requirements of an expression's fixed part, which do
 * not depend on the expression text, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (no
 * processor).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xpath_expression_memory_requirements(
    const sxml_xpath_expression_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Compiles configuration->expression_text into an expression placed in
 * memory and writes its address to *expression. memory must stay valid,
 * unmoved, until sxml_xpath_expression_finalize. Compilation parses the
 * text, resolves names against the static context, checks types where
 * they are known statically, and keeps nothing of the text.
 *
 * Returns:
 * - SXML_STATUS_STATIC_ERROR or SXML_STATUS_TYPE_ERROR, with the error
 *   record filled (code, description, location);
 * - SXML_STATUS_LIMIT_EXCEEDED at maximum_expression_depth or another
 *   limit of the processor, with only the record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (a static
 *   context breaking the rules above, identifiers the processor did not
 *   issue, a decimal format property without the form its kind takes, a
 *   default collation or collation binding target that is not supported);
 * - SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 *   memory does not meet the memory requirements, and the allocator's
 *   failures.
 * On failure *expression is untouched and nothing is left allocated, except
 * item types the type registry interned, which stay valid.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xpath_expression_initialize(
    sxml_memory_view memory,
    const sxml_xpath_expression_configuration *configuration,
    sxml_xpath_expression **expression);

/*
 * Drops the expression's reference to its compiled form. Does nothing when
 * expression is NULL.
 */
SXML_EXPORT void sxml_xpath_expression_finalize(
    sxml_xpath_expression *expression);

/*
 * Allocates an expression through allocator and compiles it as
 * sxml_xpath_expression_initialize does. allocator is stored and must
 * outlive the expression. Returns the failures of
 * sxml_xpath_expression_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xpath_expression_create(
    sxml_allocator allocator,
    const sxml_xpath_expression_configuration *configuration,
    sxml_xpath_expression **expression);

/*
 * Finalizes an expression made by sxml_xpath_expression_create and
 * releases its memory. Does nothing when expression is NULL.
 */
SXML_EXPORT void sxml_xpath_expression_destroy(
    sxml_xpath_expression *expression);

/* One evaluation of an expression. */
typedef struct sxml_xpath_evaluation_request {
    sxml_dynamic_context_configuration dynamic_context;
    /* Receives a dynamic or type error, or the location of a limit
       reached; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_xpath_evaluation_request;

/*
 * Evaluates expression against request->dynamic_context and writes the
 * result, owned, to *result (release it with sxml_sequence_release before
 * the processor is finalized). The result is complete: reading it raises
 * no error and needs nothing of the request. Everything else the
 * evaluation made has been released when this returns, apart from
 * documents and function items the result refers to.
 *
 * An expression may be evaluated many times, and evaluations may nest (a
 * function body of the host that evaluates another expression), on the
 * processor's thread.
 *
 * Returns:
 * - SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR, with the error
 *   record filled (XPDY0002 for an absent focus or an unbound variable,
 *   XPTY0004 for a bound value that does not match its declared type,
 *   FODC0002 for a document that is not available, and every error of the
 *   language and the function library);
 * - SXML_STATUS_LIMIT_EXCEEDED at maximum_evaluation_depth or another
 *   limit of the processor, with only the record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid dynamic context (values or
 *   identifiers from another processor, duplicate names, a default
 *   language that is not an xs:language), and for a resource the resolver
 *   delivered in a form its kind does not allow;
 * - the allocator's, the resolver's, the clock's and the trace sink's
 *   failures.
 * On failure *result is untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xpath_expression_evaluate(
    const sxml_xpath_expression *expression,
    const sxml_xpath_evaluation_request *request, sxml_sequence *result);

/* ---------------------------------------------------------- Sequence types */

/* A SequenceType written as text, such as "xs:integer+" or "map(*)". */
typedef struct sxml_sequence_type_parse_request {
    sxml_processor *processor;
    /* XPath 4.0 SequenceType syntax (XPath 4.0, section 3.1), UTF-8.
       Borrowed for the call. */
    sxml_byte_view sequence_type_text;
    /* The name to report in error locations; may be empty. */
    sxml_byte_view source_name;
    /* Resolves prefixes; only its namespace bindings are used. */
    sxml_static_context_configuration static_context;
    /* Receives XPST0003 (syntax), XPST0081 (unknown prefix) or XPST0051
       (unknown type); may be NULL. */
    sxml_error_record *error_record;
} sxml_sequence_type_parse_request;

/*
 * Parses request->sequence_type_text and writes the sequence type to
 * *sequence_type, registering its item type in the processor's type
 * registry: for declared types of external variables that a host has as
 * text (the "as" attribute of a QT4 test parameter, an XSLT parameter).
 *
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled, and the
 * failures of sxml_type_registry_register_item_type; *sequence_type is
 * then untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xpath_parse_sequence_type(
    const sxml_sequence_type_parse_request *request,
    sxml_sequence_type *sequence_type);
