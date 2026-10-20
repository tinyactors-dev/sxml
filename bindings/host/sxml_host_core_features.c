/*
 * The features of the core module, sxml.wasm: none (D189). Linked instead
 * of sxml_host_full_features.c, so the core links no optional
 * feature (include/sxml/processor.h, "Optional features").
 */
#include "sxml_host.h"

sxml_optional_feature_view sxml_host_features(void) {
    return (sxml_optional_feature_view){.optional_features = NULL, .optional_feature_count = 0};
}
