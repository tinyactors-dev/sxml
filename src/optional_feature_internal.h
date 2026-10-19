#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"

/* src/function_library_internal.h. */
struct sxml_native_function_view;

/*
 * Optional features inside sxml (include/sxml/processor.h, "Optional
 * features"; glossary `optional_feature`, `feature_data`; D183, D184,
 * D189). Internal to sxml; defined in src/optional_feature.c.
 *
 * A feature is one static const sxml_optional_feature_definition in the
 * feature's own source file, which also defines the exported function that
 * returns it and the feature's operations table. The core never names a
 * feature's functions: the place where the specification's behaviour
 * without the feature lives (its D162 place) asks the processor for the
 * feature by kind (sxml_processor_find_optional_feature) and calls the
 * operations it gets, or keeps that behaviour when it gets NULL. So a
 * program links a feature only when it names the feature's function.
 *
 * Rules for a feature:
 * - Its operations table is static const, of a type its own internal
 *   header declares; the core casts the table to that type by kind.
 * - It never calls another feature, nor back into the place that called it
 *   through its table (D71: no recursion, also not through function
 *   pointers).
 * - It keeps per-processor state, such as tables built from its feature
 *   data, only in its processor slot (sxml_processor_set_optional_feature_
 *   state); there is no global state.
 * - A feature that is in every program anyway is no feature: only parts
 *   that a host may want to leave out are.
 *
 * Adding a feature (packets 7.8 and later): a kind below, before COUNT;
 * the definition, operations and exported function in the feature's files;
 * one entry in sxml_all_optional_features (src/optional_feature.c); the
 * D162 place asks for the kind. A feature whose D162 place is "the
 * functions are absent" (XPST0017) gives its rows as the definition's
 * library_functions instead (D191): the processor's library serves them
 * only when the configuration names the feature.
 *
 * Thread safety: the processor's thread (D35).
 */

/* What a feature provides; one definition per kind. */
typedef enum sxml_optional_feature_kind {
    /* sxml_test_optional_feature: adds nothing to XPath, XQuery or XSLT;
       operations sxml_test_feature_operations. */
    SXML_OPTIONAL_FEATURE_KIND_TEST,
    /* sxml_expath_binary_feature: the bin: functions as library_functions
       (src/function_library_binary.c, packet 7.8, D198); operations is
       that view. */
    SXML_OPTIONAL_FEATURE_KIND_EXPATH_BINARY,
    /* sxml_expath_file_feature: the file: functions as library_functions
       over the processor's file system (src/function_library_file.c,
       packet 7.9, D200); operations is that view. Not in
       sxml_all_optional_features (D184). */
    SXML_OPTIONAL_FEATURE_KIND_EXPATH_FILE,
    /* sxml_html_parser_feature: fn:parse-html and fn:html-doc as
       library_functions (src/html.c, src/function_library_html.c, packet
       7.10a, D201); operations sxml_html_feature_operations
       (src/html_internal.h). */
    SXML_OPTIONAL_FEATURE_KIND_HTML,
    /* sxml_xslt_transform_feature: fn:transform as library_functions
       (src/xslt_transform_function.c, packet 7.11, D203); operations is
       that view. */
    SXML_OPTIONAL_FEATURE_KIND_XSLT_TRANSFORM,
    /* sxml_timezone_database_feature: zones and the zone table read
       through the resolver (src/timezone.c, packet 7.16, D209); operations
       sxml_timezone_feature_operations (src/timezone_internal.h). */
    SXML_OPTIONAL_FEATURE_KIND_TIMEZONE_DATABASE,
    /* sxml_dtd_validation_feature: the DTD validator for the
       dtd-validation option of fn:doc and fn:parse-xml (src/schema_from_
       dtd.c, packet 7.2b, D211); operations sxml_dtd_validation_feature_
       operations (src/dtd_internal.h). */
    SXML_OPTIONAL_FEATURE_KIND_DTD_VALIDATION,
    /* sxml_uca_collation_feature: UCA collations over the DUCET the
       resolver serves (src/collation_uca.c, packet 7.15a, D213);
       operations sxml_uca_collation_operations
       (src/collation_internal.h). */
    SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION,
    /* sxml_full_text_feature: the matcher of XQuery and XPath Full Text
       3.0 for the contains row of src/xquery_lower_full_text.c
       (src/full_text.c, packet 7.12a, D214); operations
       sxml_full_text_operations (src/full_text_internal.h). */
    SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT,
    SXML_OPTIONAL_FEATURE_KIND_COUNT,
} sxml_optional_feature_kind;

struct sxml_optional_feature_definition {
    sxml_optional_feature_kind feature_kind;
    /* ASCII, static: what sxml_optional_feature_get_name returns. */
    sxml_byte_view feature_name;
    /* The feature's operations table, static const, of the type its kind
       names above; never NULL. */
    const void *operations;
    /* Releases the state the feature set in a processor, with the
       processor's allocator, when the processor is finalized. NULL when
       the feature keeps no state. It must not use the processor. */
    void (*finalize_state)(void *state, sxml_allocator allocator);
    /* The library functions the feature adds (src/function_library_
       internal.h), static const rows in the feature's own files, or NULL
       for none. A processor that names the feature serves them after the
       parts of src/function_library_*.c, so without the feature the
       functions are absent (XPST0017, their D162 place) and their rows
       are not linked (D191). */
    const struct sxml_native_function_view *library_functions;
};

/* A processor's record of one kind (src/processor_internal.h). */
typedef struct sxml_optional_feature_slot {
    /* NULL when the configuration did not name the feature. */
    const sxml_optional_feature_definition *definition;
    /* The feature's state in this processor; NULL until it sets one. */
    void *state;
} sxml_optional_feature_slot;

/* The slots of one processor, one per kind. */
typedef struct sxml_optional_feature_slots {
    sxml_optional_feature_slot slots[SXML_OPTIONAL_FEATURE_KIND_COUNT];
} sxml_optional_feature_slots;

/* True for a view the configuration contract allows
   (sxml_processor_configuration.optional_features). */
bool sxml_optional_feature_view_is_valid(sxml_optional_feature_view view);

/* The slots for a valid view: each named feature by its kind, no state. */
sxml_optional_feature_slots sxml_optional_feature_slots_from_view(
    sxml_optional_feature_view view);

/* Finalizes the state of every slot that has one, with allocator, and
   clears it. */
void sxml_optional_feature_slots_finalize(sxml_optional_feature_slots *slots,
                                          sxml_allocator allocator);

/*
 * The operations of the feature of kind when the configuration of
 * processor named it, else NULL: the question a feature's D162 place
 * asks. The caller casts the table to the type the kind names. NULL also
 * for a NULL processor (a unit test's evaluation without one).
 */
const void *sxml_processor_find_optional_feature(const sxml_processor *processor,
                                                 sxml_optional_feature_kind feature_kind);

/* The state the feature of kind set in processor, or NULL. */
void *sxml_processor_get_optional_feature_state(const sxml_processor *processor,
                                                sxml_optional_feature_kind feature_kind);

/*
 * Gives the configured feature of kind its state in processor, which the
 * processor finalizes through the definition's finalize_state. Returns
 * SXML_STATUS_INVALID_ARGUMENT when the feature is not configured, has no
 * finalize_state, or already has a state; the caller then still owns
 * state.
 */
SXML_NODISCARD sxml_status sxml_processor_set_optional_feature_state(
    sxml_processor *processor, sxml_optional_feature_kind feature_kind, void *state);

/* ---------------------------------------------------------- Feature data */

/* Receives the bytes of feature data while the resolver lends them. */
typedef struct sxml_feature_data_reader_operations {
    /* Reads content, borrowed for the call: parses it into the feature's
       state, for example. A failure is returned by
       sxml_optional_feature_read_data. */
    sxml_status (*read)(void *state, sxml_byte_view content);
} sxml_feature_data_reader_operations;

typedef struct sxml_feature_data_reader {
    void *state;
    const sxml_feature_data_reader_operations *operations;
} sxml_feature_data_reader;

/* The feature data to read (glossary `feature_data`). Views are borrowed
   for the call. */
typedef struct sxml_feature_data_request {
    sxml_processor *processor;
    /* A kind whose forms include BYTES (include/sxml/processor.h); each
       feature with data adds its own kind there (D184). */
    sxml_resource_kind resource_kind;
    /* An absolute URI that names the data, for example
       "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt". */
    sxml_byte_view uri;
    sxml_feature_data_reader reader;
} sxml_feature_data_request;

/* What reading feature data gave. */
typedef struct sxml_feature_data_outcome {
    /* Whether the resolver delivered the data and the reader read it. */
    bool is_available;
} sxml_feature_data_outcome;

/*
 * Asks the processor's resource resolver for request->uri as
 * request->resource_kind, hands the bytes to request->reader and gives them
 * back to the resolver. When the resolver does not deliver the data, the
 * reader is not called, outcome->is_available is false and the feature
 * behaves as its specification says for missing data. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid request or a form the kind
 * does not allow, and the failures of the resolver, the allocator and the
 * reader; outcome->is_available is then false.
 */
SXML_NODISCARD sxml_status sxml_optional_feature_read_data(
    const sxml_feature_data_request *request, sxml_feature_data_outcome *outcome);

/* ------------------------------------------------------ The test feature */

/* The operations of sxml_test_optional_feature: enough to test the
   mechanism and the feature data path (tests/optional_feature_test.c). */
typedef struct sxml_test_feature_operations {
    /*
     * Writes to *byte_count the byte count of the feature data at
     * "urn:sxml:test-feature-data" (a TEXT resource of the processor's
     * resolver), reading it at the first call and keeping the count as
     * the feature's state in processor. Writes 0 when the resolver has no
     * such data, and then reads again at the next call. Returns the
     * failures of sxml_optional_feature_read_data and the allocator.
     */
    sxml_status (*read_data_byte_count)(sxml_processor *processor, size_t *byte_count);
} sxml_test_feature_operations;

/* ---------------------------------------------- Definitions of features */

/* The EXPath Binary feature (src/function_library_binary.c), for
   sxml_all_optional_features. */
extern const sxml_optional_feature_definition sxml_expath_binary_feature_definition;

/* The UCA collation feature (src/collation_uca.c). */
extern const sxml_optional_feature_definition sxml_uca_collation_feature_definition;

/* The HTML parser feature (src/html.c), for sxml_all_optional_features. */
extern const sxml_optional_feature_definition sxml_html_feature_definition;
/* src/xslt_transform_function.c. */
extern const sxml_optional_feature_definition sxml_xslt_transform_feature_definition;
/* The timezone database (src/timezone.c). */
extern const sxml_optional_feature_definition sxml_timezone_database_feature_definition;
/* The DTD validation feature (src/schema_from_dtd.c). */
extern const sxml_optional_feature_definition sxml_dtd_validation_feature_definition;
/* The Full Text feature (src/full_text.c). */
extern const sxml_optional_feature_definition sxml_full_text_feature_definition;
