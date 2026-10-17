#pragma once

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/xdm.h"

/*
 * Map operations beyond include/sxml/xdm.h that the function library needs
 * (src/xdm_map.c, docs/DECISIONS.md D61, D90). Internal to sxml.
 *
 * Thread safety: as sxml_map_put.
 */

/*
 * Makes the map with update->key set to update->value as its first entry,
 * the other entries keeping their order; an entry with an equal key is
 * dropped, and its key with it. Writes the map, owned, to *map. O(log n) on
 * a persistent map (a result of sxml_map_put, sxml_map_remove or this
 * function); an entry block or record is converted once, O(n log n), unless
 * the result has at most 8 entries. map:merge uses it to merge earlier maps
 * into a later, larger one (D90). Failures as for sxml_map_put.
 */
SXML_NODISCARD sxml_status sxml_map_put_first(sxml_item_heap *item_heap,
                                              const sxml_map_update *update,
                                              sxml_item *map);
