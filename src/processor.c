/*
 * The processor: clocks, resource resolvers, the processor's lifecycle,
 * document parsing and the document loader of an evaluation
 * (include/sxml/processor.h, src/processor_internal.h; docs/DECISIONS.md,
 * D30, D51, D88).
 *
 * Golden file: src/memory.c (layout, lifecycle, comments). The processor's
 * memory holds, in this order, the processor, its name pool, type
 * registry, item heap and function library (each placed with its own
 * initialize), so the whole object can live in one caller block; what they
 * allocate as they grow comes from the configuration's allocator.
 */
#include "processor_internal.h"

#include "calendar_internal.h"
#include "cast_internal.h"
#include "dtd_internal.h"
#include "function_library_parts_internal.h"
#include "memory_internal.h"
#include "schema_scope_internal.h"
#include "source_code_internal.h"
#include "sxml/tree.h"
#include "sxml/xml.h"
#include "xdm_internal.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------- Clock */

/* The minutes east of UTC a timezone may have (XDM 4.0, section 5.4). */
enum { SXML_PROCESSOR_MAXIMUM_TIMEZONE_MINUTES = 840 };

enum { SXML_PROCESSOR_NANOSECONDS_PER_SECOND = 1000000000 };

static bool sxml_timezone_offset_is_valid(int16_t offset_minutes) {
    return offset_minutes >= -SXML_PROCESSOR_MAXIMUM_TIMEZONE_MINUTES &&
           offset_minutes <= SXML_PROCESSOR_MAXIMUM_TIMEZONE_MINUTES;
}

/* The contract of sxml_clock_reading: a dateTime with a timezone whose
   components are in range, and a valid implicit timezone. */
static bool sxml_clock_reading_is_valid(const sxml_clock_reading *reading) {
    const sxml_date_time *instant = &reading->current_date_time;
    if (!instant->has_timezone ||
        !sxml_timezone_offset_is_valid(instant->timezone_offset_minutes) ||
        !sxml_timezone_offset_is_valid(
            reading->implicit_timezone_offset_minutes)) {
        return false;
    }
    if (instant->month < 1 || instant->month > 12 || instant->day < 1 ||
        instant->day > sxml_days_in_month(instant->year, instant->month)) {
        return false;
    }
    return instant->hour < 24 && instant->minute < 60 && instant->second < 60 &&
           instant->nanosecond < SXML_PROCESSOR_NANOSECONDS_PER_SECOND;
}

sxml_status sxml_clock_read(sxml_clock clock, sxml_clock_reading *reading) {
    if (clock.operations == NULL || clock.operations->read == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_clock_reading clock_reading;
    memset(&clock_reading, 0, sizeof(clock_reading));
    const sxml_status status = clock.operations->read(clock.state, &clock_reading);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_clock_reading_is_valid(&clock_reading)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *reading = clock_reading;
    return SXML_STATUS_OK;
}

sxml_date_time sxml_date_time_from_unix_time(sxml_unix_time unix_time) {
    const int64_t seconds_per_day = 86400;
    int64_t day_number = unix_time.seconds / seconds_per_day;
    int64_t second_of_day = unix_time.seconds % seconds_per_day;
    if (second_of_day < 0) {
        second_of_day += seconds_per_day;
        day_number -= 1;
    }
    /* The civil date of a day number counted from 1970-01-01, in the
       proleptic Gregorian calendar. */
    const sxml_calendar_date date = sxml_calendar_gregorian_date(day_number);
    return (sxml_date_time){.year = date.year,
                            .month = (uint8_t)date.month,
                            .day = (uint8_t)date.day,
                            .hour = (uint8_t)(second_of_day / 3600),
                            .minute = (uint8_t)(second_of_day / 60 % 60),
                            .second = (uint8_t)(second_of_day % 60),
                            .nanosecond = unix_time.nanoseconds,
                            .has_timezone = true,
                            .timezone_offset_minutes = 0};
}

/*
 * Reads the platform's UTC clock into now; false when it has none (C17,
 * section 7.27.2.5, timespec_get). Android's bionic declares timespec_get
 * only from API level 29; below it, clock_gettime with CLOCK_REALTIME
 * (POSIX.1-2008) reads the same clock (minSdk 24, D238).
 */
static bool sxml_platform_utc_time(struct timespec *now) {
#if defined(__ANDROID__) && __ANDROID_API__ < 29
    return clock_gettime(CLOCK_REALTIME, now) == 0;
#elif defined(TIME_UTC)
    return timespec_get(now, TIME_UTC) == TIME_UTC;
#else
    (void)now;
    return false;
#endif
}

static sxml_status sxml_system_clock_read(void *state, sxml_clock_reading *reading) {
    (void)state;
    struct timespec now;
    if (!sxml_platform_utc_time(&now)) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    *reading = (sxml_clock_reading){
        .current_date_time = sxml_date_time_from_unix_time(
            (sxml_unix_time){.seconds = (int64_t)now.tv_sec,
                             .nanoseconds = (uint32_t)now.tv_nsec}),
        .implicit_timezone_offset_minutes = 0};
    return SXML_STATUS_OK;
}

static const sxml_clock_operations sxml_system_clock_operations = {
    .read = sxml_system_clock_read,
};

sxml_clock sxml_system_clock(void) {
    return (sxml_clock){.state = NULL, .operations = &sxml_system_clock_operations};
}

static sxml_status sxml_fixed_clock_read(void *state, sxml_clock_reading *reading) {
    *reading = *(const sxml_clock_reading *)state;
    return SXML_STATUS_OK;
}

static const sxml_clock_operations sxml_fixed_clock_operations = {
    .read = sxml_fixed_clock_read,
};

sxml_clock sxml_fixed_clock(const sxml_clock_reading *reading) {
    /* The state is only read: the cast drops const for the interface. */
    return (sxml_clock){.state = (void *)reading,
                        .operations = &sxml_fixed_clock_operations};
}

/* ------------------------------------------------------ Resource resolver */

/* The table resolver: serves the entry whose URI is the request's absolute
   URI, or its URI reference when there is none. */
static sxml_status sxml_table_resolver_resolve(void *state,
                                               const sxml_resource_request *request,
                                               sxml_resource *resource) {
    const sxml_resource_entry_view *entries = state;
    const sxml_byte_view key = request->absolute_uri.byte_count > 0
                                   ? request->absolute_uri
                                   : request->uri_reference;
    for (size_t entry_index = 0; entry_index < entries->resource_entry_count;
         entry_index += 1) {
        const sxml_resource_entry *entry = &entries->resource_entries[entry_index];
        if (sxml_byte_view_equals(entry->uri, key)) {
            resource->resource_form = SXML_RESOURCE_FORM_BYTES;
            resource->content = entry->content;
            resource->resource_uri = key;
            return SXML_STATUS_OK;
        }
    }
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    resource->description = SXML_BYTE_VIEW_FROM_LITERAL("No such resource in the table");
    return SXML_STATUS_OK;
}

static const sxml_resource_resolver_operations sxml_table_resolver_operations = {
    .resolve = sxml_table_resolver_resolve,
    .release = NULL,
};

sxml_resource_resolver sxml_table_resource_resolver(
    const sxml_resource_entry_view *resource_entries) {
    /* The state is only read: the cast drops const for the interface. */
    return (sxml_resource_resolver){.state = (void *)resource_entries,
                                    .operations = &sxml_table_resolver_operations};
}

/* ------------------------------------------------------------- Processor */

/* Where each member lives in the processor's memory. */
typedef struct sxml_processor_layout {
    sxml_memory_requirements requirements;
    size_t name_pool_offset;
    size_t type_registry_offset;
    size_t item_heap_offset;
    size_t function_library_offset;
} sxml_processor_layout;

/* Every library part serving XPath, as CMake registers them (D45, D101):
   static data, so the processor's configuration may borrow it. */
static sxml_native_function_view_list sxml_processor_library_parts(void) {
    return *sxml_function_library_xpath_parts();
}

/* The library functions of the processor: every registered XPath part,
   then those of the optional features its configuration names, in kind
   order (D191). The function library does not keep its parts list, so
   the list lives in allocator memory only while the library is
   initialized. */
typedef struct sxml_processor_library_part_list {
    sxml_native_function_view_list parts;
    /* NULL when no feature adds functions: parts is then the static
       registry's list. */
    const sxml_native_function_view **allocated_views;
} sxml_processor_library_part_list;

static sxml_status sxml_processor_make_library_part_list(
    const sxml_processor *processor, sxml_processor_library_part_list *list) {
    const sxml_native_function_view_list registered = sxml_processor_library_parts();
    *list = (sxml_processor_library_part_list){.parts = registered, .allocated_views = NULL};
    size_t feature_part_count = 0;
    for (size_t kind_index = 0; kind_index < (size_t)SXML_OPTIONAL_FEATURE_KIND_COUNT;
         kind_index += 1) {
        const sxml_optional_feature_definition *definition =
            processor->optional_features.slots[kind_index].definition;
        if (definition != NULL && definition->library_functions != NULL) {
            feature_part_count += 1;
        }
    }
    if (feature_part_count == 0) {
        return SXML_STATUS_OK;
    }
    const size_t view_count = registered.native_function_view_count + feature_part_count;
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(view_count, sizeof(const sxml_native_function_view *), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            processor->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(const sxml_native_function_view *)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_native_function_view **views = (const sxml_native_function_view **)memory;
    size_t filled = 0;
    for (size_t part_index = 0; part_index < registered.native_function_view_count;
         part_index += 1) {
        views[filled] = registered.native_function_views[part_index];
        filled += 1;
    }
    for (size_t kind_index = 0; kind_index < (size_t)SXML_OPTIONAL_FEATURE_KIND_COUNT;
         kind_index += 1) {
        const sxml_optional_feature_definition *definition =
            processor->optional_features.slots[kind_index].definition;
        if (definition != NULL && definition->library_functions != NULL) {
            views[filled] = definition->library_functions;
            filled += 1;
        }
    }
    *list = (sxml_processor_library_part_list){
        .parts = {.native_function_views = views, .native_function_view_count = filled},
        .allocated_views = views};
    return SXML_STATUS_OK;
}

static bool sxml_processor_configuration_is_valid(
    const sxml_processor_configuration *configuration) {
    return configuration != NULL && !sxml_allocator_is_null(configuration->allocator) &&
           sxml_optional_feature_view_is_valid(configuration->optional_features) &&
           sxml_source_code_language_view_is_valid(configuration->source_code_languages) &&
           (configuration->source_location_policy == SXML_SOURCE_LOCATION_POLICY_AUTOMATIC ||
            configuration->source_location_policy == SXML_SOURCE_LOCATION_POLICY_ALWAYS ||
            configuration->source_location_policy == SXML_SOURCE_LOCATION_POLICY_NEVER);
}

static sxml_name_pool_configuration sxml_processor_name_pool_configuration(
    const sxml_processor_configuration *configuration) {
    return (sxml_name_pool_configuration){
        .allocator = configuration->allocator,
        .maximum_byte_count = configuration->limits.maximum_name_pool_byte_count};
}

static sxml_type_registry_configuration sxml_processor_type_registry_configuration(
    const sxml_processor_configuration *configuration, sxml_name_pool *name_pool) {
    return (sxml_type_registry_configuration){
        .allocator = configuration->allocator,
        .name_pool = name_pool,
        .maximum_registered_type_count =
            configuration->limits.maximum_registered_type_count};
}

/* The members' configurations refer to one another by pointer; their
   memory requirements only check that the pointers are not NULL, so the
   layout is computed with stand-ins (pointers to an object of the strictest
   fundamental alignment, so the conversions are defined; never
   dereferenced). */
typedef struct sxml_processor_stand_ins {
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
} sxml_processor_stand_ins;

static sxml_status sxml_processor_compute_layout(
    const sxml_processor_configuration *configuration, sxml_processor_layout *layout) {
    if (!sxml_processor_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    max_align_t stand_in_object;
    const sxml_processor_stand_ins stand_ins = {
        .name_pool = (sxml_name_pool *)&stand_in_object,
        .type_registry = (sxml_type_registry *)&stand_in_object,
        .item_heap = (sxml_item_heap *)&stand_in_object};
    const sxml_name_pool_configuration name_pool_configuration =
        sxml_processor_name_pool_configuration(configuration);
    const sxml_type_registry_configuration type_registry_configuration =
        sxml_processor_type_registry_configuration(configuration, stand_ins.name_pool);
    const sxml_item_heap_configuration item_heap_configuration = {
        .allocator = configuration->allocator,
        .name_pool = stand_ins.name_pool,
        .type_registry = stand_ins.type_registry,
        .maximum_byte_count = configuration->limits.maximum_item_heap_byte_count};
    const sxml_function_library_configuration function_library_configuration = {
        .allocator = configuration->allocator,
        .item_heap = stand_ins.item_heap,
        .parts = sxml_processor_library_parts()};
    sxml_memory_requirements member = {.byte_count = 0, .alignment = 1};
    sxml_processor_layout result = {
        .requirements = {.byte_count = sizeof(sxml_processor),
                         .alignment = alignof(sxml_processor)},
        .name_pool_offset = 0,
        .type_registry_offset = 0,
        .item_heap_offset = 0,
        .function_library_offset = 0};
    sxml_status status = sxml_name_pool_memory_requirements(&name_pool_configuration, &member);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(&result.requirements, member,
                                                 &result.name_pool_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_memory_requirements(&type_registry_configuration, &member);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(&result.requirements, member,
                                                 &result.type_registry_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_memory_requirements(&item_heap_configuration, &member);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(&result.requirements, member,
                                                 &result.item_heap_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_memory_requirements(&function_library_configuration,
                                                           &member);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(&result.requirements, member,
                                                 &result.function_library_offset);
    }
    if (status == SXML_STATUS_OK) {
        *layout = result;
    }
    return status;
}

sxml_status sxml_processor_memory_requirements(
    const sxml_processor_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_processor_layout layout;
    const sxml_status status = sxml_processor_compute_layout(configuration, &layout);
    if (status == SXML_STATUS_OK) {
        *requirements = layout.requirements;
    }
    return status;
}

/* The slice of memory at offset with the member's requirements. */
typedef struct sxml_processor_member_slice {
    sxml_memory_view memory;
    size_t offset;
    size_t end_offset;
} sxml_processor_member_slice;

static sxml_memory_view sxml_processor_slice(const sxml_processor_member_slice *slice) {
    sxml_memory_view member = {.bytes = NULL, .byte_count = 0};
    /* The layout lies within memory, which was checked against it. */
    if (sxml_memory_view_slice(slice->memory,
                               (sxml_byte_range){.byte_offset = slice->offset,
                                                 .byte_count = slice->end_offset -
                                                               slice->offset},
                               &member) != SXML_STATUS_OK) {
        return (sxml_memory_view){.bytes = NULL, .byte_count = 0};
    }
    return member;
}

/* Places the members in memory, finalizing those placed when one fails. */
static sxml_status sxml_processor_place_members(sxml_processor *processor,
                                                const sxml_processor_layout *layout,
                                                sxml_memory_view memory) {
    const sxml_processor_configuration configuration = {
        .allocator = processor->allocator,
        .limits = processor->limits,
        .resource_resolver = processor->resource_resolver,
        .clock = processor->clock,
        .trace_sink = processor->trace_sink};
    const sxml_name_pool_configuration name_pool_configuration =
        sxml_processor_name_pool_configuration(&configuration);
    sxml_status status = sxml_name_pool_initialize(
        sxml_processor_slice(&(sxml_processor_member_slice){
            .memory = memory,
            .offset = layout->name_pool_offset,
            .end_offset = layout->type_registry_offset}),
        &name_pool_configuration, &processor->name_pool);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_type_registry_configuration type_registry_configuration =
        sxml_processor_type_registry_configuration(&configuration, processor->name_pool);
    status = sxml_type_registry_initialize(
        sxml_processor_slice(&(sxml_processor_member_slice){
            .memory = memory,
            .offset = layout->type_registry_offset,
            .end_offset = layout->item_heap_offset}),
        &type_registry_configuration, &processor->type_registry);
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_initialize(
            sxml_processor_slice(&(sxml_processor_member_slice){
                .memory = memory,
                .offset = layout->item_heap_offset,
                .end_offset = layout->function_library_offset}),
            &(sxml_item_heap_configuration){
                .allocator = processor->allocator,
                .name_pool = processor->name_pool,
                .type_registry = processor->type_registry,
                .maximum_byte_count = processor->limits.maximum_item_heap_byte_count},
            &processor->item_heap);
        if (status != SXML_STATUS_OK) {
            sxml_type_registry_finalize(processor->type_registry);
        }
    }
    sxml_processor_library_part_list part_list = {
        .parts = {.native_function_views = NULL, .native_function_view_count = 0},
        .allocated_views = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_make_library_part_list(processor, &part_list);
        if (status != SXML_STATUS_OK) {
            sxml_item_heap_finalize(processor->item_heap);
            sxml_type_registry_finalize(processor->type_registry);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_initialize(
            sxml_processor_slice(&(sxml_processor_member_slice){
                .memory = memory,
                .offset = layout->function_library_offset,
                .end_offset = layout->requirements.byte_count}),
            &(sxml_function_library_configuration){.allocator = processor->allocator,
                                                   .item_heap = processor->item_heap,
                                                   .parts = part_list.parts},
            &processor->function_library);
        sxml_allocator_release(processor->allocator, (void *)part_list.allocated_views);
        if (status != SXML_STATUS_OK) {
            sxml_item_heap_finalize(processor->item_heap);
            sxml_type_registry_finalize(processor->type_registry);
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_name_pool_finalize(processor->name_pool);
    }
    return status;
}

sxml_status sxml_processor_initialize(sxml_memory_view memory,
                                      const sxml_processor_configuration *configuration,
                                      sxml_processor **processor) {
    sxml_processor_layout layout;
    sxml_status status = sxml_processor_compute_layout(configuration, &layout);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, layout.requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_processor *new_processor = placement;
    *new_processor = (sxml_processor){
        .allocator = configuration->allocator,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .limits = configuration->limits,
        .resource_resolver = configuration->resource_resolver,
        .clock = configuration->clock.operations == NULL ? sxml_system_clock()
                                                         : configuration->clock,
        .trace_sink = configuration->trace_sink,
        .name_pool = NULL,
        .type_registry = NULL,
        .item_heap = NULL,
        .function_library = NULL,
        .kept_schemas = NULL,
        .kept_schema_count = 0,
        .kept_schema_capacity = 0,
        .builtin_schema = NULL,
        .function_namespace_schema = NULL,
        .optional_features =
            sxml_optional_feature_slots_from_view(configuration->optional_features),
        .file_system = configuration->file_system,
        .reads_query_text_as_xml_1_1 = configuration->reads_query_text_as_xml_1_1,
        .source_code_languages = configuration->source_code_languages,
        .source_location_policy = configuration->source_location_policy,
        .location_demand = {.has_compiled_program = false,
                            .has_reading_program = false,
                            .is_evaluating = false,
                            .evaluation_reads_locations = false}};
    status = sxml_processor_place_members(new_processor, &layout, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *processor = new_processor;
    return SXML_STATUS_OK;
}

void sxml_processor_finalize(sxml_processor *processor) {
    if (processor == NULL) {
        return;
    }
    for (size_t schema_index = 0; schema_index < processor->kept_schema_count;
         schema_index += 1) {
        sxml_schema_destroy(processor->kept_schemas[schema_index]);
    }
    sxml_allocator_release(processor->allocator, (void *)processor->kept_schemas);
    processor->kept_schemas = NULL;
    processor->kept_schema_count = 0;
    processor->kept_schema_capacity = 0;
    sxml_schema_destroy(processor->builtin_schema);
    processor->builtin_schema = NULL;
    sxml_schema_destroy(processor->function_namespace_schema);
    processor->function_namespace_schema = NULL;
    sxml_optional_feature_slots_finalize(&processor->optional_features, processor->allocator);
    sxml_function_library_finalize(processor->function_library);
    sxml_item_heap_finalize(processor->item_heap);
    sxml_type_registry_finalize(processor->type_registry);
    sxml_name_pool_finalize(processor->name_pool);
}

sxml_status sxml_processor_keep_schema(sxml_processor *processor, sxml_schema *schema) {
    if (processor == NULL || schema == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (processor->kept_schema_count == processor->kept_schema_capacity) {
        const size_t capacity =
            processor->kept_schema_capacity == 0 ? 4 : processor->kept_schema_capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(sxml_schema *), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_schema *)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (processor->kept_schema_count > 0) {
            memcpy(memory, (void *)processor->kept_schemas,
                   processor->kept_schema_count * sizeof(sxml_schema *));
        }
        sxml_allocator_release(processor->allocator, (void *)processor->kept_schemas);
        processor->kept_schemas = (sxml_schema **)memory;
        processor->kept_schema_capacity = capacity;
    }
    processor->kept_schemas[processor->kept_schema_count] = schema;
    processor->kept_schema_count += 1;
    return SXML_STATUS_OK;
}

sxml_status sxml_processor_get_builtin_schema(sxml_processor *processor,
                                              const sxml_schema **schema) {
    if (processor->builtin_schema == NULL) {
        /* The processor's own schema is no validation a host asked for:
           it leaves the location demand as it was (D318). */
        const sxml_location_demand demand = processor->location_demand;
        const sxml_status status = sxml_schema_create(
            processor->allocator,
            &(sxml_schema_configuration){
                .processor = processor,
                .schema_documents = {.schema_documents = NULL, .schema_document_count = 0},
                .base_uri = {.bytes = NULL, .byte_count = 0},
                .report_sink = {.state = NULL, .operations = NULL},
                .maximum_content_model_state_count = 0,
                .error_record = NULL},
            &processor->builtin_schema);
        processor->location_demand = demand;
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *schema = processor->builtin_schema;
    return SXML_STATUS_OK;
}

sxml_status sxml_processor_get_function_namespace_schema(sxml_processor *processor,
                                                         const sxml_schema **schema) {
    if (processor->function_namespace_schema == NULL) {
        sxml_schema_document documents[SXML_FUNCTION_NAMESPACE_DOCUMENT_COUNT];
        for (size_t document_index = 0; document_index < SXML_FUNCTION_NAMESPACE_DOCUMENT_COUNT;
             document_index += 1) {
            documents[document_index] = (sxml_schema_document){
                .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                .document_text = sxml_schema_function_namespace_document(document_index),
                .document_uri = {.bytes = NULL, .byte_count = 0},
                .checks_target_namespace = false,
                .target_namespace_uri = {.bytes = NULL, .byte_count = 0}};
        }
        const sxml_location_demand demand = processor->location_demand;
        const sxml_status status = sxml_schema_create(
            processor->allocator,
            &(sxml_schema_configuration){
                .processor = processor,
                .schema_documents = {.schema_documents = documents,
                                     .schema_document_count =
                                         SXML_FUNCTION_NAMESPACE_DOCUMENT_COUNT},
                .base_uri = {.bytes = NULL, .byte_count = 0},
                .report_sink = {.state = NULL, .operations = NULL},
                .maximum_content_model_state_count = 0,
                .error_record = NULL},
            &processor->function_namespace_schema);
        processor->location_demand = demand;
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *schema = processor->function_namespace_schema;
    return SXML_STATUS_OK;
}

sxml_status sxml_processor_create(sxml_allocator allocator,
                                  const sxml_processor_configuration *configuration,
                                  sxml_processor **processor) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_processor_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_processor *new_processor = NULL;
    status = sxml_processor_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &new_processor);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_processor->creating_allocator = allocator;
    *processor = new_processor;
    return SXML_STATUS_OK;
}

void sxml_processor_destroy(sxml_processor *processor) {
    if (processor == NULL) {
        return;
    }
    const sxml_allocator allocator = processor->creating_allocator;
    sxml_processor_finalize(processor);
    sxml_allocator_release(allocator, processor);
}

sxml_item_heap *sxml_processor_get_item_heap(const sxml_processor *processor) {
    return processor->item_heap;
}

sxml_name_pool *sxml_processor_get_name_pool(const sxml_processor *processor) {
    return processor->name_pool;
}

sxml_type_registry *sxml_processor_get_type_registry(const sxml_processor *processor) {
    return processor->type_registry;
}

sxml_function_table sxml_processor_get_function_table(const sxml_processor *processor) {
    return sxml_function_library_as_table(processor->function_library);
}

uint32_t sxml_processor_get_maximum_evaluation_depth(const sxml_processor *processor) {
    return processor->limits.maximum_evaluation_depth == 0
               ? SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH
               : processor->limits.maximum_evaluation_depth;
}

uint64_t sxml_processor_get_maximum_evaluation_step_count(const sxml_processor *processor) {
    return processor->limits.maximum_evaluation_step_count == 0
               ? SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_STEP_COUNT
               : processor->limits.maximum_evaluation_step_count;
}

bool sxml_processor_reads_query_text_as_xml_1_1(const sxml_processor *processor) {
    return processor != NULL && processor->reads_query_text_as_xml_1_1;
}

uint32_t sxml_processor_get_maximum_expression_depth(const sxml_processor *processor) {
    return processor->limits.maximum_expression_depth == 0
               ? SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH
               : processor->limits.maximum_expression_depth;
}

/* ------------------------------------------------------ Source locations */

void sxml_processor_note_program(sxml_processor *processor, bool reads_source_locations) {
    processor->location_demand.has_compiled_program = true;
    processor->location_demand.has_reading_program =
        processor->location_demand.has_reading_program || reads_source_locations;
}

bool sxml_processor_keeps_locations(const sxml_processor *processor) {
    const sxml_location_demand demand = processor->location_demand;
    switch (processor->source_location_policy) {
    case SXML_SOURCE_LOCATION_POLICY_ALWAYS:
        return true;
    case SXML_SOURCE_LOCATION_POLICY_NEVER:
        return false;
    case SXML_SOURCE_LOCATION_POLICY_AUTOMATIC:
        break;
    }
    /* D313 (7): a document the host parses before any program is
       compiled keeps its locations; after that, and during an
       evaluation, the programs decide. */
    if (demand.is_evaluating) {
        return demand.evaluation_reads_locations;
    }
    return !demand.has_compiled_program || demand.has_reading_program;
}

bool sxml_processor_unit_reads_source_locations(const sxml_processor *processor,
                                                const sxml_ir_unit *unit) {
    if (unit->reads_source_locations) {
        return true;
    }
    /* A unit with in-scope schemas beyond the processor's own can
       validate (XQuery's import schema, xsl:import-schema, the host's),
       and a validation reads source locations (D313). */
    const sxml_schema_view schemas = unit->static_context.in_scope_schemas;
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        const sxml_schema *schema = schemas.schemas[schema_index];
        if (schema != processor->builtin_schema &&
            schema != processor->function_namespace_schema) {
            return true;
        }
    }
    return false;
}

sxml_location_demand sxml_processor_begin_evaluation(sxml_processor *processor,
                                                     bool reads_source_locations) {
    const sxml_location_demand previous = processor->location_demand;
    processor->location_demand.is_evaluating = true;
    /* A nested evaluation (fn:transform, fn:load-xquery-module, a host
       function's) may hand its documents to the one it runs inside. */
    processor->location_demand.evaluation_reads_locations =
        reads_source_locations ||
        (previous.is_evaluating && previous.evaluation_reads_locations);
    return previous;
}

void sxml_processor_end_evaluation(sxml_processor *processor, sxml_location_demand previous) {
    /* Programs compiled during the evaluation stay noted. */
    processor->location_demand.is_evaluating = previous.is_evaluating;
    processor->location_demand.evaluation_reads_locations = previous.evaluation_reads_locations;
}

/* ------------------------------------------------------ Parsing documents */

sxml_status sxml_processor_parse_document(sxml_processor *processor,
                                          const sxml_xml_parse_request *request,
                                          sxml_item *document_node) {
    if (processor == NULL || request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator allocator = processor->allocator;
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        allocator,
        &(sxml_xml_parser_configuration){.allocator = allocator,
                                         .name_pool = processor->name_pool,
                                         .limits = processor->limits.xml_limits},
        &parser);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        /* The tree destroys itself with its last reference (D39), as every
           tree an evaluation reads does. */
        status = sxml_arena_tree_create(
            allocator,
            &(sxml_arena_tree_configuration){
                .allocator = allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = processor->item_heap,
                .maximum_node_count = processor->limits.maximum_tree_node_count,
                .maximum_depth = processor->limits.xml_limits.maximum_depth,
                .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
                .keeps_locations = sxml_processor_keeps_locations(processor),
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = NULL},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(parser, request,
                                       sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_xml_parser_destroy(parser);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node = sxml_node_item_make((sxml_node_reference){
        .tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* -------------------------------------------------- External entities */

/* Asks the resolution's resource resolver for an external entity, or
   records the reference when it refuses (D199). */
static sxml_status sxml_resolution_resolve_entity(void *state,
                                                  const sxml_xml_external_entity_request *request,
                                                  sxml_xml_external_entity *entity) {
    sxml_entity_resolution *resolution = state;
    const sxml_resource_resolver resolver = resolution->resolver;
    if (resolution->refuses) {
        resolution->was_refused = true;
        return SXML_STATUS_OK;
    }
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    sxml_status status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_EXTERNAL_ENTITY,
                                 .uri_reference = request->system_identifier,
                                 .absolute_uri = request->absolute_uri,
                                 .processor = resolution->processor},
        &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE) {
        sxml_item_release(resource.document_node);
    } else if (resource.resource_form == SXML_RESOURCE_FORM_SEQUENCE) {
        sxml_sequence_release(resource.items);
    }
    if (resource.resource_form != SXML_RESOURCE_FORM_BYTES) {
        /* Not available in a form an entity has (include/sxml/processor.h). */
        if (resolver.operations->release != NULL) {
            resolver.operations->release(resolver.state, &resource);
        }
        return SXML_STATUS_OK;
    }
    resolution->resource = resource;
    resolution->holds_resource = true;
    *entity = (sxml_xml_external_entity){.is_available = true,
                                         .content = resource.content,
                                         .encoding_name = resource.encoding_name,
                                         .entity_uri = resource.resource_uri};
    return SXML_STATUS_OK;
}

static void sxml_resolution_release_entity(void *state, const sxml_xml_external_entity *entity) {
    (void)entity;
    sxml_entity_resolution *resolution = state;
    const sxml_resource_resolver resolver = resolution->resolver;
    if (resolution->holds_resource && resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resolution->resource);
    }
    resolution->holds_resource = false;
}

static const sxml_xml_entity_resolver_operations sxml_entity_resolution_operations = {
    .resolve = sxml_resolution_resolve_entity,
    .release = sxml_resolution_release_entity,
};

sxml_xml_entity_resolver sxml_entity_resolution_as_resolver(sxml_entity_resolution *resolution) {
    return (sxml_xml_entity_resolver){.state = resolution,
                                      .operations = &sxml_entity_resolution_operations};
}

sxml_xml_entity_resolver sxml_processor_entity_resolver(sxml_processor *processor) {
    processor->entity_resolution = (sxml_entity_resolution){
        .processor = processor,
        .resolver = processor->resource_resolver,
        .refuses = false,
        .was_refused = false,
        .holds_resource = false};
    return sxml_entity_resolution_as_resolver(&processor->entity_resolution);
}

/* ------------------------------------------------------- Document loader */

static const sxml_static_error sxml_document_not_available_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No document is available at the URI (F&O 4.0, fn:doc)"),
};

static const sxml_static_error sxml_document_not_well_formed_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The resource at the URI is not a well-formed XML document (F&O 4.0, "
        "fn:doc)"),
};

static const sxml_static_error sxml_document_not_a_document_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The resolver delivered a node that is not a document node (F&O 4.0, "
        "fn:doc)"),
};

static const sxml_static_error sxml_parse_xml_not_well_formed_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The string is not a well-formed XML document (F&O 4.0, fn:parse-xml)"),
};

sxml_status sxml_processor_document_loader_memory_requirements(
    const sxml_processor_document_loader_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->evaluation_environment.processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements =
        (sxml_memory_requirements){.byte_count = sizeof(sxml_processor_document_loader),
                                   .alignment = alignof(sxml_processor_document_loader)};
    return SXML_STATUS_OK;
}

sxml_status sxml_processor_document_loader_initialize(
    sxml_memory_view memory,
    const sxml_processor_document_loader_configuration *configuration,
    sxml_processor_document_loader **loader) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_processor_document_loader_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_processor_document_loader *new_loader = placement;
    *new_loader = (sxml_processor_document_loader){.configuration = *configuration,
                                                   .loaded_documents = NULL,
                                                   .loaded_document_count = 0,
                                                   .loaded_document_capacity = 0,
                                                   .loaded_collections = NULL,
                                                   .loaded_collection_count = 0,
                                                   .loaded_collection_capacity = 0};
    *loader = new_loader;
    return SXML_STATUS_OK;
}

void sxml_processor_document_loader_finalize(sxml_processor_document_loader *loader) {
    if (loader == NULL) {
        return;
    }
    const sxml_allocator allocator = loader->configuration.evaluation_environment.processor->allocator;
    for (size_t document_index = 0; document_index < loader->loaded_document_count;
         document_index += 1) {
        sxml_processor_loaded_document *document = &loader->loaded_documents[document_index];
        sxml_allocator_release(allocator, document->uri_bytes);
        sxml_item_release(document->document_node);
    }
    sxml_allocator_release(allocator, loader->loaded_documents);
    loader->loaded_documents = NULL;
    loader->loaded_document_count = 0;
    loader->loaded_document_capacity = 0;
    for (size_t collection_index = 0; collection_index < loader->loaded_collection_count;
         collection_index += 1) {
        sxml_processor_loaded_collection *collection =
            &loader->loaded_collections[collection_index];
        sxml_allocator_release(allocator, collection->uri_bytes);
        sxml_sequence_release(collection->items);
    }
    sxml_allocator_release(allocator, loader->loaded_collections);
    loader->loaded_collections = NULL;
    loader->loaded_collection_count = 0;
    loader->loaded_collection_capacity = 0;
}

/* The document loaded or available for uri, borrowed; found is false when
   there is none. */
typedef struct sxml_document_lookup {
    bool is_found;
    sxml_item document_node;
} sxml_document_lookup;

/* The available document of uri (the host's, as it supplied it). */
static sxml_document_lookup
sxml_loader_find_available_document(const sxml_processor_document_loader *loader,
                                    sxml_byte_view uri) {
    const sxml_available_document_view available = loader->configuration.available_documents;
    for (size_t document_index = 0; document_index < available.available_document_count;
         document_index += 1) {
        const sxml_available_document *document =
            &available.available_documents[document_index];
        if (sxml_byte_view_equals(document->document_uri, uri)) {
            return (sxml_document_lookup){.is_found = true,
                                          .document_node = document->document_node};
        }
    }
    return (sxml_document_lookup){.is_found = false, .document_node = {.representation = 0}};
}

/* A kept document's key: its URI and the preparation it went through
   (4.2.2: each XSLT package strips by its own declarations). */
typedef struct sxml_kept_document_key {
    sxml_byte_view uri;
    uint32_t preparation_number;
} sxml_kept_document_key;

static sxml_document_lookup sxml_loader_find_document(const sxml_processor_document_loader *loader,
                                                      sxml_kept_document_key key) {
    const sxml_byte_view uri = key.uri;
    const sxml_document_lookup available = sxml_loader_find_available_document(loader, uri);
    if (available.is_found) {
        return available;
    }
    for (size_t document_index = 0; document_index < loader->loaded_document_count;
         document_index += 1) {
        const sxml_processor_loaded_document *document =
            &loader->loaded_documents[document_index];
        if (document->preparation_number == key.preparation_number &&
            sxml_byte_view_equals((sxml_byte_view){.bytes = document->uri_bytes,
                                                   .byte_count = document->uri_byte_count},
                                  uri)) {
            return (sxml_document_lookup){.is_found = true,
                                          .document_node = document->document_node};
        }
    }
    return (sxml_document_lookup){.is_found = false, .document_node = {.representation = 0}};
}

/* A document to keep for the rest of the evaluation. */
typedef struct sxml_loaded_document_request {
    sxml_byte_view uri;
    /* Retained by the loader. */
    sxml_item document_node;
    uint32_t preparation_number;
} sxml_loaded_document_request;

/* An array of the loader that grows by doubling. */
typedef struct sxml_loader_array {
    void **elements;
    size_t *capacity;
    size_t element_count;
    size_t element_byte_count;
    size_t element_alignment;
} sxml_loader_array;

/* Makes room for one more element; the array is unchanged on failure. */
static sxml_status sxml_loader_array_reserve(sxml_allocator allocator,
                                             const sxml_loader_array *array) {
    return sxml_allocator_reserve_array(
        allocator, &(sxml_array_reservation_request){.elements = array->elements,
                                                     .capacity = array->capacity,
                                                     .element_count = array->element_count,
                                                     .required_count = array->element_count + 1,
                                                     .element_size = array->element_byte_count,
                                                     .element_alignment = array->element_alignment,
                                                     .minimum_capacity = 4});
}

/* Copies uri into memory of the allocator; NULL for an empty one. */
static sxml_status sxml_loader_copy_uri(sxml_allocator allocator, sxml_byte_view uri,
                                        uint8_t **uri_bytes) {
    *uri_bytes = NULL;
    if (uri.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator, (sxml_allocation_request){.byte_count = uri.byte_count, .alignment = 1},
        &memory);
    if (status == SXML_STATUS_OK) {
        memcpy(memory, uri.bytes, uri.byte_count);
        *uri_bytes = memory;
    }
    return status;
}

static sxml_status sxml_loader_keep_document(sxml_processor_document_loader *loader,
                                             const sxml_loaded_document_request *request) {
    const sxml_allocator allocator = loader->configuration.evaluation_environment.processor->allocator;
    void *elements = loader->loaded_documents;
    sxml_status status = sxml_loader_array_reserve(
        allocator, &(sxml_loader_array){.elements = &elements,
                                        .capacity = &loader->loaded_document_capacity,
                                        .element_count = loader->loaded_document_count,
                                        .element_byte_count = sizeof(sxml_processor_loaded_document),
                                        .element_alignment = alignof(sxml_processor_loaded_document)});
    loader->loaded_documents = elements;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *uri_memory = NULL;
    status = sxml_loader_copy_uri(allocator, request->uri, &uri_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(request->document_node);
    loader->loaded_documents[loader->loaded_document_count] =
        (sxml_processor_loaded_document){.uri_bytes = uri_memory,
                                         .uri_byte_count = request->uri.byte_count,
                                         .document_node = request->document_node,
                                         .preparation_number = request->preparation_number};
    loader->loaded_document_count += 1;
    return SXML_STATUS_OK;
}

/* True when item is the document node of an XTree. */
static bool sxml_item_is_document_node(sxml_item item) {
    sxml_node_reference node;
    if (sxml_item_get_node(item, &node) != SXML_STATUS_OK) {
        return false;
    }
    sxml_node_description description;
    return sxml_node_describe(node, &description) == SXML_STATUS_OK &&
           description.node_kind == SXML_NODE_KIND_DOCUMENT;
}

/* Raises FODC0002 with the resolver's description, when it gave one,
   followed by the URI reference as fn:doc was given it, in parentheses
   (F&O 4.0, fn:doc: the error is about that resource; XSLT 4.0, 8.4's
   $err:description). */
static sxml_status sxml_loader_report_not_available(const sxml_processor_document_loader *loader,
                                                    const sxml_document_request *request,
                                                    sxml_byte_view description) {
    if (description.byte_count == 0) {
        return sxml_error_record_report(request->error_record,
                                        &sxml_document_not_available_error);
    }
    sxml_processor *processor = loader->configuration.evaluation_environment.processor;
    const sxml_byte_view uri = request->uri_reference;
    const size_t text_byte_count = description.byte_count + uri.byte_count + 3U;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = text_byte_count, .alignment = 1}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *text = memory;
    memcpy(text, description.bytes, description.byte_count);
    text[description.byte_count] = ' ';
    text[description.byte_count + 1U] = '(';
    if (uri.byte_count != 0) {
        memcpy(text + description.byte_count + 2U, uri.bytes, uri.byte_count);
    }
    text[text_byte_count - 1U] = ')';
    status = sxml_item_heap_raise_error(
        processor->item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FODC0002"),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .description = {.bytes = text, .byte_count = text_byte_count},
            .has_value = false,
            .value = sxml_sequence_empty(),
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        request->error_record);
    sxml_allocator_release(processor->allocator, memory);
    return status;
}

/* What the resolver was asked and what it said. */
typedef struct sxml_resolution_exchange {
    const sxml_document_request *request;
    sxml_byte_view absolute_uri;
    /* What the resolver delivered; NULL before it was asked. */
    const sxml_resource *resource;
} sxml_resolution_exchange;

/* F&O 4.0, fn:doc and fn:parse-xml (17.1.1, 17.2.1): FODC0013 when the
   processor has no parser for the requested options; here the
   dtd-validation option of a processor whose configuration does not name
   sxml_dtd_validation_feature (D162's place, D211). */
static const sxml_static_error sxml_document_dtd_validation_unavailable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0013"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "DTD validation is not available: the processor's configuration does not name "
        "the dtd-validation feature (F&O 4.0, fn:doc, fn:parse-xml: dtd-validation)"),
};

static const sxml_static_error sxml_document_untrusted_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0016"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The document refers to an external DTD or entity, and the call does not "
        "trust external resources (F&O 4.0, fn:doc, fn:parse-xml: trust-external)"),
};

/* How a document of fn:doc or fn:parse-xml is parsed (D199). */
typedef struct sxml_loader_parse {
    const sxml_processor_document_loader *loader;
    /* The request without its entity resolver and validator. */
    const sxml_xml_parse_request *request;
    bool blocks_external_resources;
    bool validates_dtd;
    sxml_error_record *error_record;
} sxml_loader_parse;

/* Raises FODC0007 with the DTD validator's description, which the raise
   copies (F&O 4.0, fn:parse-xml, fn:doc: dtd-validation). */
static sxml_status sxml_loader_report_invalid(const sxml_loader_parse *parse,
                                              const sxml_error_record *validation_error) {
    sxml_byte_view description = validation_error->diagnostic.description;
    if (description.byte_count == 0) {
        description = SXML_BYTE_VIEW_FROM_LITERAL(
            "The document is not valid against its DTD (F&O 4.0, dtd-validation)");
    }
    return sxml_item_heap_raise_error(
        parse->loader->configuration.evaluation_environment.processor->item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FODC0007"),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .description = description,
            .has_value = false,
            .value = sxml_sequence_empty(),
            .location = validation_error->diagnostic.location},
        parse->error_record);
}

/*
 * Parses a document of fn:doc or fn:parse-xml: its external DTD and
 * entities read through the evaluation's resolver, or refused (FODC0016)
 * when the call does not trust external resources; validated against its
 * DTD on request (FODC0007). F&O 4.0, 17.1.1 and 17.2.1.
 */
static sxml_status sxml_loader_parse_with_dtd(const sxml_loader_parse *parse,
                                              sxml_item *document_node) {
    sxml_processor *processor = parse->loader->configuration.evaluation_environment.processor;
    sxml_entity_resolution resolution = {
        .processor = processor,
        .resolver = parse->loader->configuration.evaluation_environment.resource_resolver,
        .refuses = parse->blocks_external_resources,
        .was_refused = false,
        .holds_resource = false};
    sxml_error_record validation_error;
    memset(&validation_error, 0, sizeof(validation_error));
    sxml_dtd_validator *validator = NULL;
    sxml_status status = SXML_STATUS_OK;
    /* The DTD validator is an optional feature (D211), so a program that
       does not name it links no schema compiler. */
    const sxml_dtd_validation_feature_operations *validation = NULL;
    if (parse->validates_dtd) {
        validation = sxml_processor_find_optional_feature(
            processor, SXML_OPTIONAL_FEATURE_KIND_DTD_VALIDATION);
        if (validation == NULL) {
            return sxml_error_record_report(parse->error_record,
                                            &sxml_document_dtd_validation_unavailable_error);
        }
        status = validation->create_validator(
            processor->allocator,
            &(sxml_dtd_validator_configuration){
                .processor = processor,
                .report_sink = {.state = NULL, .operations = NULL},
                .error_record = &validation_error},
            &validator);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_parse_request request = *parse->request;
    request.entity_resolver = sxml_entity_resolution_as_resolver(&resolution);
    request.validator = validator == NULL
                            ? (sxml_xml_validator){.state = NULL, .operations = NULL}
                            : validation->as_xml_validator(validator);
    status = sxml_processor_parse_document(processor, &request, document_node);
    bool is_invalid = status == SXML_STATUS_DYNAMIC_ERROR && validator != NULL &&
                      validation_error.code.local_name.byte_count != 0;
    if (status == SXML_STATUS_OK && validator != NULL) {
        sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
        is_invalid = validation->get_outcome(validator, &outcome) != SXML_STATUS_OK ||
                     outcome.validity != SXML_VALIDITY_VALID;
    }
    if (status == SXML_STATUS_OK && (resolution.was_refused || is_invalid)) {
        sxml_item_release(*document_node);
        *document_node = (sxml_item){.representation = 0};
    }
    /* A refused external resource decides the error, whatever the parse
       made of its absence: the document needed it (FODC0016). */
    const bool is_resource_failure = status == SXML_STATUS_OUT_OF_MEMORY;
    if (resolution.was_refused && !is_resource_failure) {
        status = sxml_error_record_report(parse->error_record, &sxml_document_untrusted_error);
    } else if (is_invalid) {
        status = sxml_loader_report_invalid(parse, &validation_error);
    }
    if (validation != NULL) {
        validation->destroy_validator(validator);
    }
    return status;
}

/* Parses the bytes a resolver delivered into a document node, owned. */
static sxml_status sxml_loader_parse_resource(sxml_processor_document_loader *loader,
                                              const sxml_resolution_exchange *exchange,
                                              sxml_item *document_node) {
    const sxml_document_request *request = exchange->request;
    const sxml_resource *resource = exchange->resource;
    sxml_byte_view document_uri = resource->resource_uri;
    if (document_uri.byte_count == 0) {
        document_uri = exchange->absolute_uri.byte_count > 0 ? exchange->absolute_uri
                                                             : request->uri_reference;
    }
    const sxml_status status = sxml_loader_parse_with_dtd(
        &(sxml_loader_parse){
            .loader = loader,
            .request = &(sxml_xml_parse_request){.input = resource->content,
                                                 .source_name = document_uri,
                                                 .document_uri = document_uri,
                                                 .base_uri = document_uri,
                                                 .encoding_name = resource->encoding_name,
                                                 .diagnostic = NULL},
            .blocks_external_resources = request->blocks_external_resources,
            .validates_dtd = request->validates_dtd,
            .error_record = request->error_record},
        document_node);
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        /* F&O 4.0, fn:doc: a resource that cannot be parsed is FODC0002. */
        return sxml_error_record_report(request->error_record,
                                        &sxml_document_not_well_formed_error);
    }
    return status;
}

/* Reads a byte reader to its end into allocator memory, which *content
   then holds (the caller releases content->bytes). */
typedef struct sxml_byte_reader_drain {
    sxml_allocator allocator;
    sxml_byte_reader byte_reader;
} sxml_byte_reader_drain;

static sxml_status sxml_loader_drain_reader(const sxml_byte_reader_drain *drain,
                                            sxml_byte_view *content) {
    uint8_t *bytes = NULL;
    size_t byte_count = 0;
    size_t capacity = 0;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        if (byte_count == capacity) {
            size_t grown_capacity = 0;
            status = sxml_size_multiply(capacity == 0 ? 32768 : capacity, 2, &grown_capacity);
            void *grown = NULL;
            if (status == SXML_STATUS_OK) {
                status = sxml_allocator_allocate(
                    drain->allocator,
                    (sxml_allocation_request){.byte_count = grown_capacity, .alignment = 1},
                    &grown);
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (byte_count != 0) {
                memcpy(grown, bytes, byte_count);
            }
            sxml_allocator_release(drain->allocator, bytes);
            bytes = grown;
            capacity = grown_capacity;
        }
        sxml_byte_reading reading = {.byte_count = 0, .is_end = false};
        status = drain->byte_reader.operations->read(
            drain->byte_reader.state,
            (sxml_memory_view){.bytes = bytes + byte_count, .byte_count = capacity - byte_count},
            &reading);
        if (status == SXML_STATUS_OK && reading.byte_count > capacity - byte_count) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        byte_count += reading.byte_count;
        if (reading.is_end || reading.byte_count == 0) {
            *content = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
            return SXML_STATUS_OK;
        }
    }
    sxml_allocator_release(drain->allocator, bytes);
    return status;
}

/* A document a byte reader delivers, read whole first: fn:doc builds its
   tree, which holds the document anyway (D233). */
static sxml_status sxml_loader_parse_reader(sxml_processor_document_loader *loader,
                                            const sxml_resolution_exchange *exchange,
                                            sxml_item *document_node) {
    const sxml_allocator allocator =
        loader->configuration.evaluation_environment.processor->allocator;
    if (exchange->resource->byte_reader.operations == NULL ||
        exchange->resource->byte_reader.operations->read == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_resource read = *exchange->resource;
    sxml_status status = sxml_loader_drain_reader(
        &(sxml_byte_reader_drain){.allocator = allocator,
                                  .byte_reader = exchange->resource->byte_reader},
        &read.content);
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        return sxml_loader_report_not_available(
            loader, exchange->request, SXML_BYTE_VIEW_FROM_LITERAL("The resource cannot be read"));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    read.resource_form = SXML_RESOURCE_FORM_BYTES;
    status = sxml_loader_parse_resource(
        loader,
        &(sxml_resolution_exchange){.request = exchange->request,
                                    .absolute_uri = exchange->absolute_uri,
                                    .resource = &read},
        document_node);
    sxml_allocator_release(allocator, (void *)read.content.bytes);
    return status;
}

/* Turns a resource the resolver delivered into a document node, owned. */
static sxml_status sxml_loader_take_resource(sxml_processor_document_loader *loader,
                                             const sxml_resolution_exchange *exchange,
                                             sxml_item *document_node) {
    const sxml_document_request *request = exchange->request;
    const sxml_resource *resource = exchange->resource;
    switch (resource->resource_form) {
    case SXML_RESOURCE_FORM_NONE:
        return sxml_loader_report_not_available(loader, request, resource->description);
    case SXML_RESOURCE_FORM_DOCUMENT_NODE:
        if (!sxml_item_is_document_node(resource->document_node)) {
            sxml_item_release(resource->document_node);
            return sxml_error_record_report(request->error_record,
                                            &sxml_document_not_a_document_error);
        }
        *document_node = resource->document_node;
        return SXML_STATUS_OK;
    case SXML_RESOURCE_FORM_SEQUENCE:
        /* Not a form of an XML document (include/sxml/processor.h). */
        sxml_sequence_release(resource->items);
        return SXML_STATUS_INVALID_ARGUMENT;
    case SXML_RESOURCE_FORM_BYTE_READER:
        return sxml_loader_parse_reader(loader, exchange, document_node);
    case SXML_RESOURCE_FORM_BYTES:
        break;
    }
    return sxml_loader_parse_resource(loader, exchange, document_node);
}

/* Asks the resolver for the document and makes its node, owned. */
static sxml_status sxml_loader_resolve(sxml_processor_document_loader *loader,
                                       const sxml_resolution_exchange *exchange,
                                       sxml_item *document_node) {
    const sxml_resource_resolver resolver = loader->configuration.evaluation_environment.resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        return sxml_error_record_report(exchange->request->error_record,
                                        &sxml_document_not_available_error);
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    sxml_status status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT,
                                 .uri_reference = exchange->request->uri_reference,
                                 .absolute_uri = exchange->absolute_uri,
                                 .processor = loader->configuration.evaluation_environment.processor},
        &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_resolution_exchange delivered = {.request = exchange->request,
                                                .absolute_uri = exchange->absolute_uri,
                                                .resource = &resource};
    status = sxml_loader_take_resource(loader, &delivered, document_node);
    if (resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    const sxml_document_preparation preparation = loader->configuration.document_preparation;
    if (status == SXML_STATUS_OK && preparation.operations != NULL &&
        !exchange->request->skips_preparation) {
        sxml_item prepared = {.representation = 0};
        status = preparation.operations->prepare_document(
            preparation.state,
            &(sxml_preparation_request){
                .document_node = *document_node,
                .preparation_number = exchange->request->preparation_number},
            &prepared);
        sxml_item_release(*document_node);
        *document_node = prepared;
    }
    return status;
}

/*
 * fn:doc (F&O 4.0, section 15.1.1): the URI reference is resolved against
 * the request's base URI, or the static base URI; the result is looked up
 * among the available documents and the documents already loaded (fn:doc
 * is stable: the same URI gives the same node within an evaluation), then
 * asked of the resolver.
 */
static sxml_status sxml_loader_load_document(void *state,
                                             const sxml_document_request *request,
                                             sxml_item *document_node) {
    sxml_processor_document_loader *loader = state;
    const sxml_allocator allocator = loader->configuration.evaluation_environment.processor->allocator;
    /* The caller passes the static base URI of the expression that calls
       the function (lexical, D91), or the base-uri option. */
    const sxml_byte_view base_uri = request->base_uri;
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0},
                                      .memory = NULL};
    sxml_status status = sxml_uri_resolve(
        &(sxml_uri_resolution_request){.base_uri = base_uri,
                                       .reference = request->uri_reference,
                                       .allocator = allocator},
        &resolution);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view absolute_uri = sxml_uri_has_scheme(resolution.uri)
                                            ? resolution.uri
                                            : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    /* strip-space="none" with a preparation: not the kept, prepared
       document but a new unprepared one. An available document is given
       as configured (XSLT configures its stripped copies, 4.2.2). */
    const bool is_unprepared =
        request->skips_preparation && loader->configuration.document_preparation.operations != NULL;
    const sxml_document_lookup lookup =
        is_unprepared ? sxml_loader_find_available_document(loader, resolution.uri)
                      : sxml_loader_find_document(
                            loader, (sxml_kept_document_key){
                                        .uri = resolution.uri,
                                        .preparation_number = request->preparation_number});
    sxml_item node = {.representation = 0};
    if (lookup.is_found) {
        sxml_item_retain(lookup.document_node);
        node = lookup.document_node;
    } else {
        status = sxml_loader_resolve(
            loader,
            &(sxml_resolution_exchange){
                .request = request, .absolute_uri = absolute_uri, .resource = NULL},
            &node);
        if (status == SXML_STATUS_OK && !is_unprepared) {
            status = sxml_loader_keep_document(
                loader, &(sxml_loaded_document_request){
                            .uri = resolution.uri,
                            .document_node = node,
                            .preparation_number = request->preparation_number});
            if (status != SXML_STATUS_OK) {
                sxml_item_release(node);
            }
        }
    }
    sxml_allocator_release(allocator, resolution.memory);
    if (status == SXML_STATUS_OK) {
        *document_node = node;
    }
    return status;
}

/*
 * xsl:source-document with streamable="yes" (XSLT 4.0, 18.1; D219): the
 * document fn:doc would read, as bytes to parse incrementally when the
 * resolver delivers bytes, else as the node fn:doc gives (an available or
 * loaded document, a node the resolver delivers). A streamed document is
 * not kept: each xsl:source-document reads it again.
 */
sxml_status sxml_processor_document_loader_open_streamed(
    sxml_processor_document_loader *loader, const sxml_streamed_document_request *request,
    sxml_streamed_document *document) {
    memset(document, 0, sizeof(*document));
    document->document_node = (sxml_item){.representation = 0};
    const sxml_allocator allocator = loader->configuration.evaluation_environment.processor->allocator;
    sxml_status status = sxml_uri_resolve(
        &(sxml_uri_resolution_request){.base_uri = request->base_uri,
                                       .reference = request->uri_reference,
                                       .allocator = allocator},
        &document->resolution);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_document_request document_request = {.uri_reference = request->uri_reference,
                                                    .base_uri = request->base_uri,
                                                    .error_record = request->error_record,
                                                    .blocks_external_resources = false,
                                                    .validates_dtd = false};
    const sxml_document_lookup lookup = sxml_loader_find_document(
        loader,
        (sxml_kept_document_key){.uri = document->resolution.uri, .preparation_number = 0});
    const sxml_resource_resolver resolver = loader->configuration.evaluation_environment.resource_resolver;
    if (lookup.is_found || resolver.operations == NULL || resolver.operations->resolve == NULL) {
        return sxml_loader_load_document(loader, &document_request, &document->document_node);
    }
    const sxml_byte_view absolute_uri = sxml_uri_has_scheme(document->resolution.uri)
                                            ? document->resolution.uri
                                            : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT,
                                 .uri_reference = request->uri_reference,
                                 .absolute_uri = absolute_uri,
                                 .processor = loader->configuration.evaluation_environment.processor},
        &document->resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool reads_bytes =
        document->resource.resource_form == SXML_RESOURCE_FORM_BYTE_READER &&
        document->resource.byte_reader.operations != NULL &&
        document->resource.byte_reader.operations->read != NULL;
    if (document->resource.resource_form != SXML_RESOURCE_FORM_BYTES && !reads_bytes) {
        status = sxml_loader_take_resource(
            loader,
            &(sxml_resolution_exchange){.request = &document_request,
                                        .absolute_uri = absolute_uri,
                                        .resource = &document->resource},
            &document->document_node);
        if (resolver.operations->release != NULL) {
            resolver.operations->release(resolver.state, &document->resource);
        }
        const sxml_document_preparation preparation = loader->configuration.document_preparation;
        if (status == SXML_STATUS_OK && preparation.operations != NULL) {
            sxml_item prepared = {.representation = 0};
            status = preparation.operations->prepare_document(
                preparation.state,
                &(sxml_preparation_request){.document_node = document->document_node,
                                            .preparation_number = 0},
                &prepared);
            sxml_item_release(document->document_node);
            document->document_node = prepared;
        }
        return status;
    }
    document->holds_resource = true;
    document->is_streamed = true;
    document->byte_reader = reads_bytes
                                ? document->resource.byte_reader
                                : (sxml_byte_reader){.state = NULL, .operations = NULL};
    sxml_byte_view document_uri = document->resource.resource_uri;
    if (document_uri.byte_count == 0) {
        document_uri = absolute_uri.byte_count > 0 ? absolute_uri : request->uri_reference;
    }
    document->entity_resolution = (sxml_entity_resolution){
        .processor = loader->configuration.evaluation_environment.processor,
        .resolver = resolver,
        .refuses = false,
        .was_refused = false,
        .holds_resource = false};
    document->parse_request = (sxml_xml_parse_request){
        .input = reads_bytes ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                             : document->resource.content,
        .source_name = document_uri,
        .document_uri = document_uri,
        .base_uri = document_uri,
        .encoding_name = document->resource.encoding_name,
        .diagnostic = NULL,
        .entity_resolver = sxml_entity_resolution_as_resolver(&document->entity_resolution),
        .validator = {.state = NULL, .operations = NULL}};
    document->preparation = loader->configuration.document_preparation;
    return SXML_STATUS_OK;
}

void sxml_processor_document_loader_close_streamed(sxml_processor_document_loader *loader,
                                                   sxml_streamed_document *document) {
    const sxml_resource_resolver resolver = loader->configuration.evaluation_environment.resource_resolver;
    if (document->holds_resource && resolver.operations != NULL &&
        resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &document->resource);
    }
    document->holds_resource = false;
    sxml_item_release(document->document_node);
    document->document_node = (sxml_item){.representation = 0};
    sxml_allocator_release(loader->configuration.evaluation_environment.processor->allocator,
                           document->resolution.memory);
    document->resolution.memory = NULL;
    document->is_streamed = false;
}

/*
 * fn:parse-xml (F&O 4.0, section 17.2.1): a new tree on every call, whose
 * base URI is the request's (the base-uri option) or the static base URI,
 * and which has no document URI.
 */
static sxml_status sxml_loader_parse_document(void *state,
                                              const sxml_document_parse_request *request,
                                              sxml_item *document_node) {
    const sxml_processor_document_loader *loader = state;
    /* The caller passes the static base URI of the expression that calls
       the function (lexical, D91), or the base-uri option. */
    const sxml_byte_view base_uri = request->base_uri;
    const sxml_status status = sxml_loader_parse_with_dtd(
        &(sxml_loader_parse){
            .loader = loader,
            .request = &(sxml_xml_parse_request){.input = request->xml_bytes,
                                                 .source_name = {.bytes = NULL, .byte_count = 0},
                                                 .document_uri = {.bytes = NULL, .byte_count = 0},
                                                 .base_uri = base_uri,
                                                 .encoding_name = {.bytes = NULL, .byte_count = 0},
                                                 .diagnostic = NULL},
            .blocks_external_resources = request->blocks_external_resources,
            .validates_dtd = request->validates_dtd,
            .error_record = request->error_record},
        document_node);
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return sxml_error_record_report(request->error_record,
                                        &sxml_parse_xml_not_well_formed_error);
    }
    return status;
}

static const sxml_document_loader_operations sxml_processor_document_loader_operations = {
    .load_document = sxml_loader_load_document,
    .parse_document = sxml_loader_parse_document,
};

sxml_document_loader sxml_processor_document_loader_as_document_loader(
    sxml_processor_document_loader *loader) {
    return (sxml_document_loader){.state = loader,
                                  .operations = &sxml_processor_document_loader_operations};
}

sxml_status sxml_processor_compile_document_loader_initialize(
    sxml_memory_view memory, sxml_processor *processor,
    sxml_processor_document_loader **loader) {
    return sxml_processor_document_loader_initialize(
        memory,
        &(sxml_processor_document_loader_configuration){
            .evaluation_environment = {.processor = processor,
                                       .resource_resolver = processor->resource_resolver,
                                       .trace_sink = processor->trace_sink,
                                       .default_language = SXML_BYTE_VIEW_FROM_LITERAL("en"),
                                       .environment_variables = {.environment_variables = NULL,
                                                                 .environment_variable_count = 0},
                                       .result_writer = NULL,
                                       .principal_result_builder = {.state = NULL,
                                                                    .operations = NULL},
                                       .principal_result_is_raw = NULL},
            .available_documents = {.available_documents = NULL, .available_document_count = 0},
            .document_preparation = {.state = NULL, .operations = NULL}},
        loader);
}

/* The first element child of a document node; false for none (XDM 4.0,
   a document node's children). */
static bool sxml_find_document_element(sxml_item document_node, sxml_node_reference *element) {
    sxml_node_reference document;
    sxml_node_description description;
    if (sxml_item_get_node(document_node, &document) != SXML_STATUS_OK ||
        sxml_node_describe(document, &description) != SXML_STATUS_OK) {
        return false;
    }
    bool has_child = description.has_children;
    sxml_node_index child = description.first_child;
    while (has_child) {
        const sxml_node_reference child_node = {
            .tree = document.tree, .node_index = child, .namespace_number = 0};
        sxml_node_description child_description;
        if (sxml_node_describe(child_node, &child_description) != SXML_STATUS_OK) {
            return false;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            *element = child_node;
            return true;
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return false;
}

sxml_status sxml_parameter_document_load(const sxml_parameter_document_request *request,
                                         sxml_parameter_document *document) {
    memset(document, 0, sizeof(*document));
    sxml_error_record load_error;
    memset(&load_error, 0, sizeof(load_error));
    sxml_item document_node = {.representation = 0};
    const sxml_status status = request->loader.operations->load_document(
        request->loader.state,
        &(sxml_document_request){.uri_reference = request->uri_reference,
                                 .base_uri = request->base_uri,
                                 .error_record = &load_error},
        &document_node);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document->is_loaded = true;
    document->document_node = document_node;
    document->has_document_element =
        sxml_find_document_element(document_node, &document->document_element);
    return SXML_STATUS_OK;
}

void sxml_parameter_document_release(sxml_parameter_document *document) {
    if (document->is_loaded) {
        sxml_item_release(document->document_node);
    }
    document->is_loaded = false;
    document->has_document_element = false;
}

sxml_processor_document_loader *sxml_processor_document_loader_from_dynamic_context(
    const sxml_dynamic_context *dynamic_context) {
    if (dynamic_context == NULL ||
        dynamic_context->document_loader.operations != &sxml_processor_document_loader_operations) {
        return NULL;
    }
    return dynamic_context->document_loader.state;
}

const sxml_evaluation_environment *sxml_processor_document_loader_get_environment(
    const sxml_processor_document_loader *loader) {
    return &loader->configuration.evaluation_environment;
}

const sxml_evaluation_environment *sxml_processor_get_evaluation_environment(
    const sxml_dynamic_context *dynamic_context) {
    const sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(dynamic_context);
    return loader == NULL ? NULL : sxml_processor_document_loader_get_environment(loader);
}

/* ------------------------------------------------------------ Resources */

/* Resolves reference against base_uri into *resolution; the URI is
   absolute when it has a scheme (RFC 3986, section 4.3). */
typedef struct sxml_loader_uri_request {
    sxml_byte_view reference;
    sxml_byte_view base_uri;
} sxml_loader_uri_request;

static sxml_status sxml_loader_resolve_uri(const sxml_processor_document_loader *loader,
                                           sxml_loader_uri_request request,
                                           sxml_uri_resolution *resolution) {
    return sxml_uri_resolve(
        &(sxml_uri_resolution_request){
            .base_uri = request.base_uri,
            .reference = request.reference,
            .allocator = loader->configuration.evaluation_environment.processor->allocator},
        resolution);
}

/* Calls the resolver's release, when it has one, for a resource it
   delivered. */
static void sxml_loader_give_back(const sxml_processor_document_loader *loader,
                                  const sxml_resource *resource) {
    const sxml_resource_resolver resolver =
        loader->configuration.evaluation_environment.resource_resolver;
    if (resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, resource);
    }
}

void sxml_processor_document_loader_release(sxml_processor_document_loader *loader,
                                            sxml_retrieved_resource *retrieved) {
    if (retrieved->is_available &&
        retrieved->resource.resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE) {
        sxml_item_release(retrieved->resource.document_node);
    }
    if (retrieved->is_delivered) {
        sxml_loader_give_back(loader, &retrieved->resource);
    }
    sxml_allocator_release(loader->configuration.evaluation_environment.processor->allocator,
                           retrieved->resolution.memory);
    memset(retrieved, 0, sizeof(*retrieved));
}

/* Takes the form the resolver delivered for a TEXT or BINARY request
   (include/sxml/processor.h: BYTES only), or for feature data that
   accepts a document node too. */
static sxml_status sxml_loader_take_bytes(sxml_retrieved_resource *retrieved,
                                          const sxml_resource_retrieval_request *request) {
    const sxml_resource *resource = &retrieved->resource;
    switch (resource->resource_form) {
    case SXML_RESOURCE_FORM_NONE:
        retrieved->description = resource->description;
        return SXML_STATUS_OK;
    case SXML_RESOURCE_FORM_BYTES:
        retrieved->is_available = true;
        retrieved->content = resource->content;
        retrieved->encoding_name = resource->encoding_name;
        return SXML_STATUS_OK;
    case SXML_RESOURCE_FORM_DOCUMENT_NODE:
        if (request->accepts_document_node) {
            retrieved->is_available = true;
            retrieved->document_node = resource->document_node;
            return SXML_STATUS_OK;
        }
        sxml_item_release(resource->document_node);
        return SXML_STATUS_INVALID_ARGUMENT;
    case SXML_RESOURCE_FORM_SEQUENCE:
        sxml_sequence_release(resource->items);
        return SXML_STATUS_INVALID_ARGUMENT;
    case SXML_RESOURCE_FORM_BYTE_READER:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status sxml_processor_document_loader_retrieve(
    sxml_processor_document_loader *loader, const sxml_resource_retrieval_request *request,
    sxml_retrieved_resource *retrieved) {
    memset(retrieved, 0, sizeof(*retrieved));
    const sxml_evaluation_environment *environment =
        &loader->configuration.evaluation_environment;
    sxml_status status = SXML_STATUS_OK;
    if (!request->asks_unresolved || request->uri_reference.byte_count != 0) {
        status = sxml_loader_resolve_uri(
            loader,
            (sxml_loader_uri_request){.reference = request->uri_reference,
                                      .base_uri = request->base_uri},
            &retrieved->resolution);
    }
    if (status != SXML_STATUS_OK) {
        memset(retrieved, 0, sizeof(*retrieved));
        return status;
    }
    retrieved->has_absolute_uri = sxml_uri_has_scheme(retrieved->resolution.uri);
    if (!retrieved->has_absolute_uri && !request->asks_unresolved) {
        /* F&O 4.0, fn:unparsed-text: FOUT1170 when $source "cannot be
           resolved to an absolute URI". */
        retrieved->description = SXML_BYTE_VIEW_FROM_LITERAL(
            "The URI reference cannot be resolved to an absolute URI");
        return SXML_STATUS_OK;
    }
    const sxml_resource_resolver resolver = environment->resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        retrieved->description = SXML_BYTE_VIEW_FROM_LITERAL("No resource resolver is configured");
        return SXML_STATUS_OK;
    }
    status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = request->resource_kind,
                                 .uri_reference = request->uri_reference,
                                 .absolute_uri = retrieved->has_absolute_uri
                                                     ? retrieved->resolution.uri
                                                     : (sxml_byte_view){.bytes = NULL,
                                                                        .byte_count = 0},
                                 .processor = environment->processor,
                                 .encoding_name = request->encoding_name},
        &retrieved->resource);
    if (status == SXML_STATUS_OK) {
        retrieved->is_delivered = true;
        status = sxml_loader_take_bytes(retrieved, request);
    }
    if (status != SXML_STATUS_OK) {
        sxml_processor_document_loader_release(loader, retrieved);
    }
    return status;
}

/* The collection of the kind and URI read before in this evaluation, or
   NULL. */
static const sxml_processor_loaded_collection *sxml_loader_find_collection(
    const sxml_processor_document_loader *loader, const sxml_processor_loaded_collection *key) {
    for (size_t collection_index = 0; collection_index < loader->loaded_collection_count;
         collection_index += 1) {
        const sxml_processor_loaded_collection *collection =
            &loader->loaded_collections[collection_index];
        if (collection->resource_kind == key->resource_kind &&
            collection->is_default_collection == key->is_default_collection &&
            sxml_byte_view_equals(
                (sxml_byte_view){.bytes = collection->uri_bytes,
                                 .byte_count = collection->uri_byte_count},
                (sxml_byte_view){.bytes = key->uri_bytes, .byte_count = key->uri_byte_count})) {
            return collection;
        }
    }
    return NULL;
}

/* Keeps a copy of key (its URI copied, its items retained) for the rest
   of the evaluation. */
static sxml_status sxml_loader_keep_collection(sxml_processor_document_loader *loader,
                                               const sxml_processor_loaded_collection *key) {
    const sxml_allocator allocator =
        loader->configuration.evaluation_environment.processor->allocator;
    void *elements = loader->loaded_collections;
    sxml_status status = sxml_loader_array_reserve(
        allocator,
        &(sxml_loader_array){.elements = &elements,
                             .capacity = &loader->loaded_collection_capacity,
                             .element_count = loader->loaded_collection_count,
                             .element_byte_count = sizeof(sxml_processor_loaded_collection),
                             .element_alignment = alignof(sxml_processor_loaded_collection)});
    loader->loaded_collections = elements;
    uint8_t *uri_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_loader_copy_uri(
            allocator, (sxml_byte_view){.bytes = key->uri_bytes, .byte_count = key->uri_byte_count},
            &uri_memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_retain(key->items);
    sxml_processor_loaded_collection *kept =
        &loader->loaded_collections[loader->loaded_collection_count];
    *kept = *key;
    kept->uri_bytes = uri_memory;
    loader->loaded_collection_count += 1;
    return SXML_STATUS_OK;
}

/* Keeps the document nodes of a collection that have a document URI
   among the documents loaded, and puts in *items, in their place, the
   document already loaded for that URI: fn:doc of a document-uri then
   gives the same node as the collection, whichever is called first (F&O
   4.0, 17.1.3: "the documents ... are available documents"; QT4
   collection-009; D298). *items is in/out, owned. */
static sxml_status sxml_loader_keep_collection_documents(sxml_processor_document_loader *loader,
                                                         sxml_sequence *items) {
    sxml_processor *processor = loader->configuration.evaluation_environment.processor;
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(*items, NULL, &item_count);
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = item_count > SIZE_MAX / sizeof(sxml_item)
                     ? SXML_STATUS_ARITHMETIC_OVERFLOW
                     : sxml_size_multiply((size_t)item_count + 1, sizeof(sxml_item), &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            processor->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Borrowed: the old sequence's items and the loader's documents. */
    sxml_item *members = memory;
    size_t member_count = 0;
    bool is_replaced = false;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(*items);
    while (member_count < item_count) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        members[member_count] = step.item;
        member_count += 1;
        sxml_node_reference document;
        sxml_tree_description tree;
        if (!sxml_item_is_document_node(step.item) ||
            sxml_item_get_node(step.item, &document) != SXML_STATUS_OK ||
            sxml_tree_describe(document.tree, &tree) != SXML_STATUS_OK ||
            tree.document_uri.byte_count == 0) {
            continue;
        }
        const sxml_document_lookup lookup = sxml_loader_find_document(
            loader, (sxml_kept_document_key){.uri = tree.document_uri, .preparation_number = 0});
        sxml_node_reference found = {.tree = NULL, .node_index = {0}, .namespace_number = 0};
        if (lookup.is_found) {
            /* Another tree unless the collection gave the loaded node. */
            is_replaced = is_replaced ||
                          sxml_item_get_node(lookup.document_node, &found) != SXML_STATUS_OK ||
                          found.tree != document.tree;
            members[member_count - 1] = lookup.document_node;
            continue;
        }
        status = sxml_loader_keep_document(
            loader, &(sxml_loaded_document_request){.uri = tree.document_uri,
                                                    .document_node = step.item});
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    sxml_sequence replacement = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && is_replaced) {
        status = sxml_sequence_make_vector(
            processor->item_heap,
            (sxml_item_view){.items = members, .item_count = member_count}, &replacement);
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(*items);
            *items = replacement;
        }
    }
    sxml_allocator_release(processor->allocator, memory);
    return status;
}

/* Asks the resolver for the collection key names and keeps it; *items
   gets one reference, owned. */
static sxml_status sxml_loader_resolve_collection(sxml_processor_document_loader *loader,
                                                  sxml_processor_loaded_collection *key,
                                                  sxml_loaded_collection *collection) {
    const sxml_evaluation_environment *environment =
        &loader->configuration.evaluation_environment;
    const sxml_resource_resolver resolver = environment->resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        collection->description = SXML_BYTE_VIEW_FROM_LITERAL("No resource resolver is configured");
        return SXML_STATUS_OK;
    }
    const sxml_byte_view absolute_uri = {.bytes = key->uri_bytes,
                                         .byte_count = key->uri_byte_count};
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    sxml_status status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = key->resource_kind,
                                 .uri_reference = absolute_uri,
                                 .absolute_uri = absolute_uri,
                                 .processor = environment->processor,
                                 .is_default_collection = key->is_default_collection},
        &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (resource.resource_form) {
    case SXML_RESOURCE_FORM_NONE:
        /* The resolver's reason, copied: it is borrowed only until the
           release below. */
        collection->description = SXML_BYTE_VIEW_FROM_LITERAL("No such collection");
        if (resource.description.byte_count > 0) {
            status = sxml_allocator_allocate(
                environment->processor->allocator,
                (sxml_allocation_request){.byte_count = resource.description.byte_count,
                                          .alignment = 1},
                &collection->description_memory);
            if (status == SXML_STATUS_OK) {
                memcpy(collection->description_memory, resource.description.bytes,
                       resource.description.byte_count);
                collection->description =
                    (sxml_byte_view){.bytes = collection->description_memory,
                                     .byte_count = resource.description.byte_count};
            }
        }
        break;
    case SXML_RESOURCE_FORM_SEQUENCE:
        status = key->resource_kind == SXML_RESOURCE_KIND_COLLECTION
                     ? sxml_loader_keep_collection_documents(loader, &resource.items)
                     : SXML_STATUS_OK;
        key->items = resource.items;
        if (status == SXML_STATUS_OK) {
            status = sxml_loader_keep_collection(loader, key);
        }
        if (status == SXML_STATUS_OK) {
            collection->is_available = true;
            collection->items = resource.items;
        } else {
            sxml_sequence_release(resource.items);
        }
        break;
    case SXML_RESOURCE_FORM_DOCUMENT_NODE:
        sxml_item_release(resource.document_node);
        status = SXML_STATUS_INVALID_ARGUMENT;
        break;
    case SXML_RESOURCE_FORM_BYTES:
    case SXML_RESOURCE_FORM_BYTE_READER:
        status = SXML_STATUS_INVALID_ARGUMENT;
        break;
    }
    sxml_loader_give_back(loader, &resource);
    return status;
}

/*
 * fn:collection and fn:uri-collection (F&O 4.0, sections 17.1.3 and
 * 17.1.4): a relative URI is resolved against the caller's static base
 * URI; the result is kept, so a second call with the same URI gives the
 * same items (the functions are deterministic).
 */
sxml_status sxml_processor_document_loader_load_collection(
    sxml_processor_document_loader *loader, const sxml_collection_request *request,
    sxml_loaded_collection *collection) {
    *collection = (sxml_loaded_collection){.is_available = false,
                                           .items = sxml_sequence_empty(),
                                           .description = {.bytes = NULL, .byte_count = 0},
                                           .description_memory = NULL};
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    if (!request->is_default_collection) {
        const sxml_status status = sxml_loader_resolve_uri(
            loader,
            (sxml_loader_uri_request){.reference = request->uri_reference,
                                      .base_uri = request->base_uri},
            &resolution);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!sxml_uri_has_scheme(resolution.uri)) {
            sxml_allocator_release(
                loader->configuration.evaluation_environment.processor->allocator,
                resolution.memory);
            collection->description = SXML_BYTE_VIEW_FROM_LITERAL(
                "The URI reference cannot be resolved to an absolute URI");
            return SXML_STATUS_OK;
        }
    }
    sxml_processor_loaded_collection key = {
        .resource_kind = request->resource_kind,
        .is_default_collection = request->is_default_collection,
        /* Only read through the key; the cast drops const for the struct. */
        .uri_bytes = (uint8_t *)resolution.uri.bytes,
        .uri_byte_count = resolution.uri.byte_count,
        .items = sxml_sequence_empty()};
    sxml_status status = SXML_STATUS_OK;
    const sxml_processor_loaded_collection *found = sxml_loader_find_collection(loader, &key);
    if (found != NULL) {
        sxml_sequence_retain(found->items);
        collection->is_available = true;
        collection->items = found->items;
    } else {
        status = sxml_loader_resolve_collection(loader, &key, collection);
    }
    sxml_allocator_release(loader->configuration.evaluation_environment.processor->allocator,
                           resolution.memory);
    return status;
}

void sxml_processor_document_loader_release_collection(sxml_processor_document_loader *loader,
                                                       sxml_loaded_collection *collection) {
    sxml_allocator_release(loader->configuration.evaluation_environment.processor->allocator,
                           collection->description_memory);
    collection->description_memory = NULL;
    collection->description = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

sxml_status sxml_processor_document_loader_trace(const sxml_processor_document_loader *loader,
                                                 const sxml_trace_event *event) {
    const sxml_evaluation_environment *environment =
        &loader->configuration.evaluation_environment;
    const sxml_trace_sink sink = environment->trace_sink;
    if (sink.operations == NULL || sink.operations->receive == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_trace_event delivered = *event;
    delivered.processor = environment->processor;
    return sink.operations->receive(sink.state, &delivered);
}

/* ------------------------------------------------------------ Trace sink */

/* Writes "label: value" and a line feed (include/sxml/processor.h,
   sxml_stream_trace_sink): the value in the adaptive method, which
   serializes any item (F&O 4.0, fn:trace: "Any serialization of the
   implementation's trace output must not raise an error"). */
static sxml_status sxml_stream_trace_sink_receive(void *state, const sxml_trace_event *event) {
    const sxml_output_stream *output_stream = state;
    if (event->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = SXML_STATUS_OK;
    if (event->label.byte_count > 0) {
        status = sxml_output_stream_write(*output_stream, event->label);
        if (status == SXML_STATUS_OK) {
            status = sxml_output_stream_write(*output_stream, SXML_BYTE_VIEW_FROM_LITERAL(": "));
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_serialization_parameters parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE;
        parameters.has_item_separator = true;
        parameters.item_separator = SXML_BYTE_VIEW_FROM_LITERAL(" ");
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(event->processor),
            &(sxml_serialization_request){.value = event->value,
                                          .serialization_parameters = parameters,
                                          .error_record = NULL},
            *output_stream);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_stream_write(*output_stream, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    }
    return status;
}

static const sxml_trace_sink_operations sxml_stream_trace_sink_operations = {
    .receive = sxml_stream_trace_sink_receive,
};

sxml_trace_sink sxml_stream_trace_sink(const sxml_output_stream *output_stream) {
    /* The state is only read: the cast drops const for the interface. */
    return (sxml_trace_sink){.state = (void *)output_stream,
                             .operations = &sxml_stream_trace_sink_operations};
}
