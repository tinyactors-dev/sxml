#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xpath.h"

/* The two drafts the same expression runs against, and room for each. */
static const char *const draft_files[] = {"vane-00.xml", "vane.xml"};
static unsigned char draft_contents[2][1 << 16];

/* The values of $keyword for each run. */
static const sxml_byte_view keywords[] = {TEXT("SHOULD"), TEXT("MAY")};

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &processor);

    /* Variable names are interned in the processor's name pool. */
    sxml_name_identifier keyword = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(processor),
            &(sxml_name_request){.namespace_uri = SXML_NAMESPACE_NONE,
                                 .local_name = TEXT("keyword")},
            &keyword);
    }

    /* Compile once, declaring $keyword as an external xs:string. */
    sxml_variable_declaration declaration = {
        .variable_name = keyword,
        .declared_type = {.item_type = {.index = SXML_TYPE_XS_STRING},
                          .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
    };
    sxml_xpath_expression *expression = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_create(
            allocator,
            &(sxml_xpath_expression_configuration){
                .processor = processor,
                .expression_text = TEXT("string-join(//t[bcp14 = $keyword]/@anchor, ' ')"),
                .static_context = {.variable_declarations = {
                                       .variable_declarations = &declaration,
                                       .variable_declaration_count = 1}},
                .error_record = &error_record,
            },
            &expression);
    }

    /* Run many times: each draft, with each key word. */
    for (size_t draft_index = 0; status == SXML_STATUS_OK && draft_index < 2;
         draft_index += 1) {
        sxml_item document = {0};
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){
                .input = read_file(draft_files[draft_index], draft_contents[draft_index],
                                   sizeof draft_contents[draft_index]),
                .diagnostic = &error_record.diagnostic},
            &document);
        sxml_sequence context = sxml_sequence_from_item(document);
        for (size_t keyword_index = 0; status == SXML_STATUS_OK && keyword_index < 2;
             keyword_index += 1) {
            /* A string is an item in the processor's item heap. */
            sxml_item value = {0};
            status = sxml_atomic_item_make(
                sxml_processor_get_item_heap(processor),
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = {.utf8 = keywords[keyword_index]}}},
                },
                &value);
            sxml_variable_binding binding = {
                .variable_name = keyword,
                .value = sxml_sequence_from_item(value),
            };
            sxml_sequence result = sxml_sequence_empty();
            if (status == SXML_STATUS_OK) {
                status = sxml_xpath_expression_evaluate(
                    expression,
                    &(sxml_xpath_evaluation_request){
                        .dynamic_context = {.has_context_value = true,
                                            .context_value = context,
                                            .variable_bindings = {
                                                .variable_bindings = &binding,
                                                .variable_binding_count = 1}},
                        .error_record = &error_record,
                    },
                    &result);
            }
            if (status == SXML_STATUS_OK) {
                printf("%s, %.*s: ", draft_files[draft_index],
                       (int)keywords[keyword_index].byte_count,
                       (const char *)keywords[keyword_index].bytes);
                status = print_sequence(processor, result, &error_record);
            }
            sxml_sequence_release(result);
            sxml_sequence_release(binding.value);
        }
        sxml_sequence_release(context);
    }

    if (status != SXML_STATUS_OK) {
        print_failure(status, &error_record);
    }
    sxml_xpath_expression_destroy(expression);
    sxml_processor_destroy(processor);
    return status == SXML_STATUS_OK ? 0 : 1;
}
