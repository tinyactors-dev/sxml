#include <stdio.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/xquery.h"

/* Room for the draft's bytes. */
static unsigned char draft_contents[1 << 16];

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};

    /* The only resource the query may read: the draft, held in memory. */
    sxml_resource_entry resources[] = {
        {.uri = TEXT("https://example.org/drafts/vane.xml"),
         .content = read_file("vane.xml", draft_contents, sizeof draft_contents)},
    };
    sxml_resource_entry_view entries = {.resource_entries = resources,
                                        .resource_entry_count = 1};

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
                    "<keywords>{\n"
                    "  for $t in doc('vane.xml')//t[@anchor]\n"
                    "  group by $keyword := string($t/bcp14)\n"
                    "  order by $keyword\n"
                    "  return <keyword name='{ $keyword }' count='{ count($t) }'/>\n"
                    "}</keywords>,\n"
                    "doc-available('vane-00.xml')"),
                .source_name = TEXT("keywords.xq"),
                .static_context = {.static_base_uri = TEXT("https://example.org/drafts/")},
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
