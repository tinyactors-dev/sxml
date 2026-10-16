#include "sxml/version.h"

#define SXML_STRINGIFY_TOKEN(token) #token
#define SXML_STRINGIFY(token) SXML_STRINGIFY_TOKEN(token)

const char *sxml_version_text(void) {
    return SXML_STRINGIFY(SXML_VERSION_MAJOR) "." SXML_STRINGIFY(
        SXML_VERSION_MINOR) "." SXML_STRINGIFY(SXML_VERSION_PATCH);
}
