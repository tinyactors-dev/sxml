#pragma once

/*
 * Internal queries on the name pool (include/sxml/name.h) for modules that
 * keep tables indexed by its identifiers, such as the type registry and the
 * tree: identifiers are dense, so a table of this many entries covers every
 * identifier the pool has issued so far.
 */
#include "sxml/name.h"

/*
 * One more than the largest sxml_namespace_uri_identifier index the pool
 * has issued; at least SXML_NAMESPACE_WELL_KNOWN_COUNT.
 */
uint32_t sxml_name_pool_namespace_uri_limit(const sxml_name_pool *name_pool);

/*
 * One more than the largest sxml_namespace_prefix_identifier index the pool
 * has issued; at least 1 (identifier 0, "no prefix").
 */
uint32_t sxml_name_pool_namespace_prefix_limit(const sxml_name_pool *name_pool);

/*
 * One more than the largest sxml_name_identifier index the pool has issued;
 * at least 1 (identifier 0, "no name", is never issued).
 */
uint32_t sxml_name_pool_name_limit(const sxml_name_pool *name_pool);
