/*
 * The DTD of a document as XSD 1.1 schema documents (src/dtd_internal.h;
 * packet 7.14b, docs/DECISIONS.md D199).
 *
 * Golden file: src/serialize.c (text written through one writer that
 * keeps the first failure); src/memory.c for layout.
 *
 * XML 1.0 (Fifth Edition) section 3 defines the validity of an element by
 * its element type declaration (3.2) and attribute-list declarations
 * (3.3); XSD 1.1 Part 1 has a component for each part: an element type is
 * a global element declaration (3.3) whose complex type (3.4) has the
 * content model as model groups (3.8) and the attribute definitions as
 * attribute uses (3.5); notations are notation declarations (3.14). The
 * translation:
 * - EMPTY: empty content; ANY: mixed content with a strict wildcard, so
 *   that every child must be declared (VC: Element Valid, clause 4);
 *   Mixed: mixed content with a repeated choice of the names; children:
 *   xs:sequence and xs:choice with ?, * and + as minOccurs and maxOccurs.
 *   An element type named in a content model but not declared gets a
 *   declaration of a type no element matches (VC: Element Valid: every
 *   element must be declared).
 * - CDATA: xs:string; ID, IDREF, IDREFS, ENTITY, ENTITIES, NMTOKEN,
 *   NMTOKENS: the XSD types of those names (XSD 1.1 Part 2, 3.4), which
 *   check the same constraints (VC: ID, IDREF, Entity Name, Name Token);
 *   NOTATION: a restriction of xs:NOTATION and enumerations: of
 *   xs:NMTOKEN, both by xs:enumeration (VC: Notation Attributes,
 *   Enumeration); #REQUIRED: use="required"; #FIXED: fixed; a default:
 *   default (VC: Required Attribute, Fixed Attribute Default, Attribute
 *   Default Value Syntactically Correct).
 * - Attributes in the XML namespace match a skip wildcard where the
 *   element type declares one (the parser has applied their defaults);
 *   namespace declarations are not attributes (XDM 4.0).
 * Element types and prefixed attribute names are resolved through the
 * bindings given: each namespace gets its own document, which imports the
 * others.
 */
#include "dtd_internal.h"

#include <string.h>

/* The namespaces of XSD and of XML. */
#define SXML_DTD_XSD_NAMESPACE "http://www.w3.org/2001/XMLSchema"
#define SXML_DTD_XML_NAMESPACE "http://www.w3.org/XML/1998/namespace"

/* Text appended to one buffer, keeping the first failure. */
typedef struct sxml_dtd_writer {
    sxml_xml_buffer *output;
    sxml_status status;
} sxml_dtd_writer;

static void sxml_dtd_write_view(sxml_dtd_writer *writer, sxml_byte_view text) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_xml_buffer_append(writer->output, text);
    }
}

/* Writes a string literal. */
#define SXML_DTD_WRITE(writer, literal)                                                            \
    sxml_dtd_write_view((writer), SXML_BYTE_VIEW_FROM_LITERAL(literal))

/* Writes text as attribute-value content: the characters an attribute
   value cannot hold as they are become character references, so that
   attribute-value normalization gives the text back (XML 1.0, 3.3.3). */
static void sxml_dtd_write_escaped(sxml_dtd_writer *writer, sxml_byte_view text) {
    size_t start = 0;
    for (size_t byte_index = 0; byte_index <= text.byte_count; byte_index++) {
        sxml_byte_view reference = {.bytes = NULL, .byte_count = 0};
        if (byte_index < text.byte_count) {
            switch (text.bytes[byte_index]) {
            case '&':
                reference = SXML_BYTE_VIEW_FROM_LITERAL("&amp;");
                break;
            case '<':
                reference = SXML_BYTE_VIEW_FROM_LITERAL("&lt;");
                break;
            case '"':
                reference = SXML_BYTE_VIEW_FROM_LITERAL("&quot;");
                break;
            case '\t':
                reference = SXML_BYTE_VIEW_FROM_LITERAL("&#9;");
                break;
            case '\n':
                reference = SXML_BYTE_VIEW_FROM_LITERAL("&#10;");
                break;
            case '\r':
                reference = SXML_BYTE_VIEW_FROM_LITERAL("&#13;");
                break;
            default:
                continue;
            }
        }
        sxml_dtd_write_view(writer, (sxml_byte_view){.bytes = text.bytes + start,
                                                     .byte_count = byte_index - start});
        sxml_dtd_write_view(writer, reference);
        start = byte_index + 1;
    }
}

/* Writes a decimal number. */
static void sxml_dtd_write_number(sxml_dtd_writer *writer, size_t number) {
    char digits[24];
    size_t digit_count = 0;
    do {
        digits[sizeof digits - 1 - digit_count] = (char)('0' + number % 10);
        digit_count += 1;
        number /= 10;
    } while (number != 0);
    sxml_dtd_write_view(
        writer, (sxml_byte_view){.bytes = (const uint8_t *)digits + sizeof digits - digit_count,
                                 .byte_count = digit_count});
}

/* ------------------------------------------------------------ Names */

/* A qualified name of the DTD, split at its colon. */
typedef struct sxml_dtd_name {
    sxml_byte_view namespace_prefix;
    sxml_byte_view local_name;
} sxml_dtd_name;

static sxml_dtd_name sxml_dtd_split(sxml_byte_view qualified_name) {
    const uint8_t *colon = qualified_name.byte_count == 0
                               ? NULL
                               : memchr(qualified_name.bytes, ':', qualified_name.byte_count);
    if (colon == NULL) {
        return (sxml_dtd_name){.namespace_prefix = {.bytes = NULL, .byte_count = 0},
                               .local_name = qualified_name};
    }
    const size_t prefix_byte_count = (size_t)(colon - qualified_name.bytes);
    return (sxml_dtd_name){
        .namespace_prefix = {.bytes = qualified_name.bytes, .byte_count = prefix_byte_count},
        .local_name = {.bytes = colon + 1,
                       .byte_count = qualified_name.byte_count - prefix_byte_count - 1}};
}

/* The work of one translation. */
typedef struct sxml_dtd_translation {
    const sxml_dtd_schema_request *request;
    /* The namespaces, as sxml_byte_view; the first is no namespace. */
    sxml_xml_array namespaces;
    /* Open groups while a content model is written: uint8_t, 1 for a
       choice. */
    sxml_xml_array open_groups;
    sxml_dtd_writer writer;
} sxml_dtd_translation;

/* Where a name resolves to: a namespace number, or none. */
typedef struct sxml_dtd_resolution {
    bool is_resolved;
    size_t namespace_number;
    sxml_byte_view local_name;
} sxml_dtd_resolution;

/* A name to resolve and whether an empty prefix means the default
   namespace (elements) or no namespace (attributes, NS 1.0, 6.2). */
typedef struct sxml_dtd_name_request {
    sxml_byte_view qualified_name;
    bool uses_default_namespace;
} sxml_dtd_name_request;

/* Finds the namespace of a prefix among the bindings and gives it a
   number, adding it to the namespaces when it is new. */
static sxml_status sxml_dtd_resolve(sxml_dtd_translation *translation,
                                    sxml_dtd_name_request name_request,
                                    sxml_dtd_resolution *resolution) {
    const sxml_dtd_name name = sxml_dtd_split(name_request.qualified_name);
    *resolution = (sxml_dtd_resolution){
        .is_resolved = false, .namespace_number = 0, .local_name = name.local_name};
    if (name.namespace_prefix.byte_count == 0 && !name_request.uses_default_namespace) {
        resolution->is_resolved = true;
        return SXML_STATUS_OK;
    }
    const sxml_dtd_binding_view bindings = translation->request->bindings;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    bool is_bound = false;
    for (size_t binding_index = 0; binding_index < bindings.binding_count && !is_bound;
         binding_index++) {
        const sxml_dtd_prefix_binding *binding = &bindings.bindings[binding_index];
        if (sxml_byte_view_equals(binding->namespace_prefix, name.namespace_prefix)) {
            is_bound = true;
            namespace_uri = binding->namespace_uri;
        }
    }
    if (!is_bound) {
        /* An unprefixed element name without a default namespace is in no
           namespace; an unbound prefix resolves to nothing. */
        resolution->is_resolved = name.namespace_prefix.byte_count == 0;
        return SXML_STATUS_OK;
    }
    sxml_xml_array *namespaces = &translation->namespaces;
    for (size_t namespace_index = 0; namespace_index < namespaces->element_count;
         namespace_index++) {
        if (sxml_byte_view_equals(
                *(const sxml_byte_view *)sxml_xml_array_at(namespaces, namespace_index),
                namespace_uri)) {
            resolution->is_resolved = true;
            resolution->namespace_number = namespace_index;
            return SXML_STATUS_OK;
        }
    }
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(namespaces, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(sxml_byte_view *)element = namespace_uri;
    resolution->is_resolved = true;
    resolution->namespace_number = namespaces->element_count - 1;
    return SXML_STATUS_OK;
}

/* Writes a resolved name as a QName of the schema documents: the
   namespace numbered n has the prefix "n" followed by n; no namespace has
   none. */
static bool sxml_dtd_is_xml_namespace(const sxml_dtd_translation *translation,
                                      size_t namespace_number) {
    return namespace_number != 0 &&
           sxml_byte_view_equals(*(const sxml_byte_view *)sxml_xml_array_at(
                                     &translation->namespaces, namespace_number),
                                 SXML_BYTE_VIEW_FROM_LITERAL(SXML_DTD_XML_NAMESPACE));
}

static void sxml_dtd_write_qualified_name(sxml_dtd_translation *translation,
                                          const sxml_dtd_resolution *name) {
    if (sxml_dtd_is_xml_namespace(translation, name->namespace_number)) {
        /* The xml prefix is bound to its namespace in every document, and
           no other prefix may be (Namespaces in XML 1.0, section 3). */
        SXML_DTD_WRITE(&translation->writer, "xml:");
    } else if (name->namespace_number != 0) {
        SXML_DTD_WRITE(&translation->writer, "n");
        sxml_dtd_write_number(&translation->writer, name->namespace_number);
        SXML_DTD_WRITE(&translation->writer, ":");
    }
    sxml_dtd_write_view(&translation->writer, name->local_name);
}

static const sxml_byte_view sxml_dtd_empty_view = {.bytes = NULL, .byte_count = 0};

/* ---------------------------------------------------- Content models */

/* The minOccurs and maxOccurs of a quantifier ([47], [48]). */
static void sxml_dtd_write_occurrences(sxml_dtd_writer *writer, uint8_t quantifier) {
    if (quantifier == '?' || quantifier == '*') {
        SXML_DTD_WRITE(writer, " minOccurs=\"0\"");
    }
    if (quantifier == '*' || quantifier == '+') {
        SXML_DTD_WRITE(writer, " maxOccurs=\"unbounded\"");
    }
}

/* A reference to an element type; an unresolvable name refers to the
   declaration of no namespace that nothing matches. */
static sxml_status sxml_dtd_write_element_reference(sxml_dtd_translation *translation,
                                                    const sxml_xml_particle *particle) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    sxml_dtd_resolution name = {
        .is_resolved = false, .namespace_number = 0, .local_name = sxml_dtd_empty_view};
    const sxml_status status = sxml_dtd_resolve(
        translation,
        (sxml_dtd_name_request){.qualified_name = sxml_xml_dtd_text(dtd, particle->element_name),
                                .uses_default_namespace = true},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_dtd_writer *writer = &translation->writer;
    SXML_DTD_WRITE(writer, "<xs:element ref=\"");
    if (name.is_resolved) {
        sxml_dtd_write_qualified_name(translation, &name);
    } else {
        SXML_DTD_WRITE(writer, "sxml-unresolved-element");
    }
    SXML_DTD_WRITE(writer, "\"");
    sxml_dtd_write_occurrences(writer, particle->quantifier);
    SXML_DTD_WRITE(writer, "/>");
    return writer->status;
}

/* The content model of an element type ([46] contentspec), inside its
   xs:complexType. Groups nest on an explicit stack (D71). */
static sxml_status sxml_dtd_write_content(sxml_dtd_translation *translation,
                                          const sxml_xml_element_entry *element) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    sxml_dtd_writer *writer = &translation->writer;
    sxml_status status = SXML_STATUS_OK;
    if (element->content_kind == SXML_XML_CONTENT_KIND_ANY) {
        SXML_DTD_WRITE(writer, "<xs:sequence><xs:any namespace=\"##any\" "
                               "processContents=\"strict\" minOccurs=\"0\" "
                               "maxOccurs=\"unbounded\"/></xs:sequence>");
        return writer->status;
    }
    if (element->content_kind == SXML_XML_CONTENT_KIND_MIXED) {
        if (element->particle_count == 0) {
            return SXML_STATUS_OK;
        }
        SXML_DTD_WRITE(writer, "<xs:choice minOccurs=\"0\" maxOccurs=\"unbounded\">");
    }
    translation->open_groups.element_count = 0;
    for (size_t particle_index = 0;
         status == SXML_STATUS_OK && particle_index < element->particle_count; particle_index++) {
        const sxml_xml_particle *particle =
            sxml_xml_array_at(&dtd->particles, element->first_particle + particle_index);
        switch (particle->particle_kind) {
        case SXML_XML_PARTICLE_KIND_SEQUENCE:
        case SXML_XML_PARTICLE_KIND_CHOICE: {
            const bool is_choice = particle->particle_kind == SXML_XML_PARTICLE_KIND_CHOICE;
            void *open = NULL;
            status = sxml_xml_array_push(&translation->open_groups, &open);
            if (status == SXML_STATUS_OK) {
                *(uint8_t *)open = is_choice ? 1U : 0U;
                sxml_dtd_write_view(writer, is_choice
                                                ? SXML_BYTE_VIEW_FROM_LITERAL("<xs:choice")
                                                : SXML_BYTE_VIEW_FROM_LITERAL("<xs:sequence"));
                sxml_dtd_write_occurrences(writer, particle->quantifier);
                SXML_DTD_WRITE(writer, ">");
                status = writer->status;
            }
            break;
        }
        case SXML_XML_PARTICLE_KIND_GROUP_END: {
            sxml_xml_array *open_groups = &translation->open_groups;
            const bool is_choice = open_groups->element_count != 0 &&
                                   *(const uint8_t *)sxml_xml_array_at(
                                       open_groups, open_groups->element_count - 1) != 0;
            if (open_groups->element_count != 0) {
                open_groups->element_count -= 1;
            }
            sxml_dtd_write_view(writer, is_choice ? SXML_BYTE_VIEW_FROM_LITERAL("</xs:choice>")
                                                  : SXML_BYTE_VIEW_FROM_LITERAL("</xs:sequence>"));
            status = writer->status;
            break;
        }
        case SXML_XML_PARTICLE_KIND_ELEMENT:
            status = sxml_dtd_write_element_reference(translation, particle);
            break;
        }
    }
    if (element->content_kind == SXML_XML_CONTENT_KIND_MIXED) {
        SXML_DTD_WRITE(writer, "</xs:choice>");
    }
    return status == SXML_STATUS_OK ? writer->status : status;
}

/* ------------------------------------------------------- Attributes */

/* The simple type of an attribute definition ([54] AttType): a type
   attribute, or for enumerated types an inline restriction, written after
   the value constraint by sxml_dtd_write_attribute. */
static sxml_byte_view sxml_dtd_type_name(sxml_xml_attribute_type attribute_type) {
    switch (attribute_type) {
    case SXML_XML_ATTRIBUTE_TYPE_CDATA:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:string");
    case SXML_XML_ATTRIBUTE_TYPE_ID:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:ID");
    case SXML_XML_ATTRIBUTE_TYPE_IDREF:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:IDREF");
    case SXML_XML_ATTRIBUTE_TYPE_IDREFS:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:IDREFS");
    case SXML_XML_ATTRIBUTE_TYPE_ENTITY:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:ENTITY");
    case SXML_XML_ATTRIBUTE_TYPE_ENTITIES:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:ENTITIES");
    case SXML_XML_ATTRIBUTE_TYPE_NMTOKEN:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:NMTOKEN");
    case SXML_XML_ATTRIBUTE_TYPE_NMTOKENS:
        return SXML_BYTE_VIEW_FROM_LITERAL("xs:NMTOKENS");
    case SXML_XML_ATTRIBUTE_TYPE_NOTATION:
    case SXML_XML_ATTRIBUTE_TYPE_ENUMERATION:
        break;
    }
    return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

/* One attribute declaration or use: its name or reference, then its type
   and value constraint. A use refers to a global declaration (prefixed
   names) or declares a local one. */
typedef struct sxml_dtd_attribute {
    const sxml_xml_attribute_definition *definition;
    const sxml_dtd_resolution *name;
    /* A reference to the global declaration of name. */
    bool is_reference;
    /* The global declaration itself: a type, no use or value constraint. */
    bool is_global_declaration;
} sxml_dtd_attribute;

static void sxml_dtd_write_attribute(sxml_dtd_translation *translation,
                                     const sxml_dtd_attribute *attribute) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    const sxml_xml_attribute_definition *definition = attribute->definition;
    sxml_dtd_writer *writer = &translation->writer;
    sxml_dtd_write_view(writer, attribute->is_reference
                                    ? SXML_BYTE_VIEW_FROM_LITERAL("<xs:attribute ref=\"")
                                    : SXML_BYTE_VIEW_FROM_LITERAL("<xs:attribute name=\""));
    if (attribute->is_reference) {
        sxml_dtd_write_qualified_name(translation, attribute->name);
    } else {
        sxml_dtd_write_view(writer, attribute->name->local_name);
    }
    SXML_DTD_WRITE(writer, "\"");
    const sxml_byte_view type_name = sxml_dtd_type_name(definition->attribute_type);
    if (!attribute->is_reference && type_name.byte_count != 0) {
        SXML_DTD_WRITE(writer, " type=\"");
        sxml_dtd_write_view(writer, type_name);
        SXML_DTD_WRITE(writer, "\"");
    }
    if (!attribute->is_global_declaration &&
        definition->attribute_type != SXML_XML_ATTRIBUTE_TYPE_ID) {
        /* An ID has no default (VC: ID Attribute Default, which the
           parser checks), so none is written for XSD. */
        if (definition->default_kind == SXML_XML_DEFAULT_KIND_FIXED) {
            SXML_DTD_WRITE(writer, " fixed=\"");
            sxml_dtd_write_escaped(writer, sxml_xml_dtd_text(dtd, definition->default_value));
            SXML_DTD_WRITE(writer, "\"");
        } else if (definition->default_kind == SXML_XML_DEFAULT_KIND_VALUE) {
            SXML_DTD_WRITE(writer, " default=\"");
            sxml_dtd_write_escaped(writer, sxml_xml_dtd_text(dtd, definition->default_value));
            SXML_DTD_WRITE(writer, "\"");
        }
    }
    if (!attribute->is_global_declaration &&
        definition->default_kind == SXML_XML_DEFAULT_KIND_REQUIRED) {
        SXML_DTD_WRITE(writer, " use=\"required\"");
    }
    if (attribute->is_reference || type_name.byte_count != 0) {
        SXML_DTD_WRITE(writer, "/>");
        return;
    }
    SXML_DTD_WRITE(writer, "><xs:simpleType><xs:restriction base=\"");
    sxml_dtd_write_view(writer, definition->attribute_type == SXML_XML_ATTRIBUTE_TYPE_NOTATION
                                    ? SXML_BYTE_VIEW_FROM_LITERAL("xs:NOTATION")
                                    : SXML_BYTE_VIEW_FROM_LITERAL("xs:NMTOKEN"));
    SXML_DTD_WRITE(writer, "\">");
    sxml_byte_view rest = sxml_xml_dtd_text(dtd, definition->enumeration);
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    while (sxml_xml_text_next_token(&rest, &token)) {
        SXML_DTD_WRITE(writer, "<xs:enumeration value=\"");
        sxml_dtd_write_escaped(writer, token);
        SXML_DTD_WRITE(writer, "\"/>");
    }
    SXML_DTD_WRITE(writer, "</xs:restriction></xs:simpleType></xs:attribute>");
}

static bool sxml_dtd_is_namespace_declaration(sxml_byte_view name) {
    return sxml_byte_view_equals(name, SXML_BYTE_VIEW_FROM_LITERAL("xmlns")) ||
           (name.byte_count > 6 && memcmp(name.bytes, "xmlns:", 6) == 0);
}

static bool sxml_dtd_is_xml_attribute(sxml_byte_view name) {
    return name.byte_count > 4 && memcmp(name.bytes, "xml:", 4) == 0;
}

/* What one pass over the attribute definitions writes. */
typedef struct sxml_dtd_attribute_pass {
    /* The element type's attribute uses, or the global declarations of
       namespace_number (prefixed names, first definition of each name
       across the element types). */
    const sxml_xml_element_entry *element;
    size_t namespace_number;
} sxml_dtd_attribute_pass;

/* True when an earlier definition, of any element type, has the same
   prefixed name: only the first is declared globally. */
static bool sxml_dtd_is_first_of_name(const sxml_xml_dtd *dtd, uint32_t definition_number) {
    const sxml_byte_view name =
        sxml_xml_dtd_text(dtd, sxml_xml_dtd_get_definition(dtd, definition_number)->attribute_name);
    for (uint32_t earlier = 1; earlier < definition_number; earlier++) {
        if (sxml_byte_view_equals(
                sxml_xml_dtd_text(dtd, sxml_xml_dtd_get_definition(dtd, earlier)->attribute_name),
                name)) {
            return false;
        }
    }
    return true;
}

/* The attribute uses of an element type, or the global attribute
   declarations of a namespace when pass->element is NULL. */
static sxml_status sxml_dtd_write_attributes(sxml_dtd_translation *translation,
                                             const sxml_dtd_attribute_pass *pass) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    const size_t definition_count = dtd->attribute_definitions.element_count;
    bool has_xml_attribute = false;
    uint32_t number = pass->element != NULL   ? pass->element->first_definition
                      : definition_count != 0 ? 1U
                                              : 0U;
    while (number != 0) {
        const sxml_xml_attribute_definition *definition = sxml_xml_dtd_get_definition(dtd, number);
        const uint32_t current = number;
        number = pass->element != NULL       ? definition->next_definition
                 : number < definition_count ? number + 1
                                             : 0U;
        const sxml_byte_view qualified_name = sxml_xml_dtd_text(dtd, definition->attribute_name);
        if (sxml_dtd_is_namespace_declaration(qualified_name)) {
            continue;
        }
        if (sxml_dtd_is_xml_attribute(qualified_name)) {
            has_xml_attribute = true;
            continue;
        }
        sxml_dtd_resolution name = {
            .is_resolved = false, .namespace_number = 0, .local_name = sxml_dtd_empty_view};
        const sxml_status status =
            sxml_dtd_resolve(translation,
                             (sxml_dtd_name_request){.qualified_name = qualified_name,
                                                     .uses_default_namespace = false},
                             &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const bool is_prefixed = name.namespace_number != 0;
        if (!name.is_resolved) {
            continue;
        }
        if (pass->element == NULL) {
            if (is_prefixed && name.namespace_number == pass->namespace_number &&
                sxml_dtd_is_first_of_name(dtd, current)) {
                sxml_dtd_write_attribute(translation,
                                         &(sxml_dtd_attribute){.definition = definition,
                                                               .name = &name,
                                                               .is_reference = false,
                                                               .is_global_declaration = true});
            }
            continue;
        }
        sxml_dtd_write_attribute(translation,
                                 &(sxml_dtd_attribute){.definition = definition,
                                                       .name = &name,
                                                       .is_reference = is_prefixed,
                                                       .is_global_declaration = false});
    }
    if (pass->element != NULL && has_xml_attribute) {
        SXML_DTD_WRITE(&translation->writer, "<xs:anyAttribute namespace=\"" SXML_DTD_XML_NAMESPACE
                                             "\" processContents=\"skip\"/>");
    }
    return translation->writer.status;
}

/* --------------------------------------------------------- Documents */

/* An element declaration of an element type ([45] elementdecl), or of a
   name a content model refers to without a declaration. */
static sxml_status sxml_dtd_write_element(sxml_dtd_translation *translation,
                                          const sxml_xml_element_entry *element,
                                          const sxml_dtd_resolution *name) {
    sxml_dtd_writer *writer = &translation->writer;
    SXML_DTD_WRITE(writer, "<xs:element name=\"");
    sxml_dtd_write_view(writer, name->local_name);
    if (!element->is_declared) {
        SXML_DTD_WRITE(writer, "\" type=\"sxml-undeclared-element\"/>");
        return writer->status;
    }
    SXML_DTD_WRITE(writer, "\"><xs:complexType");
    if (element->content_kind == SXML_XML_CONTENT_KIND_ANY ||
        element->content_kind == SXML_XML_CONTENT_KIND_MIXED) {
        SXML_DTD_WRITE(writer, " mixed=\"true\"");
    }
    SXML_DTD_WRITE(writer, ">");
    sxml_status status = sxml_dtd_write_content(translation, element);
    if (status == SXML_STATUS_OK) {
        status = sxml_dtd_write_attributes(
            translation, &(sxml_dtd_attribute_pass){.element = element, .namespace_number = 0});
    }
    SXML_DTD_WRITE(writer, "</xs:complexType></xs:element>");
    return status == SXML_STATUS_OK ? writer->status : status;
}

/* The notation declarations ([82] NotationDecl), in the document of no
   namespace. */
static void sxml_dtd_write_notations(sxml_dtd_translation *translation) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    sxml_dtd_writer *writer = &translation->writer;
    for (size_t notation_index = 0; notation_index < dtd->notations.element_count;
         notation_index++) {
        const sxml_xml_notation *notation = sxml_xml_array_at(&dtd->notations, notation_index);
        SXML_DTD_WRITE(writer, "<xs:notation name=\"");
        sxml_dtd_write_escaped(writer, sxml_xml_dtd_text(dtd, notation->notation_name));
        SXML_DTD_WRITE(writer, "\"");
        if (notation->public_identifier.byte_count != 0) {
            SXML_DTD_WRITE(writer, " public=\"");
            sxml_dtd_write_escaped(writer, sxml_xml_dtd_text(dtd, notation->public_identifier));
            SXML_DTD_WRITE(writer, "\"");
        }
        if (notation->system_identifier.byte_count != 0 ||
            notation->public_identifier.byte_count == 0) {
            SXML_DTD_WRITE(writer, " system=\"");
            sxml_dtd_write_escaped(writer, sxml_xml_dtd_text(dtd, notation->system_identifier));
            SXML_DTD_WRITE(writer, "\"");
        }
        SXML_DTD_WRITE(writer, "/>");
    }
    SXML_DTD_WRITE(writer, "<xs:simpleType name=\"sxml-undeclared-element\">"
                           "<xs:restriction base=\"xs:string\"><xs:maxLength value=\"0\"/>"
                           "<xs:pattern value=\"x\"/></xs:restriction></xs:simpleType>"
                           "<xs:element name=\"sxml-unresolved-element\" "
                           "type=\"sxml-undeclared-element\"/>");
}

/* The schema document of the namespace numbered namespace_number. */
static sxml_status sxml_dtd_write_document(sxml_dtd_translation *translation,
                                           size_t namespace_number) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    sxml_dtd_writer *writer = &translation->writer;
    const sxml_xml_array *namespaces = &translation->namespaces;
    SXML_DTD_WRITE(writer, "<xs:schema xmlns:xs=\"" SXML_DTD_XSD_NAMESPACE "\"");
    for (size_t other = 1; other < namespaces->element_count; other++) {
        if (sxml_dtd_is_xml_namespace(translation, other)) {
            continue;
        }
        SXML_DTD_WRITE(writer, " xmlns:n");
        sxml_dtd_write_number(writer, other);
        SXML_DTD_WRITE(writer, "=\"");
        sxml_dtd_write_escaped(writer,
                               *(const sxml_byte_view *)sxml_xml_array_at(namespaces, other));
        SXML_DTD_WRITE(writer, "\"");
    }
    if (namespace_number != 0) {
        SXML_DTD_WRITE(writer, " targetNamespace=\"");
        sxml_dtd_write_escaped(
            writer, *(const sxml_byte_view *)sxml_xml_array_at(namespaces, namespace_number));
        SXML_DTD_WRITE(writer, "\"");
    }
    SXML_DTD_WRITE(writer, " elementFormDefault=\"qualified\">");
    for (size_t other = 0; other < namespaces->element_count; other++) {
        if (other == namespace_number) {
            continue;
        }
        SXML_DTD_WRITE(writer, "<xs:import");
        if (other != 0) {
            SXML_DTD_WRITE(writer, " namespace=\"");
            sxml_dtd_write_escaped(writer,
                                   *(const sxml_byte_view *)sxml_xml_array_at(namespaces, other));
            SXML_DTD_WRITE(writer, "\"");
        }
        SXML_DTD_WRITE(writer, "/>");
    }
    if (namespace_number == 0) {
        sxml_dtd_write_notations(translation);
    }
    sxml_status status = writer->status;
    for (size_t element_index = 0;
         status == SXML_STATUS_OK && element_index < dtd->elements.element_count; element_index++) {
        const sxml_xml_element_entry *element =
            sxml_xml_dtd_element_at(dtd, (uint32_t)element_index);
        const sxml_byte_view element_name = sxml_xml_dtd_text(dtd, element->element_name);
        if (!element->is_declared && !element->is_referred_to) {
            continue;
        }
        sxml_dtd_resolution name = {
            .is_resolved = false, .namespace_number = 0, .local_name = sxml_dtd_empty_view};
        status = sxml_dtd_resolve(
            translation,
            (sxml_dtd_name_request){.qualified_name = element_name, .uses_default_namespace = true},
            &name);
        if (status == SXML_STATUS_OK && name.is_resolved &&
            name.namespace_number == namespace_number) {
            status = sxml_dtd_write_element(translation, element, &name);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_dtd_write_attributes(
            translation,
            &(sxml_dtd_attribute_pass){.element = NULL, .namespace_number = namespace_number});
    }
    SXML_DTD_WRITE(writer, "</xs:schema>");
    return status == SXML_STATUS_OK ? writer->status : status;
}

/* Gives every namespace a number before any document is written, since
   each document declares and imports all of them. */
static sxml_status sxml_dtd_number_namespaces(sxml_dtd_translation *translation) {
    const sxml_xml_dtd *dtd = translation->request->dtd;
    sxml_status status = SXML_STATUS_OK;
    sxml_dtd_resolution name = {
        .is_resolved = false, .namespace_number = 0, .local_name = sxml_dtd_empty_view};
    for (size_t element_index = 0;
         status == SXML_STATUS_OK && element_index < dtd->elements.element_count; element_index++) {
        status = sxml_dtd_resolve(
            translation,
            (sxml_dtd_name_request){
                .qualified_name = sxml_xml_dtd_text(
                    dtd, sxml_xml_dtd_element_at(dtd, (uint32_t)element_index)->element_name),
                .uses_default_namespace = true},
            &name);
    }
    for (size_t particle_index = 0;
         status == SXML_STATUS_OK && particle_index < dtd->particles.element_count;
         particle_index++) {
        const sxml_xml_particle *particle = sxml_xml_array_at(&dtd->particles, particle_index);
        if (particle->particle_kind == SXML_XML_PARTICLE_KIND_ELEMENT) {
            status = sxml_dtd_resolve(translation,
                                      (sxml_dtd_name_request){.qualified_name = sxml_xml_dtd_text(
                                                                  dtd, particle->element_name),
                                                              .uses_default_namespace = true},
                                      &name);
        }
    }
    for (uint32_t number = 1;
         status == SXML_STATUS_OK && number <= dtd->attribute_definitions.element_count; number++) {
        const sxml_byte_view qualified_name =
            sxml_xml_dtd_text(dtd, sxml_xml_dtd_get_definition(dtd, number)->attribute_name);
        if (!sxml_dtd_is_namespace_declaration(qualified_name) &&
            !sxml_dtd_is_xml_attribute(qualified_name)) {
            status = sxml_dtd_resolve(translation,
                                      (sxml_dtd_name_request){.qualified_name = qualified_name,
                                                              .uses_default_namespace = false},
                                      &name);
        }
    }
    return status;
}

sxml_status sxml_dtd_write_schemas(const sxml_dtd_schema_request *request) {
    const sxml_allocator allocator = request->output->allocator;
    sxml_dtd_translation translation = {
        .request = request,
        .namespaces = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(sxml_byte_view)}),
        .open_groups = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(uint8_t)}),
        .writer = {.output = request->output, .status = SXML_STATUS_OK},
    };
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&translation.namespaces, &element);
    if (status == SXML_STATUS_OK) {
        *(sxml_byte_view *)element = sxml_dtd_empty_view;
        status = sxml_dtd_number_namespaces(&translation);
    }
    for (size_t namespace_number = 0;
         status == SXML_STATUS_OK && namespace_number < translation.namespaces.element_count;
         namespace_number++) {
        const size_t start = request->output->byte_count;
        status = sxml_dtd_write_document(&translation, namespace_number);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_array_push(request->documents, &element);
        }
        if (status == SXML_STATUS_OK) {
            *(sxml_byte_range *)element = (sxml_byte_range){
                .byte_offset = start, .byte_count = request->output->byte_count - start};
        }
    }
    sxml_xml_array_release(&translation.namespaces);
    sxml_xml_array_release(&translation.open_groups);
    return status;
}
