#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xpath.h"

/* Two documents that the same expression runs against. */
static const sxml_byte_view libraries[] = {
    TEXT("<books><book year='1979'>Godel, Escher, Bach</book>"
         "<book year='1968'>The Art of Computer Programming</book></books>"),
    TEXT("<books><book year='1965'>Dune</book>"
         "<book year='1969'>The Left Hand of Darkness</book></books>"),
};

/* The values of $before for each run. */
static const int64_t years[] = {1970, 1966};

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &processor);

    /* Variable names are interned in the processor's name pool. */
    sxml_name_identifier before = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(processor),
            &(sxml_name_request){.namespace_uri = SXML_NAMESPACE_NONE,
                                 .local_name = TEXT("before")},
            &before);
    }

    /* Compile once, declaring $before as an external xs:integer. */
    sxml_variable_declaration declaration = {
        .variable_name = before,
        .declared_type = {.item_type = {.index = SXML_TYPE_XS_INTEGER},
                          .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
    };
    sxml_xpath_expression *expression = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_create(
            allocator,
            &(sxml_xpath_expression_configuration){
                .processor = processor,
                .expression_text = TEXT("//book[@year < $before] ! string()"),
                .static_context = {.variable_declarations = {
                                       .variable_declarations = &declaration,
                                       .variable_declaration_count = 1}},
                .error_record = &error_record,
            },
            &expression);
    }

    /* Run many times: each document, with each year. */
    for (size_t library_index = 0; status == SXML_STATUS_OK && library_index < 2;
         library_index += 1) {
        sxml_item document = {0};
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){.input = libraries[library_index],
                                      .diagnostic = &error_record.diagnostic},
            &document);
        sxml_sequence context = sxml_sequence_from_item(document);
        for (size_t year_index = 0; status == SXML_STATUS_OK && year_index < 2;
             year_index += 1) {
            sxml_variable_binding binding = {
                .variable_name = before,
                .value = sxml_sequence_from_item(
                    sxml_atomic_item_from_integer(years[year_index])),
            };
            sxml_sequence result = sxml_sequence_empty();
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
            if (status == SXML_STATUS_OK) {
                printf("library %zu, before %lld:\n", library_index + 1,
                       (long long)years[year_index]);
                status = print_sequence(processor, result, &error_record);
            }
            sxml_sequence_release(result);
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
