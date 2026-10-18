/*
 * The entry point of the XSD 1.1 driver: a copy of apps/sxml/main.c with the
 * driver's program table. Standard streams and files are reached only
 * through the platform layer of the sxml program (apps/sxml/platform.h,
 * D44).
 */
#include <stdalign.h>

#include "cli_run.h"
#include "platform.h"
#include "sxml/memory.h"
#include "sxml/version.h"
#include "xsd_driver.h"

/* argv is an external boundary: NUL-terminated strings, converted once
   into byte views. argv[0] is not used. */
int main(int argument_count, char **arguments) {
    if (sxml_platform_prepare_streams() != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    const sxml_allocator allocator = sxml_default_allocator();
    const size_t view_count =
        argument_count > 1 ? (size_t)argument_count - 1 : 0;
    sxml_byte_view *argument_views = NULL;
    if (view_count > 0) {
        size_t byte_count = 0;
        if (sxml_size_multiply(view_count, sizeof(sxml_byte_view),
                               &byte_count) != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        void *memory = NULL;
        if (sxml_allocator_allocate(allocator,
                                    (sxml_allocation_request){
                                        .byte_count = byte_count,
                                        .alignment = alignof(sxml_byte_view),
                                    },
                                    &memory) != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        argument_views = memory;
        for (size_t view_index = 0; view_index < view_count; view_index += 1) {
            argument_views[view_index] =
                sxml_byte_view_from_c_string(arguments[view_index + 1]);
        }
    }
    sxml_cli_exit_code exit_code = sxml_cli_run(
        sxml_xsd_test_program(),
        &(sxml_cli_run_request){
            .argument_list = {.arguments = argument_views,
                              .argument_count = view_count},
            .standard_output = sxml_platform_standard_output(),
            .standard_error = sxml_platform_standard_error(),
            .allocator = allocator,
            .version_text = sxml_byte_view_from_c_string(sxml_version_text()),
            .file_reader = sxml_platform_file_reader(),
        });
    sxml_allocator_release(allocator, argument_views);
    if (sxml_platform_flush_streams() != SXML_STATUS_OK &&
        (exit_code == SXML_CLI_EXIT_SUCCESS ||
         exit_code == SXML_CLI_EXIT_OPERATION_FAILED)) {
        exit_code = SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    return (int)exit_code;
}
