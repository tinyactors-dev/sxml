/*
 * The JNode accessor part of the function library (packet 2.6, D115):
 * fn:jkey and fn:jvalue (F&O 4.0, sections 16.1.2 and 16.1.3), which read
 * the ·jkey· and ·jvalue· properties of a JNode (XDM 4.0, "JNodes"; D70).
 * fn:jtree stays in src/function_library_json.c.
 *
 * Golden file: src/function_library_math.c (rows, leaf bodies); rows as
 * in src/function_library_internal.h.
 *
 * Arguments reach the bodies coerced to jnode()? (XPath 4.0, section 3.4);
 * a call without $input passes the context value, which the call coerces
 * the same way (XPDY0002 when it is absent, XPTY0004 when it is not a
 * JNode).
 */
#include "function_library_core_internal.h"

/* ------------------------------------------------------------- Accessors */

/* fn:jkey($input as jnode()? := .) as xs:anyAtomicType? (F&O 4.0, section
   16.1.2): the key of the entry or the 1-based member position; () for
   () and for a root JNode, which has no ·jkey·. */
static sxml_status call_jkey(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item input;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &input);
    *result = sxml_sequence_empty();
    if (status != SXML_STATUS_OK || !input.has_item) {
        return status;
    }
    sxml_json_node_description description;
    status = sxml_json_node_describe(input.item, &description);
    if (status != SXML_STATUS_OK || !description.has_parent) {
        return status;
    }
    sxml_item_retain(description.json_key);
    *result = sxml_sequence_from_item(description.json_key);
    return SXML_STATUS_OK;
}

/* fn:jvalue($input as jnode()? := .) as item()* (F&O 4.0, section
   16.1.3): the ·jvalue· as it is (contained JNodes are not unwrapped). */
static sxml_status call_jvalue(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item input;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &input);
    *result = sxml_sequence_empty();
    if (status != SXML_STATUS_OK || !input.has_item) {
        return status;
    }
    sxml_json_node_description description;
    status = sxml_json_node_describe(input.item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_retain(description.json_value);
    *result = description.json_value;
    return SXML_STATUS_OK;
}

/* The JNode J the item axis selects from an origin G whose ·jvalue· is
   one item (XPath 4.0, section 4.7.5.1): ·jkey· 1, ·jvalue· ·jvalue·(G),
   ·jparent· G, not a child of G. The step lowers to a call of this
   internal row only for such an origin (src/xpath_lower.c). */
static sxml_status call_item_node(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item input;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &input);
    *result = sxml_sequence_empty();
    if (status != SXML_STATUS_OK || !input.has_item) {
        return status;
    }
    sxml_item json_node = {.representation = 0};
    status = sxml_json_node_make_item_node(call->item_heap, input.item, &json_node);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(json_node);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter jnode_parameters[] = {
    SXML_CONTEXT_PARAMETER("input", "jnode()?"),
};

static const sxml_native_function jnode_functions[] = {
    SXML_FUNCTION_ROW("jkey", jnode_parameters, "xs:anyAtomicType?", 0, call_jkey),
    SXML_FUNCTION_ROW("jvalue", jnode_parameters, "item()*", 0, call_jvalue),
};

static const sxml_function_parameter item_node_parameters[] = {
    SXML_REQUIRED_PARAMETER("origin", "jnode()"),
};

const sxml_native_function sxml_item_node_function =
    SXML_FUNCTION_ROW("item-node", item_node_parameters, "jnode()", 0, call_item_node);

const sxml_native_function_view sxml_function_library_jnode =
    SXML_LIBRARY_PART(jnode_functions);
