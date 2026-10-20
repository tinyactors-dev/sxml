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

/* CompileRequest. */
typedef struct sxml_kotlin_compile_fields {
    jfieldID expression_text;
    jfieldID base_uri;
    jfieldID namespaces;
    jfieldID variables;
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

/* The byte arrays of a CompileRequest's array field, in pairs. */
typedef struct sxml_kotlin_text_pairs {
    jobjectArray texts;
    size_t pair_count;
} sxml_kotlin_text_pairs;

static sxml_kotlin_text_pairs sxml_kotlin_pairs_of(const sxml_kotlin_call *call, jfieldID field) {
    JNIEnv *environment = call->environment;
    jobjectArray texts = (*environment)->GetObjectField(environment, call->request, field);
    const size_t text_count =
        texts == NULL ? 0 : (size_t)(*environment)->GetArrayLength(environment, texts);
    return (sxml_kotlin_text_pairs){.texts = texts, .pair_count = text_count / 2};
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

/* CompileRequest: compiles an XPath expression with its namespaces and
   external variables (sxml_host_compile). */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_compile(JNIEnv *environment, jclass owner,
                                                               jobject request) {
    (void)owner;
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
        const sxml_byte_view expression_view =
            sxml_kotlin_block_copy(environment, &block, expression_text);
        const sxml_byte_view base_uri_view = sxml_kotlin_block_copy(environment, &block, base_uri);
        sxml_kotlin_copy_pairs(
            environment, namespaces,
            (sxml_kotlin_pair_target){.block = &block, .views = (sxml_byte_view *)namespace_list});
        sxml_kotlin_copy_pairs(
            environment, variables,
            (sxml_kotlin_pair_target){.block = &block, .views = (sxml_byte_view *)variable_list});
        sxml_host_expression *expression = sxml_host_compile(
            glue->processor, &(sxml_host_compile_request){.expression_text = expression_view,
                                                          .static_base_uri = base_uri_view,
                                                          .namespaces = namespace_list,
                                                          .namespace_count = namespaces.pair_count,
                                                          .variables = variable_list,
                                                          .variable_count = variables.pair_count});
        if (!sxml_kotlin_finish(&call, expression)) {
            sxml_host_expression_destroy(expression);
        }
    }
    sxml_host_release(glue->processor, block.memory);
    (*environment)->DeleteLocalRef(environment, expression_text);
    (*environment)->DeleteLocalRef(environment, base_uri);
    (*environment)->DeleteLocalRef(environment, namespaces.texts);
    (*environment)->DeleteLocalRef(environment, variables.texts);
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
    sxml_host_expression *expression = (sxml_host_expression *)(intptr_t)(*environment)->GetLongField(
        environment, request, fields->expression);
    const sxml_host_value *context = (const sxml_host_value *)(intptr_t)(*environment)->GetLongField(
        environment, request, fields->context);
    jlongArray handles = (*environment)->GetObjectField(environment, request, fields->variable_values);
    const jsize value_count = handles == NULL ? 0 : (*environment)->GetArrayLength(environment, handles);
    sxml_kotlin_block block = {
        .memory = NULL,
        .byte_count = sxml_kotlin_rounded((size_t)value_count * sizeof(void *)) +
                      sxml_kotlin_rounded((size_t)value_count * sizeof(jlong)),
        .used_byte_count = 0};
    if (!sxml_kotlin_block_allocate(&call, &block)) {
        sxml_kotlin_write_out_of_memory(environment, request, &glue->table);
    } else {
        const sxml_host_value **values =
            (const sxml_host_value **)sxml_kotlin_block_take(&block, (size_t)value_count * sizeof(void *));
        jlong *value_handles = sxml_kotlin_block_take(&block, (size_t)value_count * sizeof(jlong));
        if (value_count > 0) {
            (*environment)->GetLongArrayRegion(environment, handles, 0, value_count, value_handles);
        }
        for (jsize value_index = 0; value_index < value_count; value_index += 1) {
            values[value_index] = (const sxml_host_value *)(intptr_t)value_handles[value_index];
        }
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
        .value = (const sxml_host_value *)(intptr_t)(*environment)->GetLongField(environment, request,
                                                                                 fields->value),
        .options = (const sxml_host_value *)(intptr_t)(*environment)->GetLongField(environment, request,
                                                                                   fields->options)};
    if (sxml_host_serialize(glue->processor, &serialize_request) != SXML_STATUS_OK) {
        sxml_kotlin_write_error(&call);
        return;
    }
    sxml_kotlin_set_bytes(environment,
                          (sxml_kotlin_byte_field){.owner = request, .field = fields->output},
                          *sxml_host_output_of(glue->processor));
}

/* ReleaseRequest: releases a value or destroys an expression. */
JNIEXPORT void JNICALL Java_dev_tinyactors_sxml_Native_release(JNIEnv *environment, jclass owner,
                                                               jobject request) {
    (void)owner;
    const sxml_kotlin_processor *glue = sxml_kotlin_glue_of(environment, request);
    if (glue == NULL) {
        return;
    }
    const sxml_kotlin_release_fields *fields = &glue->table.release;
    const jint kind = (*environment)->GetIntField(environment, request, fields->kind);
    void *target = (void *)(intptr_t)(*environment)->GetLongField(environment, request, fields->target);
    switch ((sxml_kotlin_handle_kind)kind) {
    case SXML_KOTLIN_HANDLE_KIND_VALUE:
        sxml_host_value_release(target);
        break;
    case SXML_KOTLIN_HANDLE_KIND_EXPRESSION:
        sxml_host_expression_destroy(target);
        break;
    }
}
