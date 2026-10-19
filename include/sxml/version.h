#pragma once

#include "sxml/export.h"

/*
 * Library version.
 *
 * The version follows Semantic Versioning. The macros describe the headers a
 * program was compiled against; sxml_version_text describes the library it
 * runs against, which can differ when the library is shared.
 *
 * Stability: from release 0.2.0 on, the public headers (include/sxml/) are
 * stable (D47, D138): a release adds declarations, and changes or removes
 * one only with a decision record and a new major version (before 1.0.0,
 * minor version). xquery.h, xslt.h and schema.h were provisional until
 * the Phase 3, 4 and 5 gates (D151, D165) and are stable since (D161,
 * D169, D182).
 * docs/RELEASE.md describes a release.
 */

#define SXML_VERSION_MAJOR 0
#define SXML_VERSION_MINOR 2
#define SXML_VERSION_PATCH 0

/*
 * A version as one integer, for comparisons in #if: major * 1,000,000 +
 * minor * 1,000 + patch, so 0.2.0 is 2000. Each part is 0 to 999.
 */
#define SXML_VERSION_ENCODE(major, minor, patch) \
    (((major) * 1000000L) + ((minor) * 1000L) + (patch))

/* The version of these headers as one integer (SXML_VERSION_ENCODE). */
#define SXML_VERSION_NUMBER \
    SXML_VERSION_ENCODE(SXML_VERSION_MAJOR, SXML_VERSION_MINOR, SXML_VERSION_PATCH)

/*
 * The API version: the release whose interface these headers declare,
 * encoded by SXML_VERSION_ENCODE. Release 0.2.0 is the first public
 * release; from it on, a release that adds declarations raises the API
 * version, and one that changes or removes a declaration raises the major
 * version (Semantic Versioning; before 1.0.0, the minor version). A host
 * tests for an interface with
 *
 *     #if SXML_API_VERSION >= SXML_VERSION_ENCODE(0, 2, 0)
 */
#define SXML_API_VERSION SXML_VERSION_ENCODE(0, 2, 0)

/*
 * Returns the library version as text, for example "0.2.0".
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
