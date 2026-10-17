/*
 * Environments of the XSLT 4.0 test suite (xslt_environment.h). Imitates
 * tests/qt4/driver/environment.c; element and attribute names follow
 * third_party/xslt40-test/admin/catalog-schema.xsd.
 */
#include "xslt_environment.h"

#include <stdalign.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

sxml_xslt_test_output sxml_xslt_test_output_absent(void) {
    return (sxml_xslt_test_output){.is_present = false,
                                   .is_serialized = false,
                                   .has_tree_choice = false,
                                   .builds_tree = false,
                                   .is_well_formed = true,
                                   .file_path = sxml_xslt_test_empty_text,
                                   .result_variable =
                                       sxml_xslt_test_empty_text};
}

sxml_xslt_test_environment sxml_xslt_test_environment_absent(void) {
    return (sxml_xslt_test_environment){
        .shared = sxml_qt4_environment_absent(),
        .source_details = {.source_details = NULL, .source_detail_count = 0},
        .parameters = {.parameters = NULL, .parameter_count = 0},
        .modules = {.modules = NULL, .module_count = 0},
        .output = sxml_xslt_test_output_absent(),
    };
}

/* Reads one attribute; a shorter call for the readers below. */
typedef struct sxml_xslt_test_attribute_target {
    sxml_byte_view local_name;
    sxml_byte_view *value;
} sxml_xslt_test_attribute_target;

typedef struct sxml_xslt_test_attribute_targets {
    const sxml_xslt_test_attribute_target *targets;
    size_t target_count;
} sxml_xslt_test_attribute_targets;

static sxml_status
sxml_xslt_test_read_attributes(const sxml_xslt_test_element_reader *reader,
                               sxml_node_index element,
                               sxml_xslt_test_attribute_targets targets) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t target_index = 0;
         status == SXML_STATUS_OK && target_index < targets.target_count;
         target_index += 1) {
        status = sxml_xslt_test_read_attribute(
            reader,
            (sxml_qt4_attribute_lookup){
                .element = element,
                .local_name = targets.targets[target_index].local_name},
            targets.targets[target_index].value);
    }
    return status;
}

/* ---------------------------------------------------------------- Modules */

static sxml_status
sxml_xslt_test_read_module(const sxml_xslt_test_element_reader *reader,
                           sxml_node_index element,
                           sxml_xslt_test_module *module) {
    sxml_byte_view role = sxml_xslt_test_empty_text;
    const sxml_xslt_test_attribute_target targets[] = {
        {SXML_TEXT("uri"), &module->uri},
        {SXML_TEXT("name"), &module->package_name},
        {SXML_TEXT("package-version"), &module->package_version},
        {SXML_TEXT("xml-version"), &module->xml_version},
        {SXML_TEXT("role"), &role},
    };
    sxml_status status = sxml_xslt_test_read_attributes(
        reader, element,
        (sxml_xslt_test_attribute_targets){
            .targets = targets,
            .target_count = sizeof targets / sizeof targets[0]});
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_file_attribute(reader, element,
                                                    &module->file_path);
    }
    if (sxml_byte_view_equals(role, SXML_TEXT("principal"))) {
        module->module_role = SXML_XSLT_TEST_MODULE_ROLE_PRINCIPAL;
    } else if (sxml_byte_view_equals(role, SXML_TEXT("secondary"))) {
        module->module_role = SXML_XSLT_TEST_MODULE_ROLE_SECONDARY;
    } else {
        module->module_role = SXML_XSLT_TEST_MODULE_ROLE_UNSPECIFIED;
    }
    return status;
}

/* True for the local names of module elements; writes the kind. */
static bool sxml_xslt_test_module_kind_of(sxml_byte_view local_name,
                                          sxml_xslt_test_module_kind *kind) {
    if (sxml_byte_view_equals(local_name, SXML_TEXT("stylesheet"))) {
        *kind = SXML_XSLT_TEST_MODULE_KIND_STYLESHEET;
        return true;
    }
    if (sxml_byte_view_equals(local_name, SXML_TEXT("package"))) {
        *kind = SXML_XSLT_TEST_MODULE_KIND_PACKAGE;
        return true;
    }
    return false;
}

sxml_status
sxml_xslt_test_read_modules(const sxml_xslt_test_element_reader *reader,
                            sxml_node_index parent,
                            sxml_xslt_test_module_view *modules) {
    sxml_xslt_test_module *filled = NULL;
    size_t module_count = 0;
    for (size_t pass_index = 0; pass_index < 2; pass_index += 1) {
        sxml_qt4_element_cursor cursor = {.document = reader->document,
                                          .has_next_node = false,
                                          .next_node = {.index = 0}};
        sxml_status status =
            sxml_qt4_element_cursor_start(reader->document, parent, &cursor);
        size_t filled_count = 0;
        while (status == SXML_STATUS_OK) {
            sxml_qt4_element_step step = {
                .has_element = false,
                .element = {.index = 0},
                .local_name = sxml_xslt_test_empty_text};
            status = sxml_qt4_element_cursor_next(&cursor, &step);
            sxml_xslt_test_module_kind kind =
                SXML_XSLT_TEST_MODULE_KIND_STYLESHEET;
            if (status != SXML_STATUS_OK || !step.has_element) {
                break;
            }
            if (!sxml_xslt_test_module_kind_of(step.local_name, &kind)) {
                continue;
            }
            if (filled != NULL) {
                sxml_xslt_test_module *module = &filled[filled_count];
                *module = (sxml_xslt_test_module){
                    .file_path = sxml_xslt_test_empty_text,
                    .uri = sxml_xslt_test_empty_text,
                    .package_name = sxml_xslt_test_empty_text,
                    .package_version = sxml_xslt_test_empty_text,
                    .xml_version = sxml_xslt_test_empty_text,
                    .module_kind = kind,
                    .module_role = SXML_XSLT_TEST_MODULE_ROLE_UNSPECIFIED};
                status =
                    sxml_xslt_test_read_module(reader, step.element, module);
            }
            filled_count += 1;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        module_count = filled_count;
        if (pass_index == 1 || filled_count == 0) {
            break;
        }
        void *memory = NULL;
        status = sxml_xslt_test_allocate_array(
            reader->arena,
            (sxml_xslt_test_array_request){
                .element_count = filled_count,
                .element_byte_count = sizeof(sxml_xslt_test_module),
                .alignment = alignof(sxml_xslt_test_module)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        filled = memory;
    }
    *modules = (sxml_xslt_test_module_view){.modules = filled,
                                            .module_count = module_count};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Parameters */

static sxml_status
sxml_xslt_test_read_parameter(const sxml_xslt_test_element_reader *reader,
                              sxml_node_index element,
                              sxml_xslt_test_parameter *parameter) {
    sxml_status status = sxml_xslt_test_read_name(
        reader,
        (sxml_qt4_attribute_lookup){.element = element,
                                    .local_name = SXML_TEXT("name")},
        &parameter->name);
    const sxml_xslt_test_attribute_target targets[] = {
        {SXML_TEXT("select"), &parameter->select},
        {SXML_TEXT("as"), &parameter->declared_type},
    };
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_attributes(
            reader, element,
            (sxml_xslt_test_attribute_targets){
                .targets = targets,
                .target_count = sizeof targets / sizeof targets[0]});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = element,
                                         .local_name = SXML_TEXT("static"),
                                         .default_value = false,
                                         .value = &parameter->is_static});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = element,
                                         .local_name = SXML_TEXT("tunnel"),
                                         .default_value = false,
                                         .value = &parameter->is_tunnel});
    }
    return status;
}

sxml_status
sxml_xslt_test_read_parameters(const sxml_xslt_test_element_reader *reader,
                               sxml_node_index parent,
                               sxml_xslt_test_parameter_view *parameters) {
    sxml_xslt_test_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_xslt_test_collect_children(
        reader,
        (sxml_xslt_test_children_request){.parent = parent,
                                          .local_name = SXML_TEXT("param")},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_allocate_array(
            reader->arena,
            (sxml_xslt_test_array_request){
                .element_count = nodes.node_count,
                .element_byte_count = sizeof(sxml_xslt_test_parameter),
                .alignment = alignof(sxml_xslt_test_parameter)},
            &memory);
    }
    sxml_xslt_test_parameter *read = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && read != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        read[node_index] =
            (sxml_xslt_test_parameter){.name = sxml_xslt_test_no_name(),
                                       .select = sxml_xslt_test_empty_text,
                                       .declared_type =
                                           sxml_xslt_test_empty_text,
                                       .is_static = false,
                                       .is_tunnel = false};
        status = sxml_xslt_test_read_parameter(reader, nodes.nodes[node_index],
                                               &read[node_index]);
    }
    if (status == SXML_STATUS_OK) {
        *parameters = (sxml_xslt_test_parameter_view){
            .parameters = read, .parameter_count = nodes.node_count};
    }
    return status;
}

/* ----------------------------------------------------------------- Output */

sxml_status
sxml_xslt_test_read_output(const sxml_xslt_test_element_reader *reader,
                           sxml_node_index parent,
                           sxml_xslt_test_output *output) {
    sxml_xslt_test_child_lookup lookup = {.element = {.index = 0},
                                          .is_found = false};
    sxml_status status = sxml_xslt_test_find_child(
        reader,
        (sxml_xslt_test_children_request){.parent = parent,
                                          .local_name = SXML_TEXT("output")},
        &lookup);
    sxml_xslt_test_output result = sxml_xslt_test_output_absent();
    if (status != SXML_STATUS_OK || !lookup.is_found) {
        if (status == SXML_STATUS_OK) {
            *output = result;
        }
        return status;
    }
    const sxml_node_index element = lookup.element;
    result.is_present = true;
    sxml_byte_view tree_text = sxml_xslt_test_empty_text;
    const sxml_xslt_test_attribute_target targets[] = {
        {SXML_TEXT("tree"), &tree_text},
        {SXML_TEXT("result-var"), &result.result_variable},
    };
    status = sxml_xslt_test_read_attributes(
        reader, element,
        (sxml_xslt_test_attribute_targets){
            .targets = targets,
            .target_count = sizeof targets / sizeof targets[0]});
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = element,
                                         .local_name = SXML_TEXT("serialize"),
                                         .default_value = false,
                                         .value = &result.is_serialized});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = element,
                                         .local_name = SXML_TEXT("tree"),
                                         .default_value = false,
                                         .value = &result.builds_tree});
        result.has_tree_choice = tree_text.byte_count != 0;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = element,
                                         .local_name = SXML_TEXT("well-formed"),
                                         .default_value = true,
                                         .value = &result.is_well_formed});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_file_attribute(reader, element,
                                                    &result.file_path);
    }
    if (status == SXML_STATUS_OK) {
        *output = result;
    }
    return status;
}

/* ---------------------------------------------------------------- Sources */

static sxml_status
sxml_xslt_test_read_source_detail(const sxml_xslt_test_element_reader *reader,
                                  sxml_node_index element,
                                  sxml_xslt_test_source_detail *detail) {
    const sxml_xslt_test_attribute_target targets[] = {
        {SXML_TEXT("select"), &detail->select},
        {SXML_TEXT("xml-version"), &detail->xml_version},
    };
    sxml_status status = sxml_xslt_test_read_attributes(
        reader, element,
        (sxml_xslt_test_attribute_targets){
            .targets = targets,
            .target_count = sizeof targets / sizeof targets[0]});
    const struct {
        sxml_byte_view local_name;
        bool *value;
    } flags[] = {
        {SXML_TEXT("streaming"), &detail->is_streamed},
        {SXML_TEXT("dtd"), &detail->needs_dtd},
        {SXML_TEXT("defines-stylesheet"), &detail->defines_stylesheet},
        {SXML_TEXT("xinclude"), &detail->needs_xinclude},
    };
    for (size_t flag_index = 0;
         status == SXML_STATUS_OK && flag_index < sizeof flags / sizeof flags[0];
         flag_index += 1) {
        status = sxml_xslt_test_read_flag(
            reader,
            (sxml_xslt_test_flag_lookup){.element = element,
                                         .local_name =
                                             flags[flag_index].local_name,
                                         .default_value = false,
                                         .value = flags[flag_index].value});
    }
    sxml_xslt_test_child_lookup content = {.element = {.index = 0},
                                           .is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_find_child(
            reader,
            (sxml_xslt_test_children_request){.parent = element,
                                              .local_name =
                                                  SXML_TEXT("content")},
            &content);
    }
    if (status == SXML_STATUS_OK && content.is_found) {
        detail->has_content = true;
        status = sxml_qt4_document_get_text(
            reader->document,
            &(sxml_qt4_text_request){.element = content.element,
                                     .arena = reader->arena},
            &detail->content);
    }
    return status;
}

static sxml_status
sxml_xslt_test_read_source_details(const sxml_xslt_test_element_reader *reader,
                                   sxml_node_index parent,
                                   sxml_xslt_test_source_detail_view *details) {
    sxml_xslt_test_node_list nodes = {.nodes = NULL, .node_count = 0};
    sxml_status status = sxml_xslt_test_collect_children(
        reader,
        (sxml_xslt_test_children_request){.parent = parent,
                                          .local_name = SXML_TEXT("source")},
        &nodes);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_allocate_array(
            reader->arena,
            (sxml_xslt_test_array_request){
                .element_count = nodes.node_count,
                .element_byte_count = sizeof(sxml_xslt_test_source_detail),
                .alignment = alignof(sxml_xslt_test_source_detail)},
            &memory);
    }
    sxml_xslt_test_source_detail *read = memory;
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && read != NULL &&
         node_index < nodes.node_count;
         node_index += 1) {
        read[node_index] = (sxml_xslt_test_source_detail){
            .has_content = false,
            .content = sxml_xslt_test_empty_text,
            .select = sxml_xslt_test_empty_text,
            .xml_version = sxml_xslt_test_empty_text,
            .is_streamed = false,
            .needs_dtd = false,
            .defines_stylesheet = false,
            .needs_xinclude = false};
        status = sxml_xslt_test_read_source_detail(
            reader, nodes.nodes[node_index], &read[node_index]);
    }
    if (status == SXML_STATUS_OK) {
        *details = (sxml_xslt_test_source_detail_view){
            .source_details = read, .source_detail_count = nodes.node_count};
    }
    return status;
}

/* ------------------------------------------------------------ Environment */

sxml_status
sxml_xslt_test_environment_read(const sxml_qt4_environment_request *request,
                                sxml_qt4_problem *problem,
                                sxml_xslt_test_environment *environment) {
    sxml_xslt_test_environment result = sxml_xslt_test_environment_absent();
    sxml_status status = sxml_qt4_environment_read(request, &result.shared);
    const sxml_xslt_test_element_reader reader = {
        .document = request->document,
        .containing_file = request->containing_file,
        .arena = request->arena,
        .problem = problem};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_source_details(&reader, request->element,
                                                    &result.source_details);
    }
    if (status == SXML_STATUS_OK &&
        result.source_details.source_detail_count !=
            result.shared.sources.source_count) {
        /* Both readers walk the same <source> children. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_parameters(&reader, request->element,
                                                &result.parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_modules(&reader, request->element,
                                             &result.modules);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_read_output(&reader, request->element,
                                            &result.output);
    }
    if (status == SXML_STATUS_OK) {
        *environment = result;
    }
    return status;
}
