#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xpath.h"

int main(void) {
    /* A counting allocator passes allocations on to the default
       allocator and counts them. Any sxml_allocator works the same way:
       a pool, an arena, or memory with a fixed budget. */
    sxml_counting_allocator *counting_allocator = NULL;
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator()},
        &counting_allocator);
    if (status != SXML_STATUS_OK) {
        return 1;
    }
    sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_error_record error_record = {0};

    sxml_processor *processor = NULL;
    status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &processor);
    sxml_xpath_expression *expression = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_create(
            allocator,
            &(sxml_xpath_expression_configuration){
                .processor = processor,
                .expression_text = TEXT("sum((1 to 1000000) ! (. * .))"),
                .error_record = &error_record,
            },
            &expression);
    }
    sxml_sequence result = sxml_sequence_empty();
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
    sxml_processor_destroy(processor);

    /* Everything sxml allocated has been given back. */
    sxml_allocation_counts counts = sxml_counting_allocator_read_counts(counting_allocator);
    printf("allocations: %llu, still live: %llu\n",
           (unsigned long long)counts.attempted_allocation_count,
           (unsigned long long)counts.live_allocation_count);
    sxml_counting_allocator_destroy(counting_allocator);
    return status == SXML_STATUS_OK ? 0 : 1;
}
