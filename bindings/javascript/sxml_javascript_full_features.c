/*
 * The features of the full module, sxml-full.wasm: every optional feature
 * sxml has (D189). Linked instead of sxml_javascript_core_features.c.
 */
#include "sxml_javascript.h"

sxml_optional_feature_view sxml_javascript_module_features(void) {
    return sxml_all_optional_features();
}
