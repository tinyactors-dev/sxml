#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xquery.h"

/* The only resources the query may read, held in memory. */
static const sxml_resource_entry resources[] = {
    {.uri = TEXT("https://example.org/books.xml"),
     .content = TEXT("<books><book id='dune'>Dune</book>"
                     "<book id='geb'>Godel, Escher, Bach</book></books>")},
    {.uri = TEXT("https://example.org/loans.xml"),
     .content = TEXT("<loans><loan book='dune' member='ada'/>"
                     "<loan book='geb' member='alan'/>"
                     "<loan book='dune' member='grace'/></loans>")},
};

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};
    sxml_resource_entry_view entries = {.resource_entries = resources,
                                        .resource_entry_count = 2};

    /* The resolver decides what fn:doc can see; nothing else is read. */
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = sxml_table_resource_resolver(&entries),
        },
        &processor);

    /* Relative URIs in the query resolve against its static base URI. */
    sxml_xquery_query *query = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_create(
            allocator,
            &(sxml_xquery_query_configuration){
                .processor = processor,
                .expression_text = TEXT(
                    "declare option output:indent 'yes';\n"
                    "<loans>{\n"
                    "  for $loan in doc('loans.xml')//loan\n"
                    "  let $book := doc('books.xml')//book[@id = $loan/@book]\n"
                    "  return <loan member='{ $loan/@member }'>{ string($book) }</loan>\n"
                    "}</loans>,\n"
                    "doc-available('secret.xml')"),
                .source_name = TEXT("report.xq"),
                .static_context = {.static_base_uri = TEXT("https://example.org/")},
                .error_record = &error_record,
            },
            &query);
    }

    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_evaluate(
            query, &(sxml_xquery_evaluation_request){.error_record = &error_record},
            &result);
    }

    /* Serialize as the query's output declarations ask. */
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters =
                    sxml_xquery_query_get_serialization_parameters(query),
                .error_record = &error_record,
            },
            stdout_stream());
        putchar('\n');
    }
    if (status != SXML_STATUS_OK) {
        print_failure(status, &error_record);
    }
    sxml_sequence_release(result);
    sxml_xquery_query_destroy(query);
    sxml_processor_destroy(processor);
    return status == SXML_STATUS_OK ? 0 : 1;
}
