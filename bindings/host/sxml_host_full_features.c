/*
 * The features of the full links, sxml-full.wasm and the native target
 * sxml_host: every optional feature sxml_all_optional_features() names
 * (D189). Linked instead of sxml_host_core_features.c.
 */
#include "sxml_host.h"

sxml_optional_feature_view sxml_host_features(void) {
    return sxml_all_optional_features();
}
