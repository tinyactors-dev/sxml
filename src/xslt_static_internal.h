#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * Static expressions of XSLT 4.0 (section 9.7): the [xsl:]use-when
 * attributes (3.11.3), the select attributes of static variables and
 * parameters (9.6) and shadow attributes (3.11.4), evaluated while the
 * stylesheet is read (src/xslt_read.c). Module xslt_static, defined in
 * src/xslt_static.c (packet 4.5, D130). Internal to sxml.
 *
 * A static scope holds what 9.7's tables give every static expression of
 * one stylesheet: the static variables declared so far, in stylesheet tree
 * order (a later declaration of a name hides an earlier one, 9.6), and the
 * function definitions: the processor's library (F&O 4.0, constructor
 * functions) with XSLT's element-available, function-available,
 * type-available, available-system-properties and system-property in
 * front. Everything else of the static context comes with each
 * expression: its namespaces, default element namespace and static base
 * URI. The context item is absent, the default collation the codepoint
 * collation, the decimal format the unnamed default one.
 *
 * Expressions are compiled through the XPath front end and lowering
 * (sxml_xpath_compile, src/xpath_static_internal.h) and run by the one
 * evaluator: no machinery of its own (D46).
 *
 * Thread safety: the thread of the processor (D35).
 */

/* What the XSLT functions of static expressions know about the
   stylesheet reader: whether an XSLT element or extension instruction is
   one sxml implements (fn:element-available, XSLT 4.0, 24.3.2). */
typedef struct sxml_xslt_element_query {
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
} sxml_xslt_element_query;

/*
 * True when the query names an element of the XSLT namespace that sxml
 * reads and implements, or the extension instruction exsl:document (D206).
 * Defined in src/xslt_read.c over its element table.
 */
bool sxml_xslt_is_element_available(sxml_xslt_element_query query);

typedef struct sxml_xslt_static_scope sxml_xslt_static_scope;

typedef struct sxml_xslt_static_scope_configuration {
    /* Kept by reference; must outlive the scope. Values are items of its
       heap; expressions are compiled with its limits. */
    sxml_processor *processor;
    /* The resolver static expressions read documents through (fn:doc in a
       use-when); the null resolver means the processor's. */
    sxml_resource_resolver resource_resolver;
} sxml_xslt_static_scope_configuration;

/* Writes the requirements of a scope to *requirements. Returns
   SXML_STATUS_INVALID_ARGUMENT without a processor. */
SXML_NODISCARD sxml_status sxml_xslt_static_scope_memory_requirements(
    const sxml_xslt_static_scope_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places an empty scope in memory, which must stay valid, unmoved, until
   sxml_xslt_static_scope_finalize. Allocates the function table of the
   XSLT functions from the processor's allocator. */
SXML_NODISCARD sxml_status sxml_xslt_static_scope_initialize(
    sxml_memory_view memory, const sxml_xslt_static_scope_configuration *configuration,
    sxml_xslt_static_scope **scope);

/* Releases the variables' values and the scope's memory; accepts NULL. */
void sxml_xslt_static_scope_finalize(sxml_xslt_static_scope *scope);

SXML_NODISCARD sxml_status sxml_xslt_static_scope_create(
    sxml_allocator allocator, const sxml_xslt_static_scope_configuration *configuration,
    sxml_xslt_static_scope **scope);

void sxml_xslt_static_scope_destroy(sxml_xslt_static_scope *scope);

/* A static variable or parameter whose value is known. */
typedef struct sxml_xslt_static_variable {
    sxml_name_identifier variable_name;
    /* Borrowed; the scope retains it. */
    sxml_sequence value;
} sxml_xslt_static_variable;

/*
 * Brings variable into scope for every expression evaluated after it,
 * hiding an earlier one of the same name. Returns the allocator's
 * failures; the scope is then unchanged.
 */
SXML_NODISCARD sxml_status sxml_xslt_static_scope_add_variable(
    sxml_xslt_static_scope *scope, const sxml_xslt_static_variable *variable);

/* The value of the static variable variable_name in scope, borrowed until
   the scope changes; is_found false when there is none. */
typedef struct sxml_xslt_static_lookup {
    bool is_found;
    sxml_sequence value;
} sxml_xslt_static_lookup;

sxml_xslt_static_lookup sxml_xslt_static_scope_find_variable(
    const sxml_xslt_static_scope *scope, sxml_name_identifier variable_name);

/* One static expression. Views are borrowed for the call. */
typedef struct sxml_xslt_static_expression {
    /* An XPath 4.0 expression, or the text of a value template when
       is_value_template (a shadow attribute: the result is its string
       value as one xs:string, XSLT 4.0, 5.6). */
    sxml_byte_view expression_text;
    bool is_value_template;
    /* The statically known namespaces of the containing element, without
       the predeclared ones; a binding with prefix 0 is the default
       namespace for elements and types (the element's
       [xsl:]xpath-default-namespace). */
    sxml_namespace_binding_view namespace_bindings;
    /* The base URI of the containing element; empty when absent. */
    sxml_byte_view static_base_uri;
    /* The module URI, the source name of error locations. */
    sxml_byte_view source_name;
    /* Receives a static error of the expression, or a dynamic error raised
       by its evaluation, which XSLT treats as a static error keeping its
       code (9.7); locations are offsets into expression_text. May be NULL. */
    sxml_error_record *error_record;
} sxml_xslt_static_expression;

/*
 * Compiles and evaluates expression with the variables in scope and writes
 * its value, owned, to *value. Returns SXML_STATUS_STATIC_ERROR,
 * SXML_STATUS_TYPE_ERROR or SXML_STATUS_DYNAMIC_ERROR with the error record
 * filled, SXML_STATUS_LIMIT_EXCEEDED at a limit of the processor, and the
 * allocator's and resolver's failures; *value is then untouched.
 */
SXML_NODISCARD sxml_status sxml_xslt_static_scope_evaluate(
    sxml_xslt_static_scope *scope, const sxml_xslt_static_expression *expression,
    sxml_sequence *value);

/* A value to coerce to the declared type of a static variable. */
typedef struct sxml_xslt_static_coercion {
    /* Borrowed. */
    sxml_sequence value;
    sxml_sequence_type declared_type;
    /* The code raised when the value does not match (XTTE0570 for a
       variable, XTTE0590 for a supplied parameter), static text. */
    sxml_byte_view error_code;
    sxml_error_record *error_record;
} sxml_xslt_static_coercion;

/*
 * Applies the coercion rules (XPath 4.0, section 3.4) to coercion->value
 * and writes the result, owned, to *value. A value that does not match is
 * SXML_STATUS_TYPE_ERROR with coercion->error_code; other failures as
 * sxml_xslt_static_scope_evaluate.
 */
SXML_NODISCARD sxml_status sxml_xslt_static_scope_coerce(
    sxml_xslt_static_scope *scope, const sxml_xslt_static_coercion *coercion,
    sxml_sequence *value);
