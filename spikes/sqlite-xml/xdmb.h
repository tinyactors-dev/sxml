#pragma once
/*
 * XDMB, a spike: a binary encoding of an XTree that sxml can query in
 * place. See README.md for the layout and why it looks like this.
 *
 * - xdmb_encode writes any sxml_tree (the arena tree the parser builds,
 *   or another XDMB tree) as bytes. The same XDM content always gives the
 *   same bytes: tables in order of first occurrence in document order,
 *   namespace bindings sorted, minimal field widths.
 * - xdmb_open makes an sxml_tree whose operations read the bytes where
 *   they are: no node is decoded ahead, nothing is copied but the name
 *   table (interned into the heap's name pool) and the namespace sets.
 */
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/* Bytes the encoder made; free with xdmb_free (malloc'd). */
typedef struct xdmb_bytes {
    uint8_t *bytes;
    size_t byte_count;
} xdmb_bytes;

sxml_status xdmb_encode(sxml_tree *tree, xdmb_bytes *encoded);
void xdmb_free(xdmb_bytes *encoded);

/* True when bytes start with the XDMB magic. */
int xdmb_is_encoded(sxml_byte_view bytes);

typedef struct xdmb_open_request {
    /* Borrowed: must outlive the tree. */
    sxml_byte_view bytes;
    sxml_item_heap *item_heap;
    sxml_allocator allocator;
} xdmb_open_request;

/* Makes a tree over request->bytes with one reference, owned by the
   caller; the tree frees itself when its last reference goes. */
sxml_status xdmb_open(const xdmb_open_request *request, sxml_tree **tree);
