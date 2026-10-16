#pragma once

#include "sxml/export.h"

/*
 * Library version.
 *
 * The version follows Semantic Versioning. The macros describe the headers a
 * program was compiled against; sxml_version_text describes the library it
 * runs against, which can differ when the library is shared.
 */

#define SXML_VERSION_MAJOR 0
#define SXML_VERSION_MINOR 0
#define SXML_VERSION_PATCH 0

/*
 * Returns the library version as text, for example "0.0.0".
 *
 * The result is a NUL-terminated ASCII string with static storage duration,
 * owned by the library; the caller must not modify or release it. A
 * NUL-terminated string is used because this is an external boundary meant for
 * logs and bindings (docs/C_GUIDELINES.md, "Memory and buffers"); a byte view
 * variant can follow once include/sxml/memory.h exists.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT const char *sxml_version_text(void);
