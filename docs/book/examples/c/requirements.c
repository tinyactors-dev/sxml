#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xpath.h"

/* Room for the draft's bytes. */
static unsigned char draft_contents[1 << 16];

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};

    /* 1. A processor owns names, types and values. */
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &processor);

    /* 2. Parse a document: the program reads the file, sxml parses it. */
    sxml_item document = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){
                .input = read_file("latch.xml", draft_contents, sizeof draft_contents),
                .diagnostic = &error_record.diagnostic,
            },
            &document);
    }
    sxml_sequence context = sxml_sequence_from_item(document);

    /* 3. Compile an expression. */
    sxml_xpath_expression *expression = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_create(
            allocator,
            &(sxml_xpath_expression_configuration){
                .processor = processor,
                .expression_text = TEXT("//t[bcp14 = 'MUST'] ! `{@anchor} in {ancestor::section[1]/name}`"),
                .error_record = &error_record,
            },
            &expression);
    }

    /* 4. Evaluate it, with the document as the context item. */
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){
                .dynamic_context = {.has_context_value = true,
                                    .context_value = context},
                .error_record = &error_record,
            },
            &result);
    }

    /* 5. Write the result. */
    if (status == SXML_STATUS_OK) {
        status = print_sequence(processor, result, &error_record);
    }
    if (status != SXML_STATUS_OK) {
        print_failure(status, &error_record);
    }

    /* 6. Release everything, the processor last. */
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
    sxml_sequence_release(context);
    sxml_processor_destroy(processor);
    return status == SXML_STATUS_OK ? 0 : 1;
}
