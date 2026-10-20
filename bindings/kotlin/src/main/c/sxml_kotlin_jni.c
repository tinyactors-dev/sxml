/*
 * The JNI glue (docs/GLOSSARY.md, "jni_glue"; D238): the only C of the
 * Kotlin binding. Each entry point turns one native request (the Kotlin
 * objects of src/main/kotlin/dev/tinyactors/sxml/Native.kt) into one host
 * shim call (bindings/host/sxml_host.h) and writes what the call made back
 * into the same object: the handle, the error, the output. Nothing else is
 * here: values, side results and errors are the host shim's, so a fix
 * there fixes the JavaScript, Kotlin and Swift bindings at once.
 *
 * Golden files: bindings/host/sxml_host.c for the C, and
 * bindings/javascript/src/index.ts for what a binding does around it.
 *
 * JNI rules (D238):
 * - every entry point is (JNIEnv *, jclass, jobject request): three
 *   parameters;
 * - the field and method IDs are looked up when a processor is made and
 *   kept in its glue state (sxml_kotlin_processor), not in globals; an
 *   entry point finds that state through the request's processor field;
 * - text crosses as byte arrays of UTF-8 (never jstring: JNI's modified
 *   UTF-8 encodes U+0000 and supplementary characters differently), copied
 *   into memory from the processor's allocator (sxml_host_allocate), so
 *   the processor's allocation counts see it (D23);
 * - the resolver is the Kotlin method ProcessorState.resolve, which keeps
 *   what the host's resolver throws and returns a failure status, so the
 *   glue never handles an exception. When the JVM itself leaves one
 *   pending (a byte array it cannot allocate), the glue makes no further
 *   JNI call but those the JNI specification allows then, releases what
 *   it made in C, and returns: the exception reaches Kotlin.
 *
 * Threads: the binding holds the processor's lock around every entry point
 * (D35), so one thread at a time uses a glue state; the environment of the
 * call in progress is kept in it for the resolver, which runs on the same
 * thread inside the call.
 *
 * Only the JNI entry points are exported (sxml_kotlin.map): their names
 * are what JNI looks up, so they are not named sxml_*.
 */
#include "sxml_host.h"

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xslt.h"

#include <jni.h>

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* What a release request releases (Kotlin HandleKind). */
typedef enum sxml_kotlin_handle_kind {
    SXML_KOTLIN_HANDLE_KIND_VALUE = 0,
    SXML_KOTLIN_HANDLE_KIND_EXPRESSION = 1,
    SXML_KOTLIN_HANDLE_KIND_QUERY = 2,
    SXML_KOTLIN_HANDLE_KIND_STYLESHEET = 3,
    SXML_KOTLIN_HANDLE_KIND_SCHEMA = 4,
} sxml_kotlin_handle_kind;

/* ------------------------------------------------------------ Field table */

/* NativeRequest: what every request has. */
typedef struct sxml_kotlin_request_fields {
    jfieldID processor;
    jfieldID handle;
    jfieldID error;
} sxml_kotlin_request_fields;

/* NativeError: sxml_host_error. */
typedef struct sxml_kotlin_error_fields {
    jfieldID status;
    jfieldID code_namespace_uri;
    jfieldID code_local_name;
    jfieldID code_namespace_prefix;
    jfieldID description;
    jfieldID source_name;
    jfieldID line_number;
    jfieldID column_number;
} sxml_kotlin_error_fields;

/* ProcessorRequest. */
typedef struct sxml_kotlin_processor_fields {
    jfieldID failing_allocation_number;
    jfieldID feature_mask;
    jfieldID state;
    jfieldID allocation_count;
} sxml_kotlin_processor_fields;

/* TextRequest. */
typedef struct sxml_kotlin_text_fields {
    jfieldID text;
    jfieldID uri;
} sxml_kotlin_text_fields;

/* CompileRequest (XPath and XQuery). */
typedef struct sxml_kotlin_compile_fields {
    jfieldID expression_text;
    jfieldID base_uri;
    jfieldID namespaces;
    jfieldID variables;
    jfieldID is_updating;
} sxml_kotlin_compile_fields;

/* EvaluateRequest. */
typedef struct sxml_kotlin_evaluate_fields {
    jfieldID expression;
    jfieldID context;
    jfieldID variable_values;
} sxml_kotlin_evaluate_fields;

/* SerializeRequest. */
typedef struct sxml_kotlin_serialize_fields {
    jfieldID value;
    jfieldID options;
    jfieldID output;
} sxml_kotlin_serialize_fields;

/* ReleaseRequest. */
typedef struct sxml_kotlin_release_fields {
    jfieldID kind;
    jfieldID target;
} sxml_kotlin_release_fields;

/* SideResultRequest's sideResults, and NativeSideResults: the side results
   of a call, flat (sxml_host_side_result_list). */
typedef struct sxml_kotlin_side_result_fields {
    jfieldID side_results;
    jfieldID kinds;
    jfieldID texts;
    jfieldID values;
    jfieldID numbers;
} sxml_kotlin_side_result_fields;

/* NativeBindings: names (namespace URI and local name, in pairs) and one
   value handle per name (sxml_host_binding_list). */
typedef struct sxml_kotlin_binding_fields {
    jfieldID names;
    jfieldID values;
} sxml_kotlin_binding_fields;

/* QueryEvaluateRequest. */
typedef struct sxml_kotlin_query_evaluate_fields {
    jfieldID query;
    jfieldID context;
    jfieldID variables;
    jfieldID accepts_put_documents;
} sxml_kotlin_query_evaluate_fields;

/* QuerySerializeRequest (value and output are SerializeRequest's). */
typedef struct sxml_kotlin_query_serialize_fields {
    jfieldID query;
} sxml_kotlin_query_serialize_fields;

/* StylesheetCompileRequest. */
typedef struct sxml_kotlin_stylesheet_compile_fields {
    jfieldID stylesheet_text;
    jfieldID base_uri;
    jfieldID static_parameters;
} sxml_kotlin_stylesheet_compile_fields;

/* TransformRequest. */
typedef struct sxml_kotlin_transform_fields {
    jfieldID stylesheet;
    jfieldID global_context_item;
    jfieldID initial_match_selection;
    jfieldID stylesheet_parameters;
    jfieldID template_parameters;
    jfieldID tunnel_parameters;
    jfieldID function_arguments;
    jfieldID base_output_uri;
    jfieldID serialization_options;
    jfieldID names;
    jfieldID invocation_kind;
    jfieldID result_delivery;
    jfieldID result_document_delivery;
    jfieldID selects_unnamed_mode;
    jfieldID output;
} sxml_kotlin_transform_fields;

/* SchemaCompileRequest. */
typedef struct sxml_kotlin_schema_compile_fields {
    jfieldID documents;
    jfieldID base_uri;
} sxml_kotlin_schema_compile_fields;

/* ValidateRequest. */
typedef struct sxml_kotlin_validate_fields {
    jfieldID schema;
    jfieldID node;
    jfieldID text;
    jfieldID document_uri;
    jfieldID validation_mode;
    jfieldID governing_type;
    jfieldID validity;
} sxml_kotlin_validate_fields;

/* ProcessorState: the resolver bridge. */
typedef struct sxml_kotlin_state_members {
    jmethodID resolve;
    jfieldID resolver_answer;
} sxml_kotlin_state_members;

/* Every field and method the glue uses, looked up once per processor. */
typedef struct sxml_kotlin_field_table {
    sxml_kotlin_request_fields request;
    sxml_kotlin_error_fields error;
    sxml_kotlin_processor_fields processor;
    sxml_kotlin_text_fields text;
    sxml_kotlin_compile_fields compile;
    sxml_kotlin_evaluate_fields evaluate;
    sxml_kotlin_serialize_fields serialize;
    sxml_kotlin_release_fields release;
    sxml_kotlin_side_result_fields side_results;
    sxml_kotlin_binding_fields bindings;
    sxml_kotlin_query_evaluate_fields query_evaluate;
    sxml_kotlin_query_serialize_fields query_serialize;
    sxml_kotlin_stylesheet_compile_fields stylesheet_compile;
    sxml_kotlin_transform_fields transform;
    sxml_kotlin_schema_compile_fields schema_compile;
    sxml_kotlin_validate_fields validate;
    sxml_kotlin_state_members state;
} sxml_kotlin_field_table;

/* One field to look up: its class, name and JNI type signature. */
typedef struct sxml_kotlin_field_name {
    const char *class_name;
    const char *field_name;
    const char *signature;
    jfieldID *field;
} sxml_kotlin_field_name;

/* ------------------------------------------------------------ Glue state */

/* A processor as the glue holds it: the host shim's processor, the Kotlin
   ProcessorState it calls back, and the table. From the default
   allocator, because the resolver's state exists before the processor. */
typedef struct sxml_kotlin_processor {
    sxml_host_processor *processor;
    /* A global reference to the ProcessorState. */
    jobject state;
    /* The environment of the call in progress, for the resolver. */
    JNIEnv *environment;
    sxml_kotlin_field_table table;
} sxml_kotlin_processor;

/* One entry point's call: its environment, glue state and request. */
typedef struct sxml_kotlin_call {
    JNIEnv *environment;
    sxml_kotlin_processor *glue;
    jobject request;
} sxml_kotlin_call;

#define SXML_KOTLIN_CLASS(name) "dev/tinyactors/sxml/" name
#define SXML_KOTLIN_BYTES "[B"
#define SXML_KOTLIN_TEXTS "[[B"
#define SXML_KOTLIN_BINDINGS "L" SXML_KOTLIN_CLASS("NativeBindings;")

/* Looks up every field of table and the resolver method; false with an
   exception pending (NoSuchFieldError, NoClassDefFoundError) when one is
   missing. */
static bool sxml_kotlin_look_up(JNIEnv *environment, sxml_kotlin_field_table *table) {
    const sxml_kotlin_field_name names[] = {
        {SXML_KOTLIN_CLASS("NativeRequest"), "processor", "J", &table->request.processor},
        {SXML_KOTLIN_CLASS("NativeRequest"), "handle", "J", &table->request.handle},
        {SXML_KOTLIN_CLASS("NativeRequest"), "error", "L" SXML_KOTLIN_CLASS("NativeError;"),
         &table->request.error},
        {SXML_KOTLIN_CLASS("NativeError"), "status", "I", &table->error.status},
        {SXML_KOTLIN_CLASS("NativeError"), "codeNamespaceUri", SXML_KOTLIN_BYTES,
         &table->error.code_namespace_uri},
        {SXML_KOTLIN_CLASS("NativeError"), "codeLocalName", SXML_KOTLIN_BYTES,
         &table->error.code_local_name},
        {SXML_KOTLIN_CLASS("NativeError"), "codeNamespacePrefix", SXML_KOTLIN_BYTES,
         &table->error.code_namespace_prefix},
        {SXML_KOTLIN_CLASS("NativeError"), "description", SXML_KOTLIN_BYTES,
         &table->error.description},
        {SXML_KOTLIN_CLASS("NativeError"), "sourceName", SXML_KOTLIN_BYTES,
         &table->error.source_name},
        {SXML_KOTLIN_CLASS("NativeError"), "lineNumber", "I", &table->error.line_number},
        {SXML_KOTLIN_CLASS("NativeError"), "columnNumber", "I", &table->error.column_number},
        {SXML_KOTLIN_CLASS("ProcessorRequest"), "failingAllocationNumber", "I",
         &table->processor.failing_allocation_number},
        {SXML_KOTLIN_CLASS("ProcessorRequest"), "featureMask", "I",
         &table->processor.feature_mask},
        {SXML_KOTLIN_CLASS("ProcessorRequest"), "state", "L" SXML_KOTLIN_CLASS("ProcessorState;"),
         &table->processor.state},
        {SXML_KOTLIN_CLASS("ProcessorRequest"), "allocationCount", "I",
         &table->processor.allocation_count},
        {SXML_KOTLIN_CLASS("TextRequest"), "text", SXML_KOTLIN_BYTES, &table->text.text},
        {SXML_KOTLIN_CLASS("TextRequest"), "uri", SXML_KOTLIN_BYTES, &table->text.uri},
        {SXML_KOTLIN_CLASS("CompileRequest"), "expressionText", SXML_KOTLIN_BYTES,
         &table->compile.expression_text},
        {SXML_KOTLIN_CLASS("CompileRequest"), "baseUri", SXML_KOTLIN_BYTES,
         &table->compile.base_uri},
        {SXML_KOTLIN_CLASS("CompileRequest"), "namespaces", "[" SXML_KOTLIN_BYTES,
         &table->compile.namespaces},
        {SXML_KOTLIN_CLASS("CompileRequest"), "variables", "[" SXML_KOTLIN_BYTES,
         &table->compile.variables},
        {SXML_KOTLIN_CLASS("CompileRequest"), "isUpdating", "Z", &table->compile.is_updating},
        {SXML_KOTLIN_CLASS("EvaluateRequest"), "expression", "J", &table->evaluate.expression},
        {SXML_KOTLIN_CLASS("EvaluateRequest"), "context", "J", &table->evaluate.context},
        {SXML_KOTLIN_CLASS("EvaluateRequest"), "variableValues", "[J",
         &table->evaluate.variable_values},
        {SXML_KOTLIN_CLASS("SerializeRequest"), "value", "J", &table->serialize.value},
        {SXML_KOTLIN_CLASS("SerializeRequest"), "options", "J", &table->serialize.options},
        {SXML_KOTLIN_CLASS("SerializeRequest"), "output", SXML_KOTLIN_BYTES,
         &table->serialize.output},
        {SXML_KOTLIN_CLASS("ReleaseRequest"), "kind", "I", &table->release.kind},
        {SXML_KOTLIN_CLASS("ReleaseRequest"), "target", "J", &table->release.target},
        {SXML_KOTLIN_CLASS("SideResultRequest"), "sideResults",
         "L" SXML_KOTLIN_CLASS("NativeSideResults;"), &table->side_results.side_results},
        {SXML_KOTLIN_CLASS("NativeSideResults"), "kinds", "[I", &table->side_results.kinds},
        {SXML_KOTLIN_CLASS("NativeSideResults"), "texts", SXML_KOTLIN_TEXTS,
         &table->side_results.texts},
        {SXML_KOTLIN_CLASS("NativeSideResults"), "values", "[J", &table->side_results.values},
        {SXML_KOTLIN_CLASS("NativeSideResults"), "numbers", "[I", &table->side_results.numbers},
        {SXML_KOTLIN_CLASS("NativeBindings"), "names", SXML_KOTLIN_TEXTS, &table->bindings.names},
        {SXML_KOTLIN_CLASS("NativeBindings"), "values", "[J", &table->bindings.values},
        {SXML_KOTLIN_CLASS("QueryEvaluateRequest"), "query", "J", &table->query_evaluate.query},
        {SXML_KOTLIN_CLASS("QueryEvaluateRequest"), "context", "J",
         &table->query_evaluate.context},
        {SXML_KOTLIN_CLASS("QueryEvaluateRequest"), "variables", SXML_KOTLIN_BINDINGS,
         &table->query_evaluate.variables},
        {SXML_KOTLIN_CLASS("QueryEvaluateRequest"), "acceptsPutDocuments", "Z",
         &table->query_evaluate.accepts_put_documents},
        {SXML_KOTLIN_CLASS("QuerySerializeRequest"), "query", "J", &table->query_serialize.query},
        {SXML_KOTLIN_CLASS("StylesheetCompileRequest"), "stylesheetText", SXML_KOTLIN_BYTES,
         &table->stylesheet_compile.stylesheet_text},
        {SXML_KOTLIN_CLASS("StylesheetCompileRequest"), "baseUri", SXML_KOTLIN_BYTES,
         &table->stylesheet_compile.base_uri},
        {SXML_KOTLIN_CLASS("StylesheetCompileRequest"), "staticParameters", SXML_KOTLIN_BINDINGS,
         &table->stylesheet_compile.static_parameters},
        {SXML_KOTLIN_CLASS("TransformRequest"), "stylesheet", "J", &table->transform.stylesheet},
        {SXML_KOTLIN_CLASS("TransformRequest"), "globalContextItem", "J",
         &table->transform.global_context_item},
        {SXML_KOTLIN_CLASS("TransformRequest"), "initialMatchSelection", "J",
         &table->transform.initial_match_selection},
        {SXML_KOTLIN_CLASS("TransformRequest"), "stylesheetParameters", SXML_KOTLIN_BINDINGS,
         &table->transform.stylesheet_parameters},
        {SXML_KOTLIN_CLASS("TransformRequest"), "templateParameters", SXML_KOTLIN_BINDINGS,
         &table->transform.template_parameters},
        {SXML_KOTLIN_CLASS("TransformRequest"), "tunnelParameters", SXML_KOTLIN_BINDINGS,
         &table->transform.tunnel_parameters},
        {SXML_KOTLIN_CLASS("TransformRequest"), "functionArguments", "[J",
         &table->transform.function_arguments},
        {SXML_KOTLIN_CLASS("TransformRequest"), "baseOutputUri", SXML_KOTLIN_BYTES,
         &table->transform.base_output_uri},
        {SXML_KOTLIN_CLASS("TransformRequest"), "serializationOptions", "J",
         &table->transform.serialization_options},
        {SXML_KOTLIN_CLASS("TransformRequest"), "names", SXML_KOTLIN_TEXTS,
         &table->transform.names},
        {SXML_KOTLIN_CLASS("TransformRequest"), "invocationKind", "I",
         &table->transform.invocation_kind},
        {SXML_KOTLIN_CLASS("TransformRequest"), "resultDelivery", "I",
         &table->transform.result_delivery},
        {SXML_KOTLIN_CLASS("TransformRequest"), "resultDocumentDelivery", "I",
         &table->transform.result_document_delivery},
        {SXML_KOTLIN_CLASS("TransformRequest"), "selectsUnnamedMode", "Z",
         &table->transform.selects_unnamed_mode},
        {SXML_KOTLIN_CLASS("TransformRequest"), "output", SXML_KOTLIN_BYTES,
         &table->transform.output},
        {SXML_KOTLIN_CLASS("SchemaCompileRequest"), "documents", SXML_KOTLIN_TEXTS,
         &table->schema_compile.documents},
        {SXML_KOTLIN_CLASS("SchemaCompileRequest"), "baseUri", SXML_KOTLIN_BYTES,
         &table->schema_compile.base_uri},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "schema", "J", &table->validate.schema},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "node", "J", &table->validate.node},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "text", SXML_KOTLIN_BYTES, &table->validate.text},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "documentUri", SXML_KOTLIN_BYTES,
         &table->validate.document_uri},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "validationMode", "I",
         &table->validate.validation_mode},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "governingType", SXML_KOTLIN_TEXTS,
         &table->validate.governing_type},
        {SXML_KOTLIN_CLASS("ValidateRequest"), "validity", "I", &table->validate.validity},
        {SXML_KOTLIN_CLASS("ProcessorState"), "resolverAnswer", SXML_KOTLIN_BYTES,
         &table->state.resolver_answer},
    };
    for (size_t name_index = 0; name_index < sizeof(names) / sizeof(names[0]); name_index += 1) {
        const sxml_kotlin_field_name *name = &names[name_index];
        jclass field_class = (*environment)->FindClass(environment, name->class_name);
        if (field_class == NULL) {
            return false;
        }
        *name->field =
            (*environment)->GetFieldID(environment, field_class, name->field_name, name->signature);
        (*environment)->DeleteLocalRef(environment, field_class);
        if (*name->field == NULL) {
            return false;
        }
    }
    jclass state_class = (*environment)->FindClass(environment, SXML_KOTLIN_CLASS("ProcessorState"));
    if (state_class == NULL) {
        return false;
    }
    table->state.resolve =
        (*environment)->GetMethodID(environment, state_class, "resolve", "(I[B[B)I");
    (*environment)->DeleteLocalRef(environment, state_class);
    return table->state.resolve != NULL;
}

/* The glue state the request names, with environment kept for the
   resolver; NULL with an exception pending when the request has no
   processor field. */
static sxml_kotlin_processor *sxml_kotlin_glue_of(JNIEnv *environment, jobject request) {
    jclass request_class = (*environment)->GetObjectClass(environment, request);
    const jfieldID field = (*environment)->GetFieldID(environment, request_class, "processor", "J");
    (*environment)->DeleteLocalRef(environment, request_class);
    if (field == NULL) {
        return NULL;
    }
    sxml_kotlin_processor *glue =
        (sxml_kotlin_processor *)(intptr_t)(*environment)->GetLongField(environment, request, field);
    if (glue != NULL) {
        glue->environment = environment;
    }
    return glue;
}

/* -------------------------------------------------------------- Helpers */

static jlong sxml_kotlin_handle_of(const void *handle) {
    return (jlong)(intptr_t)handle;
}

/* The handle a long field of an object holds (0: NULL). */
static void *sxml_kotlin_pointer_of(JNIEnv *environment, jobject owner, jfieldID field) {
    return (void *)(intptr_t)(*environment)->GetLongField(environment, owner, field);
}

/* A new byte array holding view; NULL with an exception pending when the
   JVM cannot allocate it. */
static jbyteArray sxml_kotlin_new_bytes(JNIEnv *environment, sxml_byte_view view) {
    const jsize byte_count = (jsize)view.byte_count;
    jbyteArray bytes = (*environment)->NewByteArray(environment, byte_count);
    if (bytes != NULL && byte_count > 0) {
        (*environment)->SetByteArrayRegion(environment, bytes, 0, byte_count,
                                           (const jbyte *)view.bytes);
    }
    return bytes;
}

/* A byte array field of an object. */
typedef struct sxml_kotlin_byte_field {
    jobject owner;
    jfieldID field;
} sxml_kotlin_byte_field;

/* Sets target to a new byte array of view; does nothing with an exception
   pending. */
static void sxml_kotlin_set_bytes(JNIEnv *environment, sxml_kotlin_byte_field target,
                                  sxml_byte_view view) {
    if ((*environment)->ExceptionCheck(environment)) {
        return;
    }
    jbyteArray bytes = sxml_kotlin_new_bytes(environment, view);
    if (bytes != NULL) {
        (*environment)->SetObjectField(environment, target.owner, target.field, bytes);
        (*environment)->DeleteLocalRef(environment, bytes);
    }
}

/* Writes the processor's error into the request's NativeError. */
static void sxml_kotlin_write_error(const sxml_kotlin_call *call) {
    JNIEnv *environment = call->environment;
    if ((*environment)->ExceptionCheck(environment)) {
        return;
    }
    const sxml_kotlin_field_table *table = &call->glue->table;
    const sxml_host_error *error = sxml_host_error_of(call->glue->processor);
    jobject error_object =
        (*environment)->GetObjectField(environment, call->request, table->request.error);
    if (error_object == NULL) {
        return;
    }
    (*environment)->SetIntField(environment, error_object, table->error.status, (jint)error->status);
    (*environment)->SetIntField(environment, error_object, table->error.line_number,
                                (jint)error->line_number);
    (*environment)->SetIntField(environment, error_object, table->error.column_number,
                                (jint)error->column_number);
    const struct {
        jfieldID field;
        sxml_byte_view text;
    } texts[] = {
        {table->error.code_namespace_uri, error->code_namespace_uri},
        {table->error.code_local_name, error->code_local_name},
        {table->error.code_namespace_prefix, error->code_namespace_prefix},
        {table->error.description, error->description},
        {table->error.source_name, error->source_name},
    };
    for (size_t text_index = 0; text_index < sizeof(texts) / sizeof(texts[0]); text_index += 1) {
        sxml_kotlin_set_bytes(environment,
                              (sxml_kotlin_byte_field){.owner = error_object,
                                                       .field = texts[text_index].field},
                              texts[text_index].text);
    }
    (*environment)->DeleteLocalRef(environment, error_object);
}

/* Writes an out-of-memory failure into the request's NativeError. */
static void sxml_kotlin_write_out_of_memory(JNIEnv *environment, jobject request,
                                            const sxml_kotlin_field_table *table) {
    if ((*environment)->ExceptionCheck(environment)) {
        return;
    }
    jobject error_object = (*environment)->GetObjectField(environment, request, table->request.error);
    if (error_object != NULL) {
        (*environment)->SetIntField(environment, error_object, table->error.status,
                                    (jint)SXML_STATUS_OUT_OF_MEMORY);
        (*environment)->DeleteLocalRef(environment, error_object);
    }
}

/* Writes the handle a call made into the request, or its failure when it
   is NULL. Returns false, so that the caller releases the handle, when an
   exception is pending (the request is then not written). */
static bool sxml_kotlin_finish(const sxml_kotlin_call *call, const void *handle) {
    JNIEnv *environment = call->environment;
    if ((*environment)->ExceptionCheck(environment)) {
        return false;
    }
    if (handle == NULL) {
        sxml_kotlin_write_error(call);
        return true;
    }
    (*environment)->SetLongField(environment, call->request, call->glue->table.request.handle,
                                 sxml_kotlin_handle_of(handle));
    return true;
}

/* ----------------------------------------------------------- Side results */

/* Releases the values of side results that do not reach Kotlin. */
static void sxml_kotlin_release_side_values(sxml_host_side_result_list list) {
    for (size_t result_index = 0; result_index < list.side_result_count; result_index += 1) {
        sxml_host_value_release(list.side_results[result_index].first_value);
        sxml_host_value_release(list.side_results[result_index].second_value);
    }
}

/* The flat arrays of NativeSideResults, made for one list. */
typedef struct sxml_kotlin_side_arrays {
    jintArray kinds;
    jobjectArray texts;
    jlongArray values;
    jintArray numbers;
} sxml_kotlin_side_arrays;

/* Makes the arrays for result_count side results; texts NULL with an
   exception pending when the JVM cannot allocate them. */
static sxml_kotlin_side_arrays sxml_kotlin_side_arrays_make(JNIEnv *environment,
                                                            jsize result_count) {
    sxml_kotlin_side_arrays arrays = {
        .kinds = NULL, .texts = NULL, .values = NULL, .numbers = NULL};
    arrays.kinds = (*environment)->NewIntArray(environment, result_count);
    if (arrays.kinds != NULL) {
        arrays.numbers = (*environment)->NewIntArray(environment, 3 * result_count);
    }
    if (arrays.numbers != NULL) {
        arrays.values = (*environment)->NewLongArray(environment, 2 * result_count);
    }
    if (arrays.values != NULL) {
        jclass bytes_class = (*environment)->FindClass(environment, SXML_KOTLIN_BYTES);
        if (bytes_class != NULL) {
            arrays.texts =
                (*environment)->NewObjectArray(environment, 3 * result_count, bytes_class, NULL);
            (*environment)->DeleteLocalRef(environment, bytes_class);
        }
    }
    return arrays;
}

/* Fills arrays with every side result of list; false with an exception
   pending when a text cannot be allocated. */
static bool sxml_kotlin_side_arrays_fill(JNIEnv *environment, const sxml_kotlin_side_arrays *arrays,
                                         sxml_host_side_result_list list) {
    for (size_t result_index = 0; result_index < list.side_result_count; result_index += 1) {
        const sxml_host_side_result *side_result = &list.side_results[result_index];
        const jsize position = (jsize)result_index;
        const jint kind = (jint)side_result->side_result_kind;
        const jint numbers[3] = {(jint)side_result->line_number, (jint)side_result->column_number,
                                 (jint)side_result->is_terminating};
        const jlong values[2] = {sxml_kotlin_handle_of(side_result->first_value),
                                 sxml_kotlin_handle_of(side_result->second_value)};
        (*environment)->SetIntArrayRegion(environment, arrays->kinds, position, 1, &kind);
        (*environment)->SetIntArrayRegion(environment, arrays->numbers, 3 * position, 3, numbers);
        (*environment)->SetLongArrayRegion(environment, arrays->values, 2 * position, 2, values);
        const sxml_byte_view texts[3] = {side_result->uri, side_result->text, side_result->code};
        for (jsize text_index = 0; text_index < 3; text_index += 1) {
            jbyteArray bytes = sxml_kotlin_new_bytes(environment, texts[text_index]);
            if (bytes == NULL) {
                return false;
            }
            (*environment)->SetObjectArrayElement(environment, arrays->texts,
                                                  3 * position + text_index, bytes);
            (*environment)->DeleteLocalRef(environment, bytes);
        }
    }
    return true;
}

/*
 * Writes a call's side results into the request's NativeSideResults; their
 * values are Kotlin's from then on, to adopt (sxml_host.h). Returns false,
 * with every value released in C, when an exception is pending: Kotlin
 * then never reads the request.
 */
static bool sxml_kotlin_write_side_results(const sxml_kotlin_call *call,
                                           sxml_host_side_result_list list) {
    JNIEnv *environment = call->environment;
    if ((*environment)->ExceptionCheck(environment)) {
        sxml_kotlin_release_side_values(list);
        return false;
    }
    if (list.side_result_count == 0) {
        return true;
    }
    const sxml_kotlin_side_result_fields *fields = &call->glue->table.side_results;
    jobject target = (*environment)->GetObjectField(environment, call->request, fields->side_results);
    sxml_kotlin_side_arrays arrays = {.kinds = NULL, .texts = NULL, .values = NULL, .numbers = NULL};
    if (target != NULL) {
        arrays = sxml_kotlin_side_arrays_make(environment, (jsize)list.side_result_count);
    }
    const bool is_written =
        arrays.texts != NULL && sxml_kotlin_side_arrays_fill(environment, &arrays, list);
    if (is_written) {
        (*environment)->SetObjectField(environment, target, fields->kinds, arrays.kinds);
        (*environment)->SetObjectField(environment, target, fields->texts, arrays.texts);
        (*environment)->SetObjectField(environment, target, fields->values, arrays.values);
        (*environment)->SetObjectField(environment, target, fields->numbers, arrays.numbers);
    } else {
        sxml_kotlin_release_side_values(list);
    }
    (*environment)->DeleteLocalRef(environment, arrays.kinds);
    (*environment)->DeleteLocalRef(environment, arrays.texts);
    (*environment)->DeleteLocalRef(environment, arrays.values);
    (*environment)->DeleteLocalRef(environment, arrays.numbers);
    (*environment)->DeleteLocalRef(environment, target);
    return is_written;
}

/* sxml_kotlin_finish, then the side results; false (the caller releases
   the handle) when either met a pending exception. */
static bool sxml_kotlin_finish_with_side_results(const sxml_kotlin_call *call, const void *handle,
                                                 sxml_host_side_result_list list) {
    const bool is_finished = sxml_kotlin_finish(call, handle);
    const bool is_written = sxml_kotlin_write_side_results(call, list);
    return is_finished && is_written;
}

/* ------------------------------------------------------- Request memory */

/*
 * The memory of one call's request: one block from the processor's
 * allocator holding the host shim's request arrays first and the copied
 * texts after, released together when the call returns.
 */
typedef struct sxml_kotlin_block {
    uint8_t *memory;
    size_t byte_count;
    size_t used_byte_count;
} sxml_kotlin_block;

/* byte_count rounded up to the alignment of any request struct. */
static size_t sxml_kotlin_rounded(size_t byte_count) {
    const size_t alignment = alignof(max_align_t);
    return (byte_count + alignment - 1) / alignment * alignment;
}

/* The bytes of one Kotlin byte array, rounded. */
static size_t sxml_kotlin_measure(JNIEnv *environment, jbyteArray bytes) {
    return bytes == NULL ? 0 : sxml_kotlin_rounded((size_t)(*environment)->GetArrayLength(environment, bytes));
}

/* Allocates block->byte_count bytes from the call's processor; false when
   out of memory. */
static bool sxml_kotlin_block_allocate(const sxml_kotlin_call *call, sxml_kotlin_block *block) {
    block->used_byte_count = 0;
    block->memory = sxml_host_allocate(call->glue->processor, block->byte_count);
    return block->memory != NULL;
}

/* The next byte_count bytes of the block (measured before). */
static void *sxml_kotlin_block_take(sxml_kotlin_block *block, size_t byte_count) {
    void *memory = block->memory + block->used_byte_count;
    block->used_byte_count += sxml_kotlin_rounded(byte_count);
    return memory;
}

/* Copies a Kotlin byte array into the block, as a view of the copy. */
static sxml_byte_view sxml_kotlin_block_copy(JNIEnv *environment, sxml_kotlin_block *block,
                                             jbyteArray bytes) {
    if (bytes == NULL) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const jsize byte_count = (*environment)->GetArrayLength(environment, bytes);
    uint8_t *copy = sxml_kotlin_block_take(block, (size_t)byte_count);
    if (byte_count > 0) {
        (*environment)->GetByteArrayRegion(environment, bytes, 0, byte_count, (jbyte *)copy);
    }
    return (sxml_byte_view){.bytes = copy, .byte_count = (size_t)byte_count};
}

/* ---------------------------------------------------------------- Resolver */

/*
 * The host shim's resolver (sxml_host_resolver): asks ProcessorState.resolve
 * on the calling thread and copies its answer (resolverAnswer) into the
 * requesting processor's memory. A status other than SXML_STATUS_OK stops
 * the call; the binding then rethrows what the host's resolver threw.
 */
static uint32_t sxml_kotlin_resolve(void *state, const sxml_host_resource_request *request,
                                    sxml_host_resource *resource) {
    const sxml_kotlin_processor *glue = state;
    JNIEnv *environment = glue->environment;
    if ((*environment)->ExceptionCheck(environment)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    jbyteArray uri_reference = sxml_kotlin_new_bytes(environment, request->uri_reference);
    jbyteArray absolute_uri =
        uri_reference == NULL ? NULL : sxml_kotlin_new_bytes(environment, request->absolute_uri);
    jint status = (jint)SXML_STATUS_INPUT_OUTPUT_FAILED;
    if (absolute_uri != NULL) {
        status = (*environment)->CallIntMethod(environment, glue->state, glue->table.state.resolve,
                                               (jint)request->resource_kind, uri_reference,
                                               absolute_uri);
    }
    (*environment)->DeleteLocalRef(environment, uri_reference);
    (*environment)->DeleteLocalRef(environment, absolute_uri);
    if ((*environment)->ExceptionCheck(environment)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    if (status != (jint)SXML_STATUS_OK) {
        return (uint32_t)status;
    }
    jbyteArray answer =
        (*environment)->GetObjectField(environment, glue->state, glue->table.state.resolver_answer);
    if (answer == NULL) {
        return SXML_STATUS_OK;
    }
    (*environment)->SetObjectField(environment, glue->state, glue->table.state.resolver_answer, NULL);
    const jsize byte_count = (*environment)->GetArrayLength(environment, answer);
    uint8_t *bytes = sxml_host_allocate(request->processor, (size_t)byte_count);
    if (bytes == NULL) {
        (*environment)->DeleteLocalRef(environment, answer);
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if (byte_count > 0) {
        (*environment)->GetByteArrayRegion(environment, answer, 0, byte_count, (jbyte *)bytes);
    }
    (*environment)->DeleteLocalRef(environment, answer);
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = (sxml_byte_view){.bytes = bytes, .byte_count = (size_t)byte_count};
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- Entry points */

/* The names JNI looks up for the external functions of
   dev.tinyactors.sxml.Native (JNI 21, "Resolving Native Method Names"). */
/* NOLINTBEGIN(readability-identifier-naming): JNI's names, not sxml_*. */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *machine, void *reserved);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_library(JNIEnv *environment, jclass owner,
                                                               jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_processorCreate(JNIEnv *environment,
                                                                       jclass owner,
                                                                       jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_processorDestroy(JNIEnv *environment,
                                                                        jclass owner,
                                                                        jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_liveAllocationCount(JNIEnv *environment,
                                                                           jclass owner,
                                                                           jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_parse(JNIEnv *environment, jclass owner,
                                                             jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_string(JNIEnv *environment, jclass owner,
                                                              jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_compile(JNIEnv *environment, jclass owner,
                                                               jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_evaluate(JNIEnv *environment, jclass owner,
                                                                jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_serialize(JNIEnv *environment,
                                                                 jclass owner, jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_release(JNIEnv *environment, jclass owner,
                                                               jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_queryCompile(JNIEnv *environment,
                                                                    jclass owner,
                                                                    jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_queryEvaluate(JNIEnv *environment,
                                                                     jclass owner,
                                                                     jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_querySerialize(JNIEnv *environment,
                                                                      jclass owner,
                                                                      jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_stylesheetCompile(JNIEnv *environment,
                                                                         jclass owner,
                                                                         jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_transform(JNIEnv *environment, jclass owner,
                                                                 jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_schemaCompile(JNIEnv *environment,
                                                                     jclass owner,
                                                                     jobject request);
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_schemaValidate(JNIEnv *environment,
                                                                      jclass owner,
                                                                      jobject request);
/* NOLINTEND(readability-identifier-naming) */

/* The JNI version the glue needs; JNI 1.6 is what Android supports. */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *machine, void *reserved) {
    (void)machine;
    (void)reserved;
    return JNI_VERSION_1_6;
}

/* LibraryRequest: the version and the feature names (sxml_host_features). */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_library(JNIEnv *environment, jclass owner,
                                                               jobject request) {
    (void)owner;
    jclass request_class = (*environment)->GetObjectClass(environment, request);
    const jfieldID version_field =
        (*environment)->GetFieldID(environment, request_class, "version", SXML_KOTLIN_BYTES);
    const jfieldID features_field =
        version_field == NULL ? NULL
                              : (*environment)->GetFieldID(environment, request_class, "features",
                                                           "[" SXML_KOTLIN_BYTES);
    (*environment)->DeleteLocalRef(environment, request_class);
    if (features_field == NULL) {
        return;
    }
    const char *version = sxml_host_version_text();
    sxml_kotlin_set_bytes(
        environment, (sxml_kotlin_byte_field){.owner = request, .field = version_field},
        (sxml_byte_view){.bytes = (const uint8_t *)version, .byte_count = strlen(version)});
    jclass bytes_class = (*environment)->FindClass(environment, SXML_KOTLIN_BYTES);
    if (bytes_class == NULL) {
        return;
    }
    const uint32_t feature_count = sxml_host_feature_count();
    jobjectArray features =
        (*environment)->NewObjectArray(environment, (jsize)feature_count, bytes_class, NULL);
    (*environment)->DeleteLocalRef(environment, bytes_class);
    for (uint32_t feature_index = 0; features != NULL && feature_index < feature_count;
         feature_index += 1) {
        jbyteArray name = sxml_kotlin_new_bytes(
            environment,
            (sxml_byte_view){.bytes = sxml_host_feature_name_bytes(feature_index),
                             .byte_count = sxml_host_feature_name_byte_count(feature_index)});
        if (name == NULL) {
            break;
        }
        (*environment)->SetObjectArrayElement(environment, features, (jsize)feature_index, name);
        (*environment)->DeleteLocalRef(environment, name);
    }
    if (features != NULL && !(*environment)->ExceptionCheck(environment)) {
        (*environment)->SetObjectField(environment, request, features_field, features);
    }
    (*environment)->DeleteLocalRef(environment, features);
}

/*
 * ProcessorRequest: makes the glue state and the host shim's processor
 * with the request's failing allocation number, feature mask and state
 * (whose resolve answers the resolver), and writes the glue state to the
 * request's processor field; 0 with OUT_OF_MEMORY when out of memory.
 */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_processorCreate(JNIEnv *environment,
                                                                       jclass owner,
                                                                       jobject request) {
    (void)owner;
    sxml_kotlin_field_table table;
    memset(&table, 0, sizeof(table));
    if (!sxml_kotlin_look_up(environment, &table)) {
        return;
    }
    const sxml_allocator allocator = sxml_default_allocator();
    void *memory = NULL;
    if (sxml_allocator_allocate(allocator,
                                (sxml_allocation_request){.byte_count = sizeof(sxml_kotlin_processor),
                                                          .alignment = alignof(sxml_kotlin_processor)},
                                &memory) != SXML_STATUS_OK) {
        sxml_kotlin_write_out_of_memory(environment, request, &table);
        return;
    }
    sxml_kotlin_processor *glue = memory;
    *glue = (sxml_kotlin_processor){
        .processor = NULL, .state = NULL, .environment = environment, .table = table};
    jobject state = (*environment)->GetObjectField(environment, request, table.processor.state);
    glue->state = state == NULL ? NULL : (*environment)->NewGlobalRef(environment, state);
    (*environment)->DeleteLocalRef(environment, state);
    if (glue->state != NULL) {
        glue->processor = sxml_host_processor_create(&(sxml_host_processor_configuration){
            .failing_allocation_number = (uint32_t)(*environment)->GetIntField(
                environment, request, table.processor.failing_allocation_number),
            .feature_mask =
                (uint32_t)(*environment)->GetIntField(environment, request, table.processor.feature_mask),
            .resolver = {.state = glue, .resolve = sxml_kotlin_resolve}});
    }
    if (glue->processor == NULL) {
        if (glue->state != NULL) {
            (*environment)->DeleteGlobalRef(environment, glue->state);
        }
        sxml_allocator_release(allocator, glue);
        sxml_kotlin_write_out_of_memory(environment, request, &table);
        return;
    }
    (*environment)->SetLongField(environment, request, table.request.processor,
                                 sxml_kotlin_handle_of(glue));
}

/* ProcessorRequest: destroys the processor (every handle released before)
   and writes the allocations still live to allocationCount. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_processorDestroy(JNIEnv *environment,
                                                                        jclass owner,
                                                                        jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const uint32_t leaked_allocation_count = sxml_host_processor_destroy(glue->processor);
    (*environment)->DeleteGlobalRef(environment, glue->state);
    const jfieldID allocation_count = glue->table.processor.allocation_count;
    sxml_allocator_release(sxml_default_allocator(), glue);
    (*environment)->SetIntField(environment, request, allocation_count, (jint)leaked_allocation_count);
}

/* ProcessorRequest: writes the live allocation count. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_liveAllocationCount(JNIEnv *environment,
                                                                           jclass owner,
                                                                           jobject request) {
    (void)owner;
    const sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    (*environment)->SetIntField(environment, request, glue->table.processor.allocation_count,
                                (jint)sxml_host_live_allocation_count(glue->processor));
}

/* What a text request's operation is. */
typedef enum sxml_kotlin_text_operation {
    SXML_KOTLIN_TEXT_OPERATION_PARSE,
    SXML_KOTLIN_TEXT_OPERATION_STRING,
} sxml_kotlin_text_operation;

/* TextRequest: parses a document or makes an xs:string. */
static void sxml_kotlin_text_call(JNIEnv *environment, jobject request,
                                  sxml_kotlin_text_operation operation) {
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_text_fields *fields = &glue->table.text;
    jbyteArray text = (*environment)->GetObjectField(environment, request, fields->text);
    jbyteArray uri = (*environment)->GetObjectField(environment, request, fields->uri);
    sxml_kotlin_block block = {.memory = NULL,
                               .byte_count = sxml_kotlin_measure(environment, text) +
                                             sxml_kotlin_measure(environment, uri),
                               .used_byte_count = 0};
    sxml_host_value *value = NULL;
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        const sxml_byte_view text_view = sxml_kotlin_block_copy(environment, &block, text);
        const sxml_byte_view uri_view = sxml_kotlin_block_copy(environment, &block, uri);
        value = operation == SXML_KOTLIN_TEXT_OPERATION_PARSE
                    ? sxml_host_parse(glue->processor,
                                      &(sxml_host_parse_request){.input = text_view,
                                                                 .document_uri = uri_view})
                    : sxml_host_string(glue->processor, &text_view);
        if (!sxml_kotlin_finish(&call, value)) {
            sxml_host_value_release(value);
        }
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, text);
    (*environment)->DeleteLocalRef(environment, uri);
}

JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_parse(JNIEnv *environment, jclass owner,
                                                             jobject request) {
    (void)owner;
    sxml_kotlin_text_call(environment, request, SXML_KOTLIN_TEXT_OPERATION_PARSE);
}

JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_string(JNIEnv *environment, jclass owner,
                                                              jobject request) {
    (void)owner;
    sxml_kotlin_text_call(environment, request, SXML_KOTLIN_TEXT_OPERATION_STRING);
}

typedef struct sxml_kotlin_text_pairs {
    jobjectArray texts;
    size_t pair_count;
} sxml_kotlin_text_pairs;

/* The byte arrays of an array field of owner, in pairs. */
static sxml_kotlin_text_pairs sxml_kotlin_pairs_in(JNIEnv *environment, jobject owner,
                                                   jfieldID field) {
    jobjectArray texts = (*environment)->GetObjectField(environment, owner, field);
    const size_t text_count =
        texts == NULL ? 0 : (size_t)(*environment)->GetArrayLength(environment, texts);
    return (sxml_kotlin_text_pairs){.texts = texts, .pair_count = text_count / 2};
}

static sxml_kotlin_text_pairs sxml_kotlin_pairs_of(const sxml_kotlin_call *call, jfieldID field) {
    return sxml_kotlin_pairs_in(call->environment, call->request, field);
}

/* The rounded bytes of every text of pairs. */
static size_t sxml_kotlin_measure_pairs(JNIEnv *environment, sxml_kotlin_text_pairs pairs) {
    size_t byte_count = 0;
    for (size_t text_index = 0; text_index < pairs.pair_count * 2; text_index += 1) {
        jbyteArray text =
            (*environment)->GetObjectArrayElement(environment, pairs.texts, (jsize)text_index);
        byte_count += sxml_kotlin_measure(environment, text);
        (*environment)->DeleteLocalRef(environment, text);
    }
    return byte_count;
}

/* Where to copy pairs: a block, and the views to write (2 per pair). */
typedef struct sxml_kotlin_pair_target {
    sxml_kotlin_block *block;
    sxml_byte_view *views;
} sxml_kotlin_pair_target;

/* Copies every text of pairs into the target's block, writing its views. */
static void sxml_kotlin_copy_pairs(JNIEnv *environment, sxml_kotlin_text_pairs pairs,
                                   sxml_kotlin_pair_target target) {
    for (size_t text_index = 0; text_index < pairs.pair_count * 2; text_index += 1) {
        jbyteArray text =
            (*environment)->GetObjectArrayElement(environment, pairs.texts, (jsize)text_index);
        target.views[text_index] = sxml_kotlin_block_copy(environment, target.block, text);
        (*environment)->DeleteLocalRef(environment, text);
    }
}

/* Takes view_count views from the block, each the empty view, and copies
   the texts of pairs into the first ones (names that may be absent). */
typedef struct sxml_kotlin_view_request {
    sxml_kotlin_text_pairs pairs;
    size_t view_count;
} sxml_kotlin_view_request;

static sxml_byte_view *sxml_kotlin_take_views(JNIEnv *environment, sxml_kotlin_block *block,
                                              sxml_kotlin_view_request request) {
    sxml_byte_view *views = sxml_kotlin_block_take(block, request.view_count * sizeof(sxml_byte_view));
    for (size_t view_index = 0; view_index < request.view_count; view_index += 1) {
        views[view_index] = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    sxml_kotlin_text_pairs pairs = request.pairs;
    if (pairs.pair_count * 2 > request.view_count) {
        pairs.pair_count = request.view_count / 2;
    }
    sxml_kotlin_copy_pairs(environment, pairs,
                           (sxml_kotlin_pair_target){.block = block, .views = views});
    return views;
}

/* ----------------------------------------------------- Handles, bindings */

/* The value handles of a long array, as the host shim's array of values;
   the block holds measure_handles bytes for it. */
static size_t sxml_kotlin_measure_handles(JNIEnv *environment, jlongArray handles) {
    const size_t handle_count =
        handles == NULL ? 0 : (size_t)(*environment)->GetArrayLength(environment, handles);
    return sxml_kotlin_rounded(handle_count * sizeof(void *));
}

static const sxml_host_value **sxml_kotlin_copy_handles(JNIEnv *environment, jlongArray handles,
                                                        sxml_kotlin_block *block) {
    const jsize handle_count =
        handles == NULL ? 0 : (*environment)->GetArrayLength(environment, handles);
    const sxml_host_value **values =
        (const sxml_host_value **)sxml_kotlin_block_take(block, (size_t)handle_count * sizeof(void *));
    for (jsize handle_index = 0; handle_index < handle_count; handle_index += 1) {
        jlong handle = 0;
        (*environment)->GetLongArrayRegion(environment, handles, handle_index, 1, &handle);
        values[handle_index] = (const sxml_host_value *)(intptr_t)handle;
    }
    return values;
}

/* A NativeBindings field of a request: the object, its names and values. */
typedef struct sxml_kotlin_bindings {
    jobject owner;
    sxml_kotlin_text_pairs names;
    jlongArray values;
    size_t binding_count;
} sxml_kotlin_bindings;

static sxml_kotlin_bindings sxml_kotlin_bindings_of(const sxml_kotlin_call *call, jfieldID field) {
    JNIEnv *environment = call->environment;
    const sxml_kotlin_binding_fields *fields = &call->glue->table.bindings;
    sxml_kotlin_bindings bindings = {
        .owner = (*environment)->GetObjectField(environment, call->request, field),
        .names = {.texts = NULL, .pair_count = 0},
        .values = NULL,
        .binding_count = 0};
    if (bindings.owner == NULL) {
        return bindings;
    }
    bindings.names = sxml_kotlin_pairs_in(environment, bindings.owner, fields->names);
    bindings.values = (*environment)->GetObjectField(environment, bindings.owner, fields->values);
    const size_t value_count = bindings.values == NULL
                                   ? 0
                                   : (size_t)(*environment)->GetArrayLength(environment,
                                                                            bindings.values);
    bindings.binding_count =
        bindings.names.pair_count < value_count ? bindings.names.pair_count : value_count;
    return bindings;
}

/* The bytes of the binding list and its names. */
static size_t sxml_kotlin_measure_bindings(JNIEnv *environment,
                                           const sxml_kotlin_bindings *bindings) {
    return sxml_kotlin_rounded(bindings->binding_count * sizeof(sxml_host_binding)) +
           sxml_kotlin_measure_pairs(environment, bindings->names);
}

/* The bindings as the host shim's list, copied into the block. */
static sxml_host_binding_list sxml_kotlin_copy_bindings(JNIEnv *environment,
                                                        const sxml_kotlin_bindings *bindings,
                                                        sxml_kotlin_block *block) {
    sxml_host_binding *list =
        sxml_kotlin_block_take(block, bindings->binding_count * sizeof(sxml_host_binding));
    for (size_t binding_index = 0; binding_index < bindings->binding_count; binding_index += 1) {
        jbyteArray namespace_uri = (*environment)->GetObjectArrayElement(
            environment, bindings->names.texts, (jsize)(2 * binding_index));
        jbyteArray local_name = (*environment)->GetObjectArrayElement(
            environment, bindings->names.texts, (jsize)(2 * binding_index + 1));
        jlong handle = 0;
        (*environment)->GetLongArrayRegion(environment, bindings->values, (jsize)binding_index, 1,
                                           &handle);
        list[binding_index] = (sxml_host_binding){
            .name = {.namespace_uri = sxml_kotlin_block_copy(environment, block, namespace_uri),
                     .local_name = sxml_kotlin_block_copy(environment, block, local_name)},
            .value = (const sxml_host_value *)(intptr_t)handle};
        (*environment)->DeleteLocalRef(environment, namespace_uri);
        (*environment)->DeleteLocalRef(environment, local_name);
    }
    return (sxml_host_binding_list){.bindings = list, .binding_count = bindings->binding_count};
}

/* Deletes the local references of bindings. */
static void sxml_kotlin_bindings_delete(JNIEnv *environment, const sxml_kotlin_bindings *bindings) {
    (*environment)->DeleteLocalRef(environment, bindings->names.texts);
    (*environment)->DeleteLocalRef(environment, bindings->values);
    (*environment)->DeleteLocalRef(environment, bindings->owner);
}

/* What a compile request compiles. */
typedef enum sxml_kotlin_compile_operation {
    SXML_KOTLIN_COMPILE_OPERATION_XPATH,
    SXML_KOTLIN_COMPILE_OPERATION_QUERY,
} sxml_kotlin_compile_operation;

/* The texts of a CompileRequest, copied into the block. */
typedef struct sxml_kotlin_compile_texts {
    sxml_byte_view expression_text;
    sxml_byte_view base_uri;
    const sxml_host_namespace *namespaces;
    size_t namespace_count;
    const sxml_host_variable *variables;
    size_t variable_count;
} sxml_kotlin_compile_texts;

/* Compiles the copied texts as operation says, and writes the handle (for
   a query also isUpdating) into the request. */
static void sxml_kotlin_compile_copied(const sxml_kotlin_call *call,
                                       const sxml_kotlin_compile_texts *texts,
                                       sxml_kotlin_compile_operation operation) {
    JNIEnv *environment = call->environment;
    sxml_host_processor *processor = call->glue->processor;
    if (operation == SXML_KOTLIN_COMPILE_OPERATION_XPATH) {
        sxml_host_expression *expression = sxml_host_compile(
            processor, &(sxml_host_compile_request){.expression_text = texts->expression_text,
                                                    .static_base_uri = texts->base_uri,
                                                    .namespaces = texts->namespaces,
                                                    .namespace_count = texts->namespace_count,
                                                    .variables = texts->variables,
                                                    .variable_count = texts->variable_count});
        if (!sxml_kotlin_finish(call, expression)) {
            sxml_host_expression_destroy(expression);
        }
        return;
    }
    sxml_host_query *query = sxml_host_query_compile(
        processor, &(sxml_host_query_compile_request){.query_text = texts->expression_text,
                                                      .static_base_uri = texts->base_uri,
                                                      .namespaces = texts->namespaces,
                                                      .namespace_count = texts->namespace_count});
    if (query != NULL && !(*environment)->ExceptionCheck(environment)) {
        (*environment)->SetBooleanField(environment, call->request,
                                        call->glue->table.compile.is_updating,
                                        sxml_host_query_is_updating(query) != 0 ? JNI_TRUE
                                                                                : JNI_FALSE);
    }
    if (!sxml_kotlin_finish(call, query)) {
        sxml_host_query_destroy(query);
    }
}

/* CompileRequest: compiles an XPath expression with its namespaces and
   external variables (sxml_host_compile), or an XQuery main module with
   its namespaces (sxml_host_query_compile). */
static void sxml_kotlin_compile_call(JNIEnv *environment, jobject request,
                                     sxml_kotlin_compile_operation operation) {
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_compile_fields *fields = &glue->table.compile;
    jbyteArray expression_text =
        (*environment)->GetObjectField(environment, request, fields->expression_text);
    jbyteArray base_uri = (*environment)->GetObjectField(environment, request, fields->base_uri);
    const sxml_kotlin_text_pairs namespaces = sxml_kotlin_pairs_of(&call, fields->namespaces);
    const sxml_kotlin_text_pairs variables = sxml_kotlin_pairs_of(&call, fields->variables);
    /* sxml_host_namespace and sxml_host_variable are two byte views each,
       which the copies write in place. */
    _Static_assert(sizeof(sxml_host_namespace) == 2 * sizeof(sxml_byte_view), "namespace: 2 views");
    _Static_assert(sizeof(sxml_host_variable) == 2 * sizeof(sxml_byte_view), "variable: 2 views");
    const size_t namespace_byte_count = namespaces.pair_count * sizeof(sxml_host_namespace);
    const size_t variable_byte_count = variables.pair_count * sizeof(sxml_host_variable);
    sxml_kotlin_block block = {
        .memory = NULL,
        .byte_count = sxml_kotlin_rounded(namespace_byte_count) +
                      sxml_kotlin_rounded(variable_byte_count) +
                      sxml_kotlin_measure(environment, expression_text) +
                      sxml_kotlin_measure(environment, base_uri) +
                      sxml_kotlin_measure_pairs(environment, namespaces) +
                      sxml_kotlin_measure_pairs(environment, variables),
        .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        sxml_host_namespace *namespace_list = sxml_kotlin_block_take(&block, namespace_byte_count);
        sxml_host_variable *variable_list = sxml_kotlin_block_take(&block, variable_byte_count);
        const sxml_kotlin_compile_texts texts = {
            .expression_text = sxml_kotlin_block_copy(environment, &block, expression_text),
            .base_uri = sxml_kotlin_block_copy(environment, &block, base_uri),
            .namespaces = namespace_list,
            .namespace_count = namespaces.pair_count,
            .variables = variable_list,
            .variable_count = variables.pair_count};
        sxml_kotlin_copy_pairs(
            environment, namespaces,
            (sxml_kotlin_pair_target){.block = &block, .views = (sxml_byte_view *)namespace_list});
        sxml_kotlin_copy_pairs(
            environment, variables,
            (sxml_kotlin_pair_target){.block = &block, .views = (sxml_byte_view *)variable_list});
        sxml_kotlin_compile_copied(&call, &texts, operation);
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, expression_text);
    (*environment)->DeleteLocalRef(environment, base_uri);
    (*environment)->DeleteLocalRef(environment, namespaces.texts);
    (*environment)->DeleteLocalRef(environment, variables.texts);
}

JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_compile(JNIEnv *environment, jclass owner,
                                                               jobject request) {
    (void)owner;
    sxml_kotlin_compile_call(environment, request, SXML_KOTLIN_COMPILE_OPERATION_XPATH);
}

/* EvaluateRequest: evaluates an expression with a context value and one
   value (or 0, unbound) per variable (sxml_host_evaluate). */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_evaluate(JNIEnv *environment, jclass owner,
                                                                jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_evaluate_fields *fields = &glue->table.evaluate;
    sxml_host_expression *expression = sxml_kotlin_pointer_of(environment, request, fields->expression);
    const sxml_host_value *context = sxml_kotlin_pointer_of(environment, request, fields->context);
    jlongArray handles = (*environment)->GetObjectField(environment, request, fields->variable_values);
    const jsize value_count = handles == NULL ? 0 : (*environment)->GetArrayLength(environment, handles);
    sxml_kotlin_block block = {.memory = NULL,
                               .byte_count = sxml_kotlin_measure_handles(environment, handles),
                               .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        const sxml_host_value **values = sxml_kotlin_copy_handles(environment, handles, &block);
        sxml_host_value *result = sxml_host_evaluate(
            expression, &(sxml_host_evaluate_request){.context_value = context,
                                                      .variable_values = values,
                                                      .variable_value_count = (size_t)value_count});
        if (!sxml_kotlin_finish(&call, result)) {
            sxml_host_value_release(result);
        }
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, handles);
}

/* SerializeRequest: serializes a value, adaptively or with an options map
   (sxml_host_serialize), and writes the text to output. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_serialize(JNIEnv *environment,
                                                                 jclass owner, jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_serialize_fields *fields = &glue->table.serialize;
    const sxml_host_serialize_request serialize_request = {
        .value = sxml_kotlin_pointer_of(environment, request, fields->value),
        .options = sxml_kotlin_pointer_of(environment, request, fields->options)};
    if (sxml_host_serialize(glue->processor, &serialize_request) != SXML_STATUS_OK) {
        sxml_kotlin_write_error(&call);
        return;
    }
    sxml_kotlin_set_bytes(environment,
                          (sxml_kotlin_byte_field){.owner = request, .field = fields->output},
                          *sxml_host_output_of(glue->processor));
}

/* ------------------------------------------------------------------ XQuery */

/* CompileRequest: compiles an XQuery main module (sxml_host_query_compile)
   and writes isUpdating. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_queryCompile(JNIEnv *environment,
                                                                    jclass owner,
                                                                    jobject request) {
    (void)owner;
    sxml_kotlin_compile_call(environment, request, SXML_KOTLIN_COMPILE_OPERATION_QUERY);
}

/* QueryEvaluateRequest: evaluates a query with a context value and
   external variables (sxml_host_query_evaluate), and writes its side
   results: updated trees, put documents and messages. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_queryEvaluate(JNIEnv *environment,
                                                                     jclass owner,
                                                                     jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_query_evaluate_fields *fields = &glue->table.query_evaluate;
    sxml_host_query *query = sxml_kotlin_pointer_of(environment, request, fields->query);
    const sxml_kotlin_bindings variables = sxml_kotlin_bindings_of(&call, fields->variables);
    sxml_kotlin_block block = {.memory = NULL,
                               .byte_count = sxml_kotlin_measure_bindings(environment, &variables),
                               .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        sxml_host_query_evaluate_request evaluate_request = {
            .context_value = sxml_kotlin_pointer_of(environment, request, fields->context),
            .variables = sxml_kotlin_copy_bindings(environment, &variables, &block),
            .accepts_put_documents =
                (*environment)->GetBooleanField(environment, request, fields->accepts_put_documents)
                    ? 1
                    : 0,
            .side_results = {.side_results = NULL, .side_result_count = 0}};
        sxml_host_value *result = sxml_host_query_evaluate(query, &evaluate_request);
        if (!sxml_kotlin_finish_with_side_results(&call, result, evaluate_request.side_results)) {
            sxml_host_value_release(result);
        }
    }
    sxml_host_release(glue->processor, block.memory);
    sxml_kotlin_bindings_delete(environment, &variables);
}

/* QuerySerializeRequest: serializes a value with the query's output
   declarations (sxml_host_query_serialize), and writes the text to
   output. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_querySerialize(JNIEnv *environment,
                                                                      jclass owner,
                                                                      jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_serialize_fields *fields = &glue->table.serialize;
    sxml_host_query *query =
        sxml_kotlin_pointer_of(environment, request, glue->table.query_serialize.query);
    const sxml_host_value *value = sxml_kotlin_pointer_of(environment, request, fields->value);
    if (sxml_host_query_serialize(query, value) != SXML_STATUS_OK) {
        sxml_kotlin_write_error(&call);
        return;
    }
    sxml_kotlin_set_bytes(environment,
                          (sxml_kotlin_byte_field){.owner = request, .field = fields->output},
                          *sxml_host_output_of(glue->processor));
}

/* -------------------------------------------------------------------- XSLT */

/* StylesheetCompileRequest: compiles a stylesheet with its static
   parameters (sxml_host_stylesheet_compile). */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_stylesheetCompile(JNIEnv *environment,
                                                                         jclass owner,
                                                                         jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_stylesheet_compile_fields *fields = &glue->table.stylesheet_compile;
    jbyteArray text = (*environment)->GetObjectField(environment, request, fields->stylesheet_text);
    jbyteArray base_uri = (*environment)->GetObjectField(environment, request, fields->base_uri);
    const sxml_kotlin_bindings parameters = sxml_kotlin_bindings_of(&call, fields->static_parameters);
    sxml_kotlin_block block = {.memory = NULL,
                               .byte_count = sxml_kotlin_measure(environment, text) +
                                             sxml_kotlin_measure(environment, base_uri) +
                                             sxml_kotlin_measure_bindings(environment, &parameters),
                               .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        const sxml_host_stylesheet_compile_request compile_request = {
            .stylesheet_text = sxml_kotlin_block_copy(environment, &block, text),
            .static_base_uri = sxml_kotlin_block_copy(environment, &block, base_uri),
            .static_parameters = sxml_kotlin_copy_bindings(environment, &parameters, &block)};
        sxml_host_stylesheet *stylesheet =
            sxml_host_stylesheet_compile(glue->processor, &compile_request);
        if (!sxml_kotlin_finish(&call, stylesheet)) {
            sxml_host_stylesheet_destroy(stylesheet);
        }
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, text);
    (*environment)->DeleteLocalRef(environment, base_uri);
    sxml_kotlin_bindings_delete(environment, &parameters);
}

/* The arrays a TransformRequest holds, read before the block is
   measured. */
typedef struct sxml_kotlin_transform_arrays {
    sxml_kotlin_bindings stylesheet_parameters;
    sxml_kotlin_bindings template_parameters;
    sxml_kotlin_bindings tunnel_parameters;
    jlongArray function_arguments;
    jbyteArray base_output_uri;
    /* Initial mode, template and function: namespace URI and local name. */
    sxml_kotlin_text_pairs names;
} sxml_kotlin_transform_arrays;

/* The three names of a transform request. */
enum { SXML_KOTLIN_TRANSFORM_NAME_VIEW_COUNT = 6 };

static sxml_kotlin_transform_arrays sxml_kotlin_transform_arrays_of(const sxml_kotlin_call *call) {
    JNIEnv *environment = call->environment;
    const sxml_kotlin_transform_fields *fields = &call->glue->table.transform;
    return (sxml_kotlin_transform_arrays){
        .stylesheet_parameters = sxml_kotlin_bindings_of(call, fields->stylesheet_parameters),
        .template_parameters = sxml_kotlin_bindings_of(call, fields->template_parameters),
        .tunnel_parameters = sxml_kotlin_bindings_of(call, fields->tunnel_parameters),
        .function_arguments =
            (*environment)->GetObjectField(environment, call->request, fields->function_arguments),
        .base_output_uri =
            (*environment)->GetObjectField(environment, call->request, fields->base_output_uri),
        .names = sxml_kotlin_pairs_of(call, fields->names)};
}

static size_t sxml_kotlin_measure_transform(JNIEnv *environment,
                                            const sxml_kotlin_transform_arrays *arrays) {
    return sxml_kotlin_measure_bindings(environment, &arrays->stylesheet_parameters) +
           sxml_kotlin_measure_bindings(environment, &arrays->template_parameters) +
           sxml_kotlin_measure_bindings(environment, &arrays->tunnel_parameters) +
           sxml_kotlin_measure_handles(environment, arrays->function_arguments) +
           sxml_kotlin_measure(environment, arrays->base_output_uri) +
           sxml_kotlin_rounded(SXML_KOTLIN_TRANSFORM_NAME_VIEW_COUNT * sizeof(sxml_byte_view)) +
           sxml_kotlin_measure_pairs(environment, arrays->names);
}

static void sxml_kotlin_transform_arrays_delete(JNIEnv *environment,
                                                const sxml_kotlin_transform_arrays *arrays) {
    sxml_kotlin_bindings_delete(environment, &arrays->stylesheet_parameters);
    sxml_kotlin_bindings_delete(environment, &arrays->template_parameters);
    sxml_kotlin_bindings_delete(environment, &arrays->tunnel_parameters);
    (*environment)->DeleteLocalRef(environment, arrays->function_arguments);
    (*environment)->DeleteLocalRef(environment, arrays->base_output_uri);
    (*environment)->DeleteLocalRef(environment, arrays->names.texts);
}

/* The host shim's transform request of a TransformRequest, its arrays
   copied into the block. */
typedef struct sxml_kotlin_transform_source {
    const sxml_kotlin_call *call;
    const sxml_kotlin_transform_arrays *arrays;
} sxml_kotlin_transform_source;

static sxml_host_transform_request sxml_kotlin_transform_request_of(
    sxml_kotlin_transform_source source, sxml_kotlin_block *block) {
    JNIEnv *environment = source.call->environment;
    jobject request = source.call->request;
    const sxml_kotlin_transform_fields *fields = &source.call->glue->table.transform;
    const sxml_kotlin_transform_arrays *arrays = source.arrays;
    const sxml_byte_view *names = sxml_kotlin_take_views(
        environment, block,
        (sxml_kotlin_view_request){.pairs = arrays->names,
                                   .view_count = SXML_KOTLIN_TRANSFORM_NAME_VIEW_COUNT});
    const jsize argument_count = arrays->function_arguments == NULL
                                     ? 0
                                     : (*environment)->GetArrayLength(environment,
                                                                      arrays->function_arguments);
    return (sxml_host_transform_request){
        .global_context_item = sxml_kotlin_pointer_of(environment, request, fields->global_context_item),
        .initial_match_selection =
            sxml_kotlin_pointer_of(environment, request, fields->initial_match_selection),
        .stylesheet_parameters =
            sxml_kotlin_copy_bindings(environment, &arrays->stylesheet_parameters, block),
        .template_parameters =
            sxml_kotlin_copy_bindings(environment, &arrays->template_parameters, block),
        .tunnel_parameters = sxml_kotlin_copy_bindings(environment, &arrays->tunnel_parameters, block),
        .function_arguments =
            sxml_kotlin_copy_handles(environment, arrays->function_arguments, block),
        .function_argument_count = (size_t)argument_count,
        .base_output_uri = sxml_kotlin_block_copy(environment, block, arrays->base_output_uri),
        .serialization_options =
            sxml_kotlin_pointer_of(environment, request, fields->serialization_options),
        .initial_mode = {.namespace_uri = names[0], .local_name = names[1]},
        .initial_template = {.namespace_uri = names[2], .local_name = names[3]},
        .initial_function = {.namespace_uri = names[4], .local_name = names[5]},
        .invocation_kind =
            (uint32_t)(*environment)->GetIntField(environment, request, fields->invocation_kind),
        .result_delivery =
            (uint32_t)(*environment)->GetIntField(environment, request, fields->result_delivery),
        .result_document_delivery = (uint32_t)(*environment)->GetIntField(
            environment, request, fields->result_document_delivery),
        .selects_unnamed_mode =
            (*environment)->GetBooleanField(environment, request, fields->selects_unnamed_mode) ? 1
                                                                                               : 0,
        .side_results = {.side_results = NULL, .side_result_count = 0}};
}

/* TransformRequest: runs one transformation (sxml_host_stylesheet_transform),
   writes the principal result (a value handle, and output when it is
   serialized) and the side results: result documents and messages. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_transform(JNIEnv *environment, jclass owner,
                                                                 jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_transform_fields *fields = &glue->table.transform;
    sxml_host_stylesheet *stylesheet = sxml_kotlin_pointer_of(environment, request, fields->stylesheet);
    const sxml_kotlin_transform_arrays arrays = sxml_kotlin_transform_arrays_of(&call);
    sxml_kotlin_block block = {.memory = NULL,
                               .byte_count = sxml_kotlin_measure_transform(environment, &arrays),
                               .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        sxml_host_transform_request transform_request = sxml_kotlin_transform_request_of(
            (sxml_kotlin_transform_source){.call = &call, .arrays = &arrays}, &block);
        sxml_host_value *result = sxml_host_stylesheet_transform(stylesheet, &transform_request);
        if (result != NULL &&
            transform_request.result_delivery == (uint32_t)SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
            sxml_kotlin_set_bytes(environment,
                                  (sxml_kotlin_byte_field){.owner = request, .field = fields->output},
                                  *sxml_host_output_of(glue->processor));
        }
        if (!sxml_kotlin_finish_with_side_results(&call, result, transform_request.side_results)) {
            sxml_host_value_release(result);
        }
    }
    sxml_host_release(glue->processor, block.memory);
    sxml_kotlin_transform_arrays_delete(environment, &arrays);
}

/* --------------------------------------------------------------------- XSD */

/* Compiles the copied schema documents into schema and writes the
   outcome: the handle, or the error (schema then destroyed); the reports
   either way. */
static void sxml_kotlin_schema_compile_copied(const sxml_kotlin_call *call, sxml_host_schema *schema,
                                              sxml_host_schema_compile_request *compile_request) {
    const uint32_t status = sxml_host_schema_compile(schema, compile_request);
    const sxml_host_schema *handle = status == SXML_STATUS_OK ? schema : NULL;
    const bool is_kept =
        sxml_kotlin_finish_with_side_results(call, handle, compile_request->side_results);
    if (handle == NULL || !is_kept) {
        sxml_host_schema_destroy(schema);
    }
}

/* SchemaCompileRequest: makes a schema and compiles its documents (text
   and URI in pairs; a null text is a URI the resolver delivers) into it
   (sxml_host_schema_compile); writes every report as a side result. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_schemaCompile(JNIEnv *environment,
                                                                     jclass owner,
                                                                     jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_schema_compile_fields *fields = &glue->table.schema_compile;
    const sxml_kotlin_text_pairs documents = sxml_kotlin_pairs_of(&call, fields->documents);
    jbyteArray base_uri = (*environment)->GetObjectField(environment, request, fields->base_uri);
    /* sxml_host_schema_document is two byte views, which the copies write
       in place; a null text stays the NULL view (RESOLVED). */
    _Static_assert(sizeof(sxml_host_schema_document) == 2 * sizeof(sxml_byte_view),
                   "schema document: 2 views");
    const size_t document_byte_count = documents.pair_count * sizeof(sxml_host_schema_document);
    sxml_kotlin_block block = {.memory = NULL,
                               .byte_count = sxml_kotlin_rounded(document_byte_count) +
                                             sxml_kotlin_measure_pairs(environment, documents) +
                                             sxml_kotlin_measure(environment, base_uri),
                               .used_byte_count = 0};
    sxml_host_schema *schema = sxml_host_schema_create(glue->processor);
    if (schema == NULL || !sxml_kotlin_block_allocate(&call, &block)) {
        sxml_host_schema_destroy(schema);
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        sxml_host_schema_document *document_list = sxml_kotlin_block_take(&block, document_byte_count);
        sxml_kotlin_copy_pairs(
            environment, documents,
            (sxml_kotlin_pair_target){.block = &block, .views = (sxml_byte_view *)document_list});
        sxml_host_schema_compile_request compile_request = {
            .documents = document_list,
            .document_count = documents.pair_count,
            .base_uri = sxml_kotlin_block_copy(environment, &block, base_uri),
            .side_results = {.side_results = NULL, .side_result_count = 0}};
        sxml_kotlin_schema_compile_copied(&call, schema, &compile_request);
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, documents.texts);
    (*environment)->DeleteLocalRef(environment, base_uri);
}

/* Validates with the copied texts and writes the outcome: the validated
   copy, or the error when validation failed (none when the input is only
   invalid), the validity and the reports. */
static void sxml_kotlin_validate_copied(const sxml_kotlin_call *call, sxml_host_schema *schema,
                                        sxml_host_validate_request *validate_request) {
    JNIEnv *environment = call->environment;
    sxml_host_value *result = sxml_host_schema_validate(schema, validate_request);
    if (!(*environment)->ExceptionCheck(environment)) {
        (*environment)->SetIntField(environment, call->request, call->glue->table.validate.validity,
                                    (jint)validate_request->validity);
    }
    const bool has_failed =
        result == NULL && sxml_host_error_of(call->glue->processor)->status != SXML_STATUS_OK;
    const bool is_written =
        result != NULL || has_failed
            ? sxml_kotlin_finish_with_side_results(call, result, validate_request->side_results)
            : sxml_kotlin_write_side_results(call, validate_request->side_results);
    if (!is_written) {
        sxml_host_value_release(result);
    }
}

/* ValidateRequest: validates a node, or XML text parsed and validated in
   one pass (sxml_host_schema_validate). */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_schemaValidate(JNIEnv *environment,
                                                                      jclass owner,
                                                                      jobject request) {
    (void)owner;
    sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_call call = {.environment = environment, .glue = glue, .request = request};
    const sxml_kotlin_validate_fields *fields = &glue->table.validate;
    jbyteArray text = (*environment)->GetObjectField(environment, request, fields->text);
    jbyteArray document_uri = (*environment)->GetObjectField(environment, request, fields->document_uri);
    const sxml_kotlin_text_pairs type = sxml_kotlin_pairs_of(&call, fields->governing_type);
    sxml_kotlin_block block = {
        .memory = NULL,
        .byte_count = sxml_kotlin_measure(environment, text) +
                      sxml_kotlin_measure(environment, document_uri) +
                      sxml_kotlin_rounded(2 * sizeof(sxml_byte_view)) +
                      sxml_kotlin_measure_pairs(environment, type),
        .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        const sxml_byte_view *type_name = sxml_kotlin_take_views(
            environment, &block, (sxml_kotlin_view_request){.pairs = type, .view_count = 2});
        sxml_host_validate_request validate_request = {
            .node = sxml_kotlin_pointer_of(environment, request, fields->node),
            .text = sxml_kotlin_block_copy(environment, &block, text),
            .document_uri = sxml_kotlin_block_copy(environment, &block, document_uri),
            .validation_mode =
                (uint32_t)(*environment)->GetIntField(environment, request, fields->validation_mode),
            .governing_type = {.namespace_uri = type_name[0], .local_name = type_name[1]},
            .validity = 0,
            .side_results = {.side_results = NULL, .side_result_count = 0}};
        sxml_kotlin_validate_copied(&call, sxml_kotlin_pointer_of(environment, request, fields->schema),
                                    &validate_request);
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, text);
    (*environment)->DeleteLocalRef(environment, document_uri);
    (*environment)->DeleteLocalRef(environment, type.texts);
}

/* ------------------------------------------------------------------ Release */

/* ReleaseRequest: releases a value or destroys an expression, a query, a
   stylesheet or a schema. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_release(JNIEnv *environment, jclass owner,
                                                               jobject request) {
    (void)owner;
    const sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_release_fields *fields = &glue->table.release;
    const jint kind = (*environment)->GetIntField(environment, request, fields->kind);
    void *target = sxml_kotlin_pointer_of(environment, request, fields->target);
    switch ((sxml_kotlin_handle_kind)kind) {
    case SXML_KOTLIN_HANDLE_KIND_VALUE:
        sxml_host_value_release(target);
        break;
    case SXML_KOTLIN_HANDLE_KIND_EXPRESSION:
        sxml_host_expression_destroy(target);
        break;
    case SXML_KOTLIN_HANDLE_KIND_QUERY:
        sxml_host_query_destroy(target);
        break;
    case SXML_KOTLIN_HANDLE_KIND_STYLESHEET:
        sxml_host_stylesheet_destroy(target);
        break;
    case SXML_KOTLIN_HANDLE_KIND_SCHEMA:
        sxml_host_schema_destroy(target);
        break;
    }
}
