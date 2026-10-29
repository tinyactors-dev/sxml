#include <stdio.h>
#include <string.h>

#include "output.h"
#include "sxml/processor.h"
#include "sxml/schematron.h"

/* Room for the schema's and the draft's bytes. */
static unsigned char schema_contents[1 << 14];
static unsigned char draft_contents[1 << 16];

static void print_text(sxml_byte_view text) {
    printf("%.*s", (int)text.byte_count, (const char *)text.bytes);
}

/* Receives each error in the schema while it is compiled. */
static sxml_status print_schema_error(void *state, const sxml_schema_report *report) {
    (void)state;
    print_text(report->location.source_name);
    printf(":%u:%u: ", (unsigned)report->location.line_number,
           (unsigned)report->location.column_number);
    print_text(report->code.local_name);
    printf(": ");
    print_text(report->description);
    putchar('\n');
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations schema_error_operations = {
    .receive = print_schema_error,
};

/* Receives each finding while the draft is validated. */
static sxml_status print_finding(void *state, const sxml_schematron_report *report) {
    (void)state;
    printf("%u:%u %s ", (unsigned)report->node_location.line_number,
           (unsigned)report->node_location.column_number,
           report->is_error ? "error" : "note");
    print_text(report->id);
    printf(": ");
    print_text(report->message);
    putchar('\n');
    for (size_t index = 0; index < report->diagnostics.diagnostic_count; index++) {
        printf("    ");
        print_text(report->diagnostics.diagnostics[index].text);
        putchar('\n');
    }
    return SXML_STATUS_OK;
}

static const sxml_schematron_report_sink_operations finding_operations = {
    .receive = print_finding,
};

int main(int argument_count, char **arguments) {
    /* The schema: review.sch, or the file the command line names. */
    const char *schema_path = argument_count > 1 ? arguments[1] : "review.sch";
    sxml_byte_view schema_name = {.bytes = (const uint8_t *)schema_path,
                                  .byte_count = strlen(schema_path)};
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &processor);

    /* Compile the schema once; every error goes to the sink. */
    sxml_schematron *schematron = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_schematron_create(
            allocator,
            &(sxml_schematron_configuration){
                .processor = processor,
                .document_text = read_file(schema_path, schema_contents,
                                           sizeof schema_contents),
                .document_uri = schema_name,
                .report_sink = {.operations = &schema_error_operations},
                .error_record = &error_record,
            },
            &schematron);
    }

    /* Parse the draft, with its name, so findings have lines and columns. */
    sxml_item draft = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){
                .input = read_file("vane.xml", draft_contents, sizeof draft_contents),
                .source_name = TEXT("vane.xml"),
                .diagnostic = &error_record.diagnostic,
            },
            &draft);
    }

    /* Validate it: any number of documents could follow. */
    sxml_schematron_outcome outcome = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_schematron_validate_node(
            schematron,
            &(sxml_schematron_validation_request){
                .node = draft,
                .report_sink = {.operations = &finding_operations},
                .error_record = &error_record,
            },
            &outcome);
    }
    if (status == SXML_STATUS_OK) {
        printf("%zu findings, %zu of them errors\n", outcome.finding_count,
               outcome.error_finding_count);
    } else {
        print_failure(status, &error_record);
    }

    sxml_item_release(draft);
    sxml_schematron_destroy(schematron);
    sxml_processor_destroy(processor);
    return status != SXML_STATUS_OK || outcome.error_finding_count > 0 ? 1 : 0;
}
