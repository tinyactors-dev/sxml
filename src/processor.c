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

#include "sxml/tree.h"
#include "sxml/xml.h"
#include "xdm_internal.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------- Clock */

/* The minutes east of UTC a timezone may have (XDM 4.0, section 5.4). */
enum { SXML_PROCESSOR_MAXIMUM_TIMEZONE_MINUTES = 840 };

enum { SXML_PROCESSOR_NANOSECONDS_PER_SECOND = 1000000000 };

/* True for a leap year of the proleptic Gregorian calendar, year 0
   included (XSD 1.1 Part 2, section D.2.1: year 0 is 1 BCE). */
static bool sxml_year_is_leap(int64_t year) {
    if (year % 4 != 0) {
        return false;
    }
    return year % 100 != 0 || year % 400 == 0;
}

static uint8_t sxml_month_day_count(int64_t year, uint8_t month) {
    static const uint8_t day_counts[12] = {31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31};
    if (month == 2 && sxml_year_is_leap(year)) {
        return 29;
    }
    return day_counts[month - 1];
}

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
        instant->day > sxml_month_day_count(instant->year, instant->month)) {
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

/* The civil date of a day number counted from 1970-01-01 (the inverse of
   "days from civil", H. Hinnant, "chrono-Compatible Low-Level Date
   Algorithms"), in the proleptic Gregorian calendar. */
typedef struct sxml_civil_date {
    int64_t year;
    uint8_t month;
    uint8_t day;
} sxml_civil_date;

static sxml_civil_date sxml_civil_date_from_day_number(int64_t day_number) {
    const int64_t shifted_day = day_number + 719468;
    const int64_t era = (shifted_day >= 0 ? shifted_day : shifted_day - 146096) / 146097;
    const int64_t day_of_era = shifted_day - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) /
        365;
    const int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_from_march = (5 * day_of_year + 2) / 153;
    const int64_t day = day_of_year - (153 * month_from_march + 2) / 5 + 1;
    const int64_t month = month_from_march < 10 ? month_from_march + 3 : month_from_march - 9;
    return (sxml_civil_date){.year = era * 400 + year_of_era + (month <= 2 ? 1 : 0),
                             .month = (uint8_t)month,
                             .day = (uint8_t)day};
}

/* Reads the platform's UTC clock (C17, section 7.27.2.5, timespec_get). */
static sxml_status sxml_system_clock_read(void *state, sxml_clock_reading *reading) {
    (void)state;
#if defined(TIME_UTC)
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    const int64_t seconds = (int64_t)now.tv_sec;
    const int64_t seconds_per_day = 86400;
    int64_t day_number = seconds / seconds_per_day;
    int64_t second_of_day = seconds % seconds_per_day;
    if (second_of_day < 0) {
        second_of_day += seconds_per_day;
        day_number -= 1;
    }
    const sxml_civil_date date = sxml_civil_date_from_day_number(day_number);
    *reading = (sxml_clock_reading){
        .current_date_time = {.year = date.year,
                              .month = date.month,
                              .day = date.day,
                              .hour = (uint8_t)(second_of_day / 3600),
                              .minute = (uint8_t)(second_of_day / 60 % 60),
                              .second = (uint8_t)(second_of_day % 60),
                              .nanosecond = (uint32_t)now.tv_nsec,
                              .has_timezone = true,
                              .timezone_offset_minutes = 0},
        .implicit_timezone_offset_minutes = 0};
    return SXML_STATUS_OK;
#else
    (void)reading;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
#endif
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

/* Every part of the function library (D55, D78, D79, D82, D88, D91). */
static const sxml_native_function_view *const sxml_processor_library_parts[] = {
    &sxml_function_library_numeric,  &sxml_function_library_string,
    &sxml_function_library_sequence, &sxml_function_library_node,
    &sxml_function_library_context,  &sxml_function_library_error,
    &sxml_function_library_higher_order, &sxml_function_library_map,
    &sxml_function_library_array,    &sxml_function_library_json,
    &sxml_function_library_uri,          &sxml_function_library_math,
    &sxml_function_library_qname,
};

static bool sxml_processor_configuration_is_valid(
    const sxml_processor_configuration *configuration) {
    return configuration != NULL && !sxml_allocator_is_null(configuration->allocator);
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
        .parts = {.native_function_views = sxml_processor_library_parts,
                  .native_function_view_count = sizeof(sxml_processor_library_parts) /
                                                sizeof(sxml_processor_library_parts[0])}};
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
        .clock = processor->clock};
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
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_initialize(
            sxml_processor_slice(&(sxml_processor_member_slice){
                .memory = memory,
                .offset = layout->function_library_offset,
                .end_offset = layout->requirements.byte_count}),
            &(sxml_function_library_configuration){
                .allocator = processor->allocator,
                .item_heap = processor->item_heap,
                .parts = {.native_function_views = sxml_processor_library_parts,
                          .native_function_view_count =
                              sizeof(sxml_processor_library_parts) /
                              sizeof(sxml_processor_library_parts[0])}},
            &processor->function_library);
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
        .name_pool = NULL,
        .type_registry = NULL,
        .item_heap = NULL,
        .function_library = NULL};
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
    sxml_function_library_finalize(processor->function_library);
    sxml_item_heap_finalize(processor->item_heap);
    sxml_type_registry_finalize(processor->type_registry);
    sxml_name_pool_finalize(processor->name_pool);
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

uint32_t sxml_processor_get_maximum_expression_depth(const sxml_processor *processor) {
    return processor->limits.maximum_expression_depth == 0
               ? SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH
               : processor->limits.maximum_expression_depth;
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
                .keeps_locations = false,
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
    if (configuration == NULL || configuration->processor == NULL) {
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
                                                   .loaded_document_capacity = 0};
    *loader = new_loader;
    return SXML_STATUS_OK;
}

void sxml_processor_document_loader_finalize(sxml_processor_document_loader *loader) {
    if (loader == NULL) {
        return;
    }
    const sxml_allocator allocator = loader->configuration.processor->allocator;
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
}

/* The document loaded or available for uri, borrowed; found is false when
   there is none. */
typedef struct sxml_document_lookup {
    bool is_found;
    sxml_item document_node;
} sxml_document_lookup;

static sxml_document_lookup sxml_loader_find_document(
    const sxml_processor_document_loader *loader, sxml_byte_view uri) {
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
    for (size_t document_index = 0; document_index < loader->loaded_document_count;
         document_index += 1) {
        const sxml_processor_loaded_document *document =
            &loader->loaded_documents[document_index];
        if (sxml_byte_view_equals((sxml_byte_view){.bytes = document->uri_bytes,
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
} sxml_loaded_document_request;

static sxml_status sxml_loader_keep_document(sxml_processor_document_loader *loader,
                                             const sxml_loaded_document_request *request) {
    const sxml_allocator allocator = loader->configuration.processor->allocator;
    if (loader->loaded_document_count == loader->loaded_document_capacity) {
        const size_t capacity =
            loader->loaded_document_capacity == 0 ? 4 : loader->loaded_document_capacity;
        size_t new_capacity = 0;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, 2, &new_capacity);
        if (status == SXML_STATUS_OK) {
            status = sxml_size_multiply(new_capacity, sizeof(sxml_processor_loaded_document),
                                        &byte_count);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_processor_loaded_document)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (loader->loaded_document_count > 0) {
            memcpy(memory, loader->loaded_documents,
                   loader->loaded_document_count * sizeof(sxml_processor_loaded_document));
        }
        sxml_allocator_release(allocator, loader->loaded_documents);
        loader->loaded_documents = memory;
        loader->loaded_document_capacity = new_capacity;
    }
    void *uri_memory = NULL;
    if (request->uri.byte_count > 0) {
        const sxml_status status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = request->uri.byte_count, .alignment = 1},
            &uri_memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(uri_memory, request->uri.bytes, request->uri.byte_count);
    }
    sxml_item_retain(request->document_node);
    loader->loaded_documents[loader->loaded_document_count] =
        (sxml_processor_loaded_document){.uri_bytes = uri_memory,
                                         .uri_byte_count = request->uri.byte_count,
                                         .document_node = request->document_node};
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

/* Raises FODC0002 with the resolver's description, when it gave one. */
static sxml_status sxml_loader_report_not_available(const sxml_processor_document_loader *loader,
                                                    const sxml_document_request *request,
                                                    sxml_byte_view description) {
    if (description.byte_count == 0) {
        return sxml_error_record_report(request->error_record,
                                        &sxml_document_not_available_error);
    }
    return sxml_item_heap_raise_error(
        loader->configuration.processor->item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FODC0002"),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .description = description,
            .has_value = false,
            .value = sxml_sequence_empty(),
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        request->error_record);
}

/* What the resolver was asked and what it said. */
typedef struct sxml_resolution_exchange {
    const sxml_document_request *request;
    sxml_byte_view absolute_uri;
    /* What the resolver delivered; NULL before it was asked. */
    const sxml_resource *resource;
} sxml_resolution_exchange;

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
    const sxml_status status = sxml_processor_parse_document(
        loader->configuration.processor,
        &(sxml_xml_parse_request){.input = resource->content,
                                  .source_name = document_uri,
                                  .document_uri = document_uri,
                                  .base_uri = document_uri,
                                  .encoding_name = resource->encoding_name,
                                  .diagnostic = NULL},
        document_node);
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        /* F&O 4.0, fn:doc: a resource that cannot be parsed is FODC0002. */
        return sxml_error_record_report(request->error_record,
                                        &sxml_document_not_well_formed_error);
    }
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
    case SXML_RESOURCE_FORM_BYTES:
        break;
    }
    return sxml_loader_parse_resource(loader, exchange, document_node);
}

/* Asks the resolver for the document and makes its node, owned. */
static sxml_status sxml_loader_resolve(sxml_processor_document_loader *loader,
                                       const sxml_resolution_exchange *exchange,
                                       sxml_item *document_node) {
    const sxml_resource_resolver resolver = loader->configuration.resource_resolver;
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
                                 .processor = loader->configuration.processor},
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
    const sxml_allocator allocator = loader->configuration.processor->allocator;
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
    const sxml_document_lookup lookup = sxml_loader_find_document(loader, resolution.uri);
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
        if (status == SXML_STATUS_OK) {
            status = sxml_loader_keep_document(
                loader, &(sxml_loaded_document_request){.uri = resolution.uri,
                                                        .document_node = node});
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
    const sxml_status status = sxml_processor_parse_document(
        loader->configuration.processor,
        &(sxml_xml_parse_request){.input = request->xml_bytes,
                                  .source_name = {.bytes = NULL, .byte_count = 0},
                                  .document_uri = {.bytes = NULL, .byte_count = 0},
                                  .base_uri = base_uri,
                                  .encoding_name = {.bytes = NULL, .byte_count = 0},
                                  .diagnostic = NULL},
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
