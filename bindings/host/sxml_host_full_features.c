/*
 * The features of every link, sxml.wasm and the native target sxml_host:
 * every optional feature sxml_all_optional_features() names (D189, D272).
 */
#include "sxml_host.h"

sxml_optional_feature_view sxml_host_features(void) {
    return sxml_all_optional_features();
}
