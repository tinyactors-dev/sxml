#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xpath.h"

/* Compiles and evaluates one expression, and reports how it went. */
static void run(sxml_processor *processor, sxml_byte_view expression_text) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};
    sxml_xpath_expression *expression = NULL;
    sxml_sequence result = sxml_sequence_empty();
    printf("%.*s => ", (int)expression_text.byte_count,
           (const char *)expression_text.bytes);
    sxml_status status = sxml_xpath_expression_create(
        allocator,
        &(sxml_xpath_expression_configuration){
            .processor = processor,
            .expression_text = expression_text,
            .error_record = &error_record,
        },
        &expression);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression, &(sxml_xpath_evaluation_request){.error_record = &error_record},
            &result);
    }
    if (status == SXML_STATUS_OK) {
        status = print_sequence(processor, result, &error_record);
    }
    if (status != SXML_STATUS_OK) {
        print_failure(status, &error_record);
    }
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
}

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            /* Stop any evaluation after ten million steps. */
            .limits = {.maximum_evaluation_step_count = 10000000},
        },
        &processor);
    if (status != SXML_STATUS_OK) {
        return 1;
    }
    run(processor, TEXT("6 * 7"));
    run(processor, TEXT("1 +"));
    run(processor, TEXT("1 div 0"));
    run(processor, TEXT("error(xs:QName('err:LATCH0001'), 'no requirement req-12')"));
    run(processor, TEXT("some $n in 1 to 100000000000 satisfies $n = 0"));
    sxml_processor_destroy(processor);
    return 0;
}
