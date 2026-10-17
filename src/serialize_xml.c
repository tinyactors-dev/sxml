/*
 * The XML and XHTML output methods (Serialization 4.0, sections 5 and 6;
 * docs/DECISIONS.md D106, D120), and the node writer of the adaptive
 * method (section 11, D82).
 *
 * The result tree of sequence normalization (src/serialize.c) arrives as
 * text and nodes; each node's subtree is written by walking its node
 * indices in document order with a stack of open elements in heap memory
 * (D71: no recursion). Namespace declarations are computed against the
 * bindings already written for the open elements (a stack of bindings),
 * which gives namespace fixup, undeclarations, prefix normalization for
 * XHTML with HTML5 and the namespace axis of Canonical XML from one rule.
 *
 * Golden file: src/serialize.c (the adaptive method this file grew out
 * of).
 */
#include "serialize_internal.h"

#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/tree.h"
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Tables */

/* An attribute whose value is a URI for one HTML element (Serialization
   4.0, appendix D "List of URI Attributes"). */
typedef struct sxml_uri_attribute {
    sxml_byte_view attribute_name;
    sxml_byte_view element_name;
} sxml_uri_attribute;

static const sxml_uri_attribute sxml_uri_attributes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("action"), SXML_BYTE_VIEW_INITIALIZER("form")},      {SXML_BYTE_VIEW_INITIALIZER("archive"), SXML_BYTE_VIEW_INITIALIZER("object")},    {SXML_BYTE_VIEW_INITIALIZER("background"), SXML_BYTE_VIEW_INITIALIZER("body")},
    {SXML_BYTE_VIEW_INITIALIZER("cite"), SXML_BYTE_VIEW_INITIALIZER("blockquote")},  {SXML_BYTE_VIEW_INITIALIZER("cite"), SXML_BYTE_VIEW_INITIALIZER("del")},          {SXML_BYTE_VIEW_INITIALIZER("cite"), SXML_BYTE_VIEW_INITIALIZER("ins")},
    {SXML_BYTE_VIEW_INITIALIZER("cite"), SXML_BYTE_VIEW_INITIALIZER("q")},           {SXML_BYTE_VIEW_INITIALIZER("classid"), SXML_BYTE_VIEW_INITIALIZER("object")},    {SXML_BYTE_VIEW_INITIALIZER("codebase"), SXML_BYTE_VIEW_INITIALIZER("applet")},
    {SXML_BYTE_VIEW_INITIALIZER("codebase"), SXML_BYTE_VIEW_INITIALIZER("object")},  {SXML_BYTE_VIEW_INITIALIZER("data"), SXML_BYTE_VIEW_INITIALIZER("object")},       {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("button")},
    {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("div")},      {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("input")},     {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("object")},
    {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("select")},   {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("span")},      {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("table")},
    {SXML_BYTE_VIEW_INITIALIZER("datasrc"), SXML_BYTE_VIEW_INITIALIZER("textarea")}, {SXML_BYTE_VIEW_INITIALIZER("for"), SXML_BYTE_VIEW_INITIALIZER("script")},        {SXML_BYTE_VIEW_INITIALIZER("formaction"), SXML_BYTE_VIEW_INITIALIZER("button")},
    {SXML_BYTE_VIEW_INITIALIZER("formaction"), SXML_BYTE_VIEW_INITIALIZER("input")}, {SXML_BYTE_VIEW_INITIALIZER("href"), SXML_BYTE_VIEW_INITIALIZER("a")},            {SXML_BYTE_VIEW_INITIALIZER("href"), SXML_BYTE_VIEW_INITIALIZER("area")},
    {SXML_BYTE_VIEW_INITIALIZER("href"), SXML_BYTE_VIEW_INITIALIZER("base")},        {SXML_BYTE_VIEW_INITIALIZER("href"), SXML_BYTE_VIEW_INITIALIZER("link")},         {SXML_BYTE_VIEW_INITIALIZER("icon"), SXML_BYTE_VIEW_INITIALIZER("command")},
    {SXML_BYTE_VIEW_INITIALIZER("longdesc"), SXML_BYTE_VIEW_INITIALIZER("frame")},   {SXML_BYTE_VIEW_INITIALIZER("longdesc"), SXML_BYTE_VIEW_INITIALIZER("iframe")},   {SXML_BYTE_VIEW_INITIALIZER("longdesc"), SXML_BYTE_VIEW_INITIALIZER("img")},
    {SXML_BYTE_VIEW_INITIALIZER("manifest"), SXML_BYTE_VIEW_INITIALIZER("html")},    {SXML_BYTE_VIEW_INITIALIZER("name"), SXML_BYTE_VIEW_INITIALIZER("a")},            {SXML_BYTE_VIEW_INITIALIZER("poster"), SXML_BYTE_VIEW_INITIALIZER("video")},
    {SXML_BYTE_VIEW_INITIALIZER("profile"), SXML_BYTE_VIEW_INITIALIZER("head")},     {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("audio")},         {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("embed")},
    {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("frame")},        {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("iframe")},        {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("img")},
    {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("input")},        {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("script")},        {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("source")},
    {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("track")},        {SXML_BYTE_VIEW_INITIALIZER("src"), SXML_BYTE_VIEW_INITIALIZER("video")},         {SXML_BYTE_VIEW_INITIALIZER("usemap"), SXML_BYTE_VIEW_INITIALIZER("img")},
    {SXML_BYTE_VIEW_INITIALIZER("usemap"), SXML_BYTE_VIEW_INITIALIZER("input")},     {SXML_BYTE_VIEW_INITIALIZER("usemap"), SXML_BYTE_VIEW_INITIALIZER("object")},     {SXML_BYTE_VIEW_INITIALIZER("value"), SXML_BYTE_VIEW_INITIALIZER("input")},
};

/* The elements of HTML5 (HTML Living Standard, section 4), recognized in
   no namespace by the XHTML method with HTML5 (Serialization 4.0,
   section 6). */
static const sxml_byte_view sxml_html5_element_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("a"),        SXML_BYTE_VIEW_INITIALIZER("abbr"),     SXML_BYTE_VIEW_INITIALIZER("address"),  SXML_BYTE_VIEW_INITIALIZER("area"),     SXML_BYTE_VIEW_INITIALIZER("article"),    SXML_BYTE_VIEW_INITIALIZER("aside"),    SXML_BYTE_VIEW_INITIALIZER("audio"),
    SXML_BYTE_VIEW_INITIALIZER("b"),        SXML_BYTE_VIEW_INITIALIZER("base"),     SXML_BYTE_VIEW_INITIALIZER("bdi"),      SXML_BYTE_VIEW_INITIALIZER("bdo"),      SXML_BYTE_VIEW_INITIALIZER("blockquote"), SXML_BYTE_VIEW_INITIALIZER("body"),     SXML_BYTE_VIEW_INITIALIZER("br"),
    SXML_BYTE_VIEW_INITIALIZER("button"),   SXML_BYTE_VIEW_INITIALIZER("canvas"),   SXML_BYTE_VIEW_INITIALIZER("caption"),  SXML_BYTE_VIEW_INITIALIZER("cite"),     SXML_BYTE_VIEW_INITIALIZER("code"),       SXML_BYTE_VIEW_INITIALIZER("col"),      SXML_BYTE_VIEW_INITIALIZER("colgroup"),
    SXML_BYTE_VIEW_INITIALIZER("data"),     SXML_BYTE_VIEW_INITIALIZER("datalist"), SXML_BYTE_VIEW_INITIALIZER("dd"),       SXML_BYTE_VIEW_INITIALIZER("del"),      SXML_BYTE_VIEW_INITIALIZER("details"),    SXML_BYTE_VIEW_INITIALIZER("dfn"),      SXML_BYTE_VIEW_INITIALIZER("dialog"),
    SXML_BYTE_VIEW_INITIALIZER("div"),      SXML_BYTE_VIEW_INITIALIZER("dl"),       SXML_BYTE_VIEW_INITIALIZER("dt"),       SXML_BYTE_VIEW_INITIALIZER("em"),       SXML_BYTE_VIEW_INITIALIZER("embed"),      SXML_BYTE_VIEW_INITIALIZER("fieldset"), SXML_BYTE_VIEW_INITIALIZER("figcaption"),
    SXML_BYTE_VIEW_INITIALIZER("figure"),   SXML_BYTE_VIEW_INITIALIZER("footer"),   SXML_BYTE_VIEW_INITIALIZER("form"),     SXML_BYTE_VIEW_INITIALIZER("h1"),       SXML_BYTE_VIEW_INITIALIZER("h2"),         SXML_BYTE_VIEW_INITIALIZER("h3"),       SXML_BYTE_VIEW_INITIALIZER("h4"),
    SXML_BYTE_VIEW_INITIALIZER("h5"),       SXML_BYTE_VIEW_INITIALIZER("h6"),       SXML_BYTE_VIEW_INITIALIZER("head"),     SXML_BYTE_VIEW_INITIALIZER("header"),   SXML_BYTE_VIEW_INITIALIZER("hgroup"),     SXML_BYTE_VIEW_INITIALIZER("hr"),       SXML_BYTE_VIEW_INITIALIZER("html"),
    SXML_BYTE_VIEW_INITIALIZER("i"),        SXML_BYTE_VIEW_INITIALIZER("iframe"),   SXML_BYTE_VIEW_INITIALIZER("img"),      SXML_BYTE_VIEW_INITIALIZER("input"),    SXML_BYTE_VIEW_INITIALIZER("ins"),        SXML_BYTE_VIEW_INITIALIZER("kbd"),      SXML_BYTE_VIEW_INITIALIZER("keygen"),
    SXML_BYTE_VIEW_INITIALIZER("label"),    SXML_BYTE_VIEW_INITIALIZER("legend"),   SXML_BYTE_VIEW_INITIALIZER("li"),       SXML_BYTE_VIEW_INITIALIZER("link"),     SXML_BYTE_VIEW_INITIALIZER("main"),       SXML_BYTE_VIEW_INITIALIZER("map"),      SXML_BYTE_VIEW_INITIALIZER("mark"),
    SXML_BYTE_VIEW_INITIALIZER("menu"),     SXML_BYTE_VIEW_INITIALIZER("meta"),     SXML_BYTE_VIEW_INITIALIZER("meter"),    SXML_BYTE_VIEW_INITIALIZER("nav"),      SXML_BYTE_VIEW_INITIALIZER("noscript"),   SXML_BYTE_VIEW_INITIALIZER("object"),   SXML_BYTE_VIEW_INITIALIZER("ol"),
    SXML_BYTE_VIEW_INITIALIZER("optgroup"), SXML_BYTE_VIEW_INITIALIZER("option"),   SXML_BYTE_VIEW_INITIALIZER("output"),   SXML_BYTE_VIEW_INITIALIZER("p"),        SXML_BYTE_VIEW_INITIALIZER("param"),      SXML_BYTE_VIEW_INITIALIZER("picture"),  SXML_BYTE_VIEW_INITIALIZER("pre"),
    SXML_BYTE_VIEW_INITIALIZER("progress"), SXML_BYTE_VIEW_INITIALIZER("q"),        SXML_BYTE_VIEW_INITIALIZER("rp"),       SXML_BYTE_VIEW_INITIALIZER("rt"),       SXML_BYTE_VIEW_INITIALIZER("ruby"),       SXML_BYTE_VIEW_INITIALIZER("s"),        SXML_BYTE_VIEW_INITIALIZER("samp"),
    SXML_BYTE_VIEW_INITIALIZER("script"),   SXML_BYTE_VIEW_INITIALIZER("search"),   SXML_BYTE_VIEW_INITIALIZER("section"),  SXML_BYTE_VIEW_INITIALIZER("select"),   SXML_BYTE_VIEW_INITIALIZER("slot"),       SXML_BYTE_VIEW_INITIALIZER("small"),    SXML_BYTE_VIEW_INITIALIZER("source"),
    SXML_BYTE_VIEW_INITIALIZER("span"),     SXML_BYTE_VIEW_INITIALIZER("strong"),   SXML_BYTE_VIEW_INITIALIZER("style"),    SXML_BYTE_VIEW_INITIALIZER("sub"),      SXML_BYTE_VIEW_INITIALIZER("summary"),    SXML_BYTE_VIEW_INITIALIZER("sup"),      SXML_BYTE_VIEW_INITIALIZER("table"),
    SXML_BYTE_VIEW_INITIALIZER("tbody"),    SXML_BYTE_VIEW_INITIALIZER("td"),       SXML_BYTE_VIEW_INITIALIZER("template"), SXML_BYTE_VIEW_INITIALIZER("textarea"), SXML_BYTE_VIEW_INITIALIZER("tfoot"),      SXML_BYTE_VIEW_INITIALIZER("th"),       SXML_BYTE_VIEW_INITIALIZER("thead"),
    SXML_BYTE_VIEW_INITIALIZER("time"),     SXML_BYTE_VIEW_INITIALIZER("title"),    SXML_BYTE_VIEW_INITIALIZER("tr"),       SXML_BYTE_VIEW_INITIALIZER("track"),    SXML_BYTE_VIEW_INITIALIZER("u"),          SXML_BYTE_VIEW_INITIALIZER("ul"),       SXML_BYTE_VIEW_INITIALIZER("var"),
    SXML_BYTE_VIEW_INITIALIZER("video"),    SXML_BYTE_VIEW_INITIALIZER("wbr"),
};

/* Inline elements: %inline of XHTML 1.0 and phrasing content of HTML5
   (Serialization 4.0, 6.1.5); ins and del only without element
   children. */
static const sxml_byte_view sxml_inline_elements[] = {
    SXML_BYTE_VIEW_INITIALIZER("a"),      SXML_BYTE_VIEW_INITIALIZER("abbr"),   SXML_BYTE_VIEW_INITIALIZER("acronym"), SXML_BYTE_VIEW_INITIALIZER("applet"), SXML_BYTE_VIEW_INITIALIZER("audio"),  SXML_BYTE_VIEW_INITIALIZER("b"),        SXML_BYTE_VIEW_INITIALIZER("basefont"), SXML_BYTE_VIEW_INITIALIZER("bdi"),
    SXML_BYTE_VIEW_INITIALIZER("bdo"),    SXML_BYTE_VIEW_INITIALIZER("big"),    SXML_BYTE_VIEW_INITIALIZER("br"),      SXML_BYTE_VIEW_INITIALIZER("button"), SXML_BYTE_VIEW_INITIALIZER("canvas"), SXML_BYTE_VIEW_INITIALIZER("cite"),     SXML_BYTE_VIEW_INITIALIZER("code"),     SXML_BYTE_VIEW_INITIALIZER("data"),
    SXML_BYTE_VIEW_INITIALIZER("datalist"), SXML_BYTE_VIEW_INITIALIZER("dfn"),  SXML_BYTE_VIEW_INITIALIZER("em"),      SXML_BYTE_VIEW_INITIALIZER("embed"),  SXML_BYTE_VIEW_INITIALIZER("font"),   SXML_BYTE_VIEW_INITIALIZER("i"),        SXML_BYTE_VIEW_INITIALIZER("iframe"),   SXML_BYTE_VIEW_INITIALIZER("img"),
    SXML_BYTE_VIEW_INITIALIZER("input"),  SXML_BYTE_VIEW_INITIALIZER("kbd"),    SXML_BYTE_VIEW_INITIALIZER("label"),   SXML_BYTE_VIEW_INITIALIZER("map"),    SXML_BYTE_VIEW_INITIALIZER("mark"),   SXML_BYTE_VIEW_INITIALIZER("math"),     SXML_BYTE_VIEW_INITIALIZER("meter"),    SXML_BYTE_VIEW_INITIALIZER("noscript"),
    SXML_BYTE_VIEW_INITIALIZER("object"), SXML_BYTE_VIEW_INITIALIZER("output"), SXML_BYTE_VIEW_INITIALIZER("picture"), SXML_BYTE_VIEW_INITIALIZER("progress"), SXML_BYTE_VIEW_INITIALIZER("q"),    SXML_BYTE_VIEW_INITIALIZER("ruby"),     SXML_BYTE_VIEW_INITIALIZER("s"),        SXML_BYTE_VIEW_INITIALIZER("samp"),
    SXML_BYTE_VIEW_INITIALIZER("script"), SXML_BYTE_VIEW_INITIALIZER("select"), SXML_BYTE_VIEW_INITIALIZER("slot"),    SXML_BYTE_VIEW_INITIALIZER("small"),  SXML_BYTE_VIEW_INITIALIZER("span"),   SXML_BYTE_VIEW_INITIALIZER("strike"),   SXML_BYTE_VIEW_INITIALIZER("strong"),   SXML_BYTE_VIEW_INITIALIZER("sub"),
    SXML_BYTE_VIEW_INITIALIZER("sup"),    SXML_BYTE_VIEW_INITIALIZER("svg"),    SXML_BYTE_VIEW_INITIALIZER("template"), SXML_BYTE_VIEW_INITIALIZER("textarea"), SXML_BYTE_VIEW_INITIALIZER("time"), SXML_BYTE_VIEW_INITIALIZER("tt"),      SXML_BYTE_VIEW_INITIALIZER("u"),        SXML_BYTE_VIEW_INITIALIZER("var"),
    SXML_BYTE_VIEW_INITIALIZER("video"),  SXML_BYTE_VIEW_INITIALIZER("wbr"),
};

/* Formatted elements, inside which whitespace is never added (6.1.5). */
static const sxml_byte_view sxml_formatted_elements[] = {
    SXML_BYTE_VIEW_INITIALIZER("pre"), SXML_BYTE_VIEW_INITIALIZER("script"), SXML_BYTE_VIEW_INITIALIZER("style"), SXML_BYTE_VIEW_INITIALIZER("title"), SXML_BYTE_VIEW_INITIALIZER("textarea"),
};

/* True when text equals name without regard to ASCII case. */
/* True when text equals name, which is in lower case, without regard to
   ASCII case. */
static bool sxml_equals_ignoring_case(sxml_byte_view text, sxml_byte_view name) {
    if (text.byte_count != name.byte_count) {
        return false;
    }
    for (size_t offset = 0; offset < name.byte_count; offset += 1) {
        uint8_t byte = text.bytes[offset];
        if (byte >= 'A' && byte <= 'Z') {
            byte = (uint8_t)(byte - 'A' + 'a');
        }
        if (byte != name.bytes[offset]) {
            return false;
        }
    }
    return true;
}

/* A list of names to look a local name up in. */
typedef struct sxml_name_list {
    const sxml_byte_view *names;
    size_t name_count;
} sxml_name_list;

#define SXML_NAME_LIST(array) ((sxml_name_list){.names = (array), .name_count = sizeof(array) / sizeof((array)[0])})

static bool sxml_name_list_contains(sxml_name_list list, sxml_byte_view local_name) {
    for (size_t name_index = 0; name_index < list.name_count; name_index += 1) {
        if (sxml_equals_ignoring_case(local_name, list.names[name_index])) {
            return true;
        }
    }
    return false;
}

static bool sxml_text_is_whitespace(sxml_byte_view text) {
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        const uint8_t byte = text.bytes[offset];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- The writer */

/* An element whose end tag is still to be written. */
typedef struct sxml_xml_open_element {
    sxml_node_index element;
    sxml_node_index last_descendant;
    /* Number of written bindings before this element's. */
    size_t binding_mark;
    /* Levels of indentation of the element's tags. */
    size_t depth;
    /* Whether whitespace is added before its children and end tag. */
    bool is_indenting;
    bool has_written_child;
    /* Its text children are CDATA sections (cdata-section-elements). */
    bool is_cdata_parent;
    /* An HTML head that received a meta element (include-content-type);
       existing content-type meta children are left out. */
    bool is_content_type_head;
    /* Written with its local name only (prefix normalization). */
    bool is_unprefixed;
    /* An HTML script or style element or inside one: its text and the
       attributes of its descendants are raw text (section 7.1). */
    bool is_raw_text;
} sxml_xml_open_element;

/* What is true of the element whose start tag is being written. */
typedef struct sxml_element_facts {
    sxml_node_reference element;
    sxml_node_description description;
    sxml_name_parts name_parts;
    /* Recognized as an HTML element (Serialization 4.0, section 6). */
    bool is_html_element;
    bool is_unprefixed;
    size_t depth;
    bool is_suppressed;
    /* Whether it has element children (ins and del are inline only
       without). */
    bool has_element_child;
    /* Whether a meta element is added as its first child
       (include-content-type). */
    bool has_content_type_meta;
} sxml_element_facts;

typedef struct sxml_xml_writer {
    sxml_serializer *serializer;
    const sxml_serialization_parameters *parameters;
    bool is_xhtml;
    /* The HTML method (section 7): HTML syntax for HTML elements. */
    bool is_html;
    /* XHTML or HTML: the rules both share (recognized HTML elements,
       inline and formatted elements, the meta element, URI attributes). */
    bool has_html_rules;
    /* XML or XHTML: SERE0003 for comments and processing instructions
       XML cannot hold (Serialization 4.0, 5.1). */
    bool checks_xml_constructs;
    bool is_html5;
    bool is_canonical;
    bool is_indenting;
    bool indents_attributes;
    bool undeclares_prefixes;
    bool includes_content_type;
    bool escapes_uri_attributes;
    sxml_byte_view indent_unit;
    /* Identifiers of the namespaces of prefix normalization; 0 when the
       pool has never seen the namespace, so no node is in it. */
    sxml_namespace_uri_identifier xhtml_namespace;
    sxml_namespace_uri_identifier svg_namespace;
    sxml_namespace_uri_identifier mathml_namespace;
    sxml_xml_open_element *open_elements;
    size_t open_element_count;
    size_t open_element_capacity;
    /* The bindings written for the open elements, innermost last;
       namespace URI 0 undeclares. */
    sxml_namespace_binding *bindings;
    size_t binding_count;
    size_t binding_capacity;
    /* Scratch: the bindings an element wants, and those it declares. */
    sxml_namespace_binding *wanted;
    size_t wanted_count;
    size_t wanted_capacity;
    sxml_namespace_binding *declared;
    size_t declared_count;
    size_t declared_capacity;
    /* Scratch: attribute numbers in the order written (canonical). */
    uint32_t *attribute_numbers;
    size_t attribute_number_count;
    size_t attribute_number_capacity;
} sxml_xml_writer;

/* A serialization error: a code of the err namespace and a description,
   both string literals. */
#define SXML_XML_ERROR(code_literal, description_literal)                    \
    ((sxml_serialization_error){                                              \
        .error_local_name = SXML_BYTE_VIEW_INITIALIZER(code_literal),         \
        .description = SXML_BYTE_VIEW_INITIALIZER(description_literal)})

static sxml_status sxml_xml_writer_raise(sxml_xml_writer *writer, sxml_serialization_error error) {
    return sxml_serializer_raise_error(writer->serializer, error);
}

static sxml_status sxml_xml_writer_markup(sxml_xml_writer *writer, sxml_byte_view markup) {
    return sxml_serializer_write_markup(writer->serializer, markup);
}


/* A newline and depth indent units (Serialization 4.0, 5.1.4, 5.1.5). */
static sxml_status sxml_xml_writer_indent(sxml_xml_writer *writer, size_t depth) {
    sxml_status status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    for (size_t level = 0; status == SXML_STATUS_OK && level < depth; level += 1) {
        status = sxml_xml_writer_markup(writer, writer->indent_unit);
    }
    return status;
}

static void sxml_xml_writer_finish(sxml_xml_writer *writer) {
    const sxml_allocator allocator = writer->serializer->allocator;
    sxml_allocator_release(allocator, writer->open_elements);
    sxml_allocator_release(allocator, writer->bindings);
    sxml_allocator_release(allocator, writer->wanted);
    sxml_allocator_release(allocator, writer->declared);
    sxml_allocator_release(allocator, writer->attribute_numbers);
}

/* Appends binding to one of the binding lists. */
typedef struct sxml_binding_list {
    sxml_namespace_binding **bindings;
    size_t *binding_count;
    size_t *binding_capacity;
} sxml_binding_list;

static sxml_status sxml_xml_writer_append_binding(sxml_xml_writer *writer, sxml_binding_list list,
                                                  sxml_namespace_binding binding) {
    const sxml_status status = sxml_serializer_grow(
        writer->serializer->allocator,
        &(sxml_serializer_growth){.elements = (void **)list.bindings,
                                  .element_capacity = list.binding_capacity,
                                  .element_count = *list.binding_count,
                                  .element_byte_count = sizeof(sxml_namespace_binding),
                                  .alignment = alignof(sxml_namespace_binding)});
    if (status == SXML_STATUS_OK) {
        (*list.bindings)[*list.binding_count] = binding;
        *list.binding_count += 1;
    }
    return status;
}

/* The namespace URI the written bindings give prefix (0 for none). */
static sxml_namespace_uri_identifier sxml_xml_writer_find_written(
    const sxml_xml_writer *writer, sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = writer->binding_count; binding_index > 0; binding_index -= 1) {
        const sxml_namespace_binding binding = writer->bindings[binding_index - 1];
        if (binding.namespace_prefix.index == prefix.index) {
            return binding.namespace_uri;
        }
    }
    return (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
}

/* The index of prefix in the wanted bindings, or wanted_count. */
static size_t sxml_xml_writer_find_wanted(const sxml_xml_writer *writer,
                                          sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = 0; binding_index < writer->wanted_count; binding_index += 1) {
        if (writer->wanted[binding_index].namespace_prefix.index == prefix.index) {
            return binding_index;
        }
    }
    return writer->wanted_count;
}

/* Makes the wanted bindings bind prefix to namespace_uri, replacing what
   they bound it to. */
static sxml_status sxml_xml_writer_want(sxml_xml_writer *writer, sxml_namespace_binding binding) {
    const size_t found = sxml_xml_writer_find_wanted(writer, binding.namespace_prefix);
    if (found < writer->wanted_count) {
        writer->wanted[found] = binding;
        return SXML_STATUS_OK;
    }
    return sxml_xml_writer_append_binding(
        writer,
        (sxml_binding_list){.bindings = &writer->wanted,
                            .binding_count = &writer->wanted_count,
                            .binding_capacity = &writer->wanted_capacity},
        binding);
}

static bool sxml_xml_writer_is_normalized_namespace(const sxml_xml_writer *writer,
                                                    sxml_namespace_uri_identifier namespace_uri) {
    return namespace_uri.index != SXML_NAMESPACE_NONE &&
           (namespace_uri.index == writer->xhtml_namespace.index ||
            namespace_uri.index == writer->svg_namespace.index ||
            namespace_uri.index == writer->mathml_namespace.index);
}

/* Whether an attribute of element or of one of its ancestors is written
   with prefix (the exception of prefix normalization). */
static sxml_status sxml_xml_writer_prefix_is_used(sxml_node_reference element,
                                                  sxml_namespace_prefix_identifier prefix,
                                                  sxml_answer *is_used_answer) {
    is_used_answer->is_true = false;
    sxml_node_reference current = element;
    for (;;) {
        sxml_node_description description;
        sxml_status status = sxml_node_describe(current, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            return SXML_STATUS_OK;
        }
        for (uint32_t attribute_number = 1; attribute_number <= description.attribute_count;
             attribute_number += 1) {
            sxml_node_description attribute;
            status = sxml_node_describe(
                (sxml_node_reference){
                    .tree = current.tree,
                    .node_index = {.index = current.node_index.index + attribute_number},
                    .namespace_number = 0},
                &attribute);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (attribute.namespace_prefix.index == prefix.index) {
                is_used_answer->is_true = true;
                return SXML_STATUS_OK;
            }
        }
        if (!description.has_parent) {
            return SXML_STATUS_OK;
        }
        current.node_index = description.parent;
    }
}

/* Fills the wanted bindings of an element (Serialization 4.0, section 5;
   prefix normalization, section 6): its in-scope namespaces, minus xml
   and, with HTML5, the prefixes of the three normalized namespaces; plus
   what its own name and its attributes' names need. */
static sxml_status sxml_xml_writer_collect_wanted(sxml_xml_writer *writer,
                                                  const sxml_element_facts *facts) {
    writer->wanted_count = 0;
    sxml_namespace_binding_view in_scope = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_node_get_in_scope_namespaces(facts->element, &in_scope);
    const bool normalizes = writer->has_html_rules && writer->is_html5 && !writer->is_canonical;
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < in_scope.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = in_scope.namespace_bindings[binding_index];
        if (binding.namespace_uri.index == SXML_NAMESPACE_XML ||
            binding.namespace_uri.index == SXML_NAMESPACE_NONE) {
            continue;
        }
        if (normalizes && binding.namespace_prefix.index != 0 &&
            sxml_xml_writer_is_normalized_namespace(writer, binding.namespace_uri)) {
            sxml_answer is_used = {.is_true = false};
            status = sxml_xml_writer_prefix_is_used(facts->element, binding.namespace_prefix,
                                                    &is_used);
            if (status != SXML_STATUS_OK || !is_used.is_true) {
                continue;
            }
        }
        status = sxml_xml_writer_want(writer, binding);
    }
    /* The element's own name: its prefix bound to its namespace, or no
       default namespace for an unprefixed name in no namespace. */
    const sxml_namespace_prefix_identifier element_prefix = {
        .index = facts->is_unprefixed ? 0 : facts->description.namespace_prefix.index};
    if (status == SXML_STATUS_OK && facts->name_parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        status = sxml_xml_writer_want(
            writer, (sxml_namespace_binding){.namespace_prefix = element_prefix,
                                             .namespace_uri = facts->name_parts.namespace_uri});
    } else if (status == SXML_STATUS_OK && element_prefix.index == 0) {
        const size_t found = sxml_xml_writer_find_wanted(writer, element_prefix);
        if (found < writer->wanted_count) {
            writer->wanted[found] = writer->wanted[writer->wanted_count - 1];
            writer->wanted_count -= 1;
        }
    }
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK && attribute_number <= facts->description.attribute_count;
         attribute_number += 1) {
        sxml_node_description attribute;
        sxml_name_parts parts;
        status = sxml_node_describe(
            (sxml_node_reference){
                .tree = facts->element.tree,
                .node_index = {.index = facts->element.node_index.index + attribute_number},
                .namespace_number = 0},
            &attribute);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                                   attribute.node_name, &parts);
        }
        if (status == SXML_STATUS_OK && parts.namespace_uri.index != SXML_NAMESPACE_NONE &&
            parts.namespace_uri.index != SXML_NAMESPACE_XML &&
            attribute.namespace_prefix.index != 0 &&
            sxml_xml_writer_find_wanted(writer, attribute.namespace_prefix) ==
                writer->wanted_count) {
            status = sxml_xml_writer_want(
                writer, (sxml_namespace_binding){.namespace_prefix = attribute.namespace_prefix,
                                                 .namespace_uri = parts.namespace_uri});
        }
    }
    return status;
}

/* Fills the declared bindings: the wanted ones the written bindings do
   not already give, then undeclarations of written bindings the element
   lacks (xmlns="" always; xmlns:p="" with undeclare-prefixes, 5.1.11).
   Pushes them onto the written bindings. */
static sxml_status sxml_xml_writer_collect_declared(sxml_xml_writer *writer) {
    writer->declared_count = 0;
    const sxml_binding_list declared = {.bindings = &writer->declared,
                                        .binding_count = &writer->declared_count,
                                        .binding_capacity = &writer->declared_capacity};
    sxml_status status = SXML_STATUS_OK;
    for (size_t wanted_index = 0; status == SXML_STATUS_OK && wanted_index < writer->wanted_count;
         wanted_index += 1) {
        const sxml_namespace_binding binding = writer->wanted[wanted_index];
        if (sxml_xml_writer_find_written(writer, binding.namespace_prefix).index !=
            binding.namespace_uri.index) {
            status = sxml_xml_writer_append_binding(writer, declared, binding);
        }
    }
    for (size_t binding_index = writer->binding_count;
         status == SXML_STATUS_OK && binding_index > 0; binding_index -= 1) {
        const sxml_namespace_binding binding = writer->bindings[binding_index - 1];
        if (binding.namespace_uri.index == SXML_NAMESPACE_NONE ||
            sxml_xml_writer_find_wanted(writer, binding.namespace_prefix) < writer->wanted_count ||
            (binding.namespace_prefix.index != 0 && !writer->undeclares_prefixes)) {
            continue;
        }
        /* Only the innermost binding of a prefix counts. */
        bool is_shadowed = false;
        for (size_t later = binding_index; !is_shadowed && later < writer->binding_count;
             later += 1) {
            is_shadowed = writer->bindings[later].namespace_prefix.index ==
                          binding.namespace_prefix.index;
        }
        bool is_declared = false;
        for (size_t declared_index = 0; !is_declared && declared_index < writer->declared_count;
             declared_index += 1) {
            is_declared = writer->declared[declared_index].namespace_prefix.index ==
                          binding.namespace_prefix.index;
        }
        if (!is_shadowed && !is_declared) {
            status = sxml_xml_writer_append_binding(
                writer, declared,
                (sxml_namespace_binding){.namespace_prefix = binding.namespace_prefix,
                                         .namespace_uri = {.index = SXML_NAMESPACE_NONE}});
        }
    }
    for (size_t declared_index = 0;
         status == SXML_STATUS_OK && declared_index < writer->declared_count;
         declared_index += 1) {
        status = sxml_xml_writer_append_binding(
            writer,
            (sxml_binding_list){.bindings = &writer->bindings,
                                .binding_count = &writer->binding_count,
                                .binding_capacity = &writer->binding_capacity},
            writer->declared[declared_index]);
    }
    return status;
}

/* ----------------------------------------------------- Canonical order */

/* Compares two texts bytewise (codepoint order for UTF-8). */
static int sxml_compare_texts(sxml_byte_view first, sxml_byte_view second) {
    const size_t common = first.byte_count < second.byte_count ? first.byte_count
                                                                : second.byte_count;
    const int compared = common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (compared != 0) {
        return compared;
    }
    if (first.byte_count == second.byte_count) {
        return 0;
    }
    return first.byte_count < second.byte_count ? -1 : 1;
}

/* What the canonical order compares: for a declaration its prefix (the
   default namespace first), for an attribute its namespace URI, then its
   local name (Canonical XML 1.1, section 2.2). */
typedef struct sxml_canonical_key {
    sxml_byte_view primary;
    sxml_byte_view secondary;
} sxml_canonical_key;

static sxml_status sxml_xml_writer_declaration_key(const sxml_xml_writer *writer, size_t position,
                                                   sxml_canonical_key *key) {
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_name_pool_get_namespace_prefix(
        writer->serializer->name_pool, writer->declared[position].namespace_prefix, &prefix);
    *key = (sxml_canonical_key){.primary = prefix, .secondary = {.bytes = NULL, .byte_count = 0}};
    return status;
}

typedef struct sxml_attribute_key_request {
    sxml_node_reference element;
    size_t position;
} sxml_attribute_key_request;

static sxml_status sxml_xml_writer_attribute_key(const sxml_xml_writer *writer,
                                                 sxml_attribute_key_request request,
                                                 sxml_canonical_key *key) {
    sxml_node_description description;
    sxml_name_parts parts;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_node_describe(
        (sxml_node_reference){
            .tree = request.element.tree,
            .node_index = {.index = request.element.node_index.index +
                                    writer->attribute_numbers[request.position]},
            .namespace_number = 0},
        &description);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                               description.node_name, &parts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(writer->serializer->name_pool,
                                                  parts.namespace_uri, &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        *key = (sxml_canonical_key){.primary = namespace_uri, .secondary = parts.local_name};
    }
    return status;
}

/* What to sort into canonical order: the declarations, or the attribute
   numbers of element. */
typedef struct sxml_canonical_sort {
    bool is_attributes;
    sxml_node_reference element;
    size_t count;
} sxml_canonical_sort;

/* One position of a canonical sort. */
typedef struct sxml_canonical_position {
    const sxml_canonical_sort *sort;
    size_t position;
} sxml_canonical_position;

static sxml_status sxml_xml_writer_key_at(const sxml_xml_writer *writer,
                                          sxml_canonical_position position,
                                          sxml_canonical_key *key) {
    if (position.sort->is_attributes) {
        return sxml_xml_writer_attribute_key(
            writer,
            (sxml_attribute_key_request){.element = position.sort->element,
                                         .position = position.position},
            key);
    }
    return sxml_xml_writer_declaration_key(writer, position.position, key);
}

/* Two positions to compare, and the outcome (in/out). */
typedef struct sxml_position_comparison {
    size_t first;
    size_t second;
    /* Set to whether the key at first sorts after the key at second. */
    bool is_greater;
} sxml_position_comparison;

static sxml_status sxml_xml_writer_compare(const sxml_xml_writer *writer,
                                           const sxml_canonical_sort *sort,
                                           sxml_position_comparison *comparison) {
    sxml_canonical_key first_key;
    sxml_canonical_key second_key;
    sxml_status status = sxml_xml_writer_key_at(
        writer, (sxml_canonical_position){.sort = sort, .position = comparison->first},
        &first_key);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_key_at(
            writer, (sxml_canonical_position){.sort = sort, .position = comparison->second},
            &second_key);
    }
    if (status == SXML_STATUS_OK) {
        int ordering = sxml_compare_texts(first_key.primary, second_key.primary);
        if (ordering == 0) {
            ordering = sxml_compare_texts(first_key.secondary, second_key.secondary);
        }
        comparison->is_greater = ordering > 0;
    }
    return status;
}

static void sxml_xml_writer_swap(sxml_xml_writer *writer, const sxml_canonical_sort *sort,
                                 sxml_position_comparison positions) {
    if (sort->is_attributes) {
        const uint32_t swapped = writer->attribute_numbers[positions.first];
        writer->attribute_numbers[positions.first] = writer->attribute_numbers[positions.second];
        writer->attribute_numbers[positions.second] = swapped;
    } else {
        const sxml_namespace_binding swapped = writer->declared[positions.first];
        writer->declared[positions.first] = writer->declared[positions.second];
        writer->declared[positions.second] = swapped;
    }
}

/* Sorts declarations or attributes into canonical order: heapsort, so
   the work stays O(n log n) for any number of attributes, without
   recursion. */
static sxml_status sxml_xml_writer_sort(sxml_xml_writer *writer, const sxml_canonical_sort *sort) {
    const size_t count = sort->count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t sort_pass = 0; status == SXML_STATUS_OK && sort_pass < 2 * count; sort_pass += 1) {
        const bool is_building = sort_pass < count;
        size_t heap_size = count;
        size_t root = 0;
        if (is_building) {
            root = count - 1 - sort_pass;
        } else {
            heap_size = 2 * count - sort_pass - 1;
            sxml_xml_writer_swap(writer, sort,
                                 (sxml_position_comparison){.first = 0, .second = heap_size});
        }
        while (status == SXML_STATUS_OK) {
            size_t largest = root;
            const size_t children[2] = {2 * root + 1, 2 * root + 2};
            for (size_t child_index = 0; status == SXML_STATUS_OK && child_index < 2;
                 child_index += 1) {
                if (children[child_index] >= heap_size) {
                    continue;
                }
                sxml_position_comparison comparison = {
                    .first = children[child_index], .second = largest, .is_greater = false};
                status = sxml_xml_writer_compare(writer, sort, &comparison);
                if (comparison.is_greater) {
                    largest = children[child_index];
                }
            }
            if (largest == root) {
                break;
            }
            sxml_xml_writer_swap(writer, sort,
                                 (sxml_position_comparison){.first = root, .second = largest});
            root = largest;
        }
    }
    return status;
}

/* ------------------------------------------------------------- Names */

/* Writes prefix:local, or local alone for an empty prefix. */
typedef struct sxml_written_qname {
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_byte_view local_name;
} sxml_written_qname;

static sxml_status sxml_xml_writer_write_qname(sxml_xml_writer *writer,
                                               sxml_written_qname qname) {
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    if (qname.namespace_prefix.index != 0) {
        status = sxml_name_pool_get_namespace_prefix(writer->serializer->name_pool,
                                                     qname.namespace_prefix, &prefix);
    }
    if (status == SXML_STATUS_OK && prefix.byte_count > 0) {
        status = sxml_xml_writer_markup(writer, prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(":"));
        }
    }
    return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, qname.local_name) : status;
}

/* Writes xmlns="uri" or xmlns:prefix="uri" (an empty URI undeclares). */
static sxml_status sxml_xml_writer_write_declaration(sxml_xml_writer *writer,
                                                     sxml_namespace_binding binding) {
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    const sxml_name_pool *name_pool = writer->serializer->name_pool;
    sxml_status status = SXML_STATUS_OK;
    if (binding.namespace_prefix.index != 0) {
        status = sxml_name_pool_get_namespace_prefix(name_pool, binding.namespace_prefix, &prefix);
    }
    if (status == SXML_STATUS_OK && binding.namespace_uri.index != SXML_NAMESPACE_NONE) {
        status = sxml_name_pool_get_namespace_uri(name_pool, binding.namespace_uri, &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, prefix.byte_count > 0 ? SXML_BYTE_VIEW_FROM_LITERAL("xmlns:") : SXML_BYTE_VIEW_FROM_LITERAL("xmlns"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("=\""));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_character_data(
            writer->serializer,
            &(sxml_character_data){.text = namespace_uri,
                                   .character_context = SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE});
    }
    return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\"")) : status;
}

/* Canonical XML 1.1, section 2.1 (via Serialization 4.0, 5.1.1): a
   relative namespace URI is an error; an absolute one starts with a
   scheme (RFC 3986, section 3.1). */
static bool sxml_namespace_uri_is_absolute(sxml_byte_view namespace_uri) {
    if (namespace_uri.byte_count == 0) {
        return true;
    }
    for (size_t offset = 0; offset < namespace_uri.byte_count; offset += 1) {
        const uint8_t byte = namespace_uri.bytes[offset];
        const bool is_letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (byte == ':') {
            return offset > 0;
        }
        if (!is_letter &&
            (offset == 0 || !((byte >= '0' && byte <= '9') || byte == '+' || byte == '-' ||
                              byte == '.'))) {
            return false;
        }
    }
    return false;
}

/* ----------------------------------------------------- Element facts */

/* Whether name is one of the element names of list (cdata-section-
   elements, suppress-indentation). */
static bool sxml_element_names_contain(sxml_element_name_view list, sxml_name_identifier name) {
    for (size_t name_index = 0; name_index < list.element_name_count; name_index += 1) {
        if (list.element_names[name_index].index == name.index) {
            return true;
        }
    }
    return false;
}

/* suppress-indentation: the expanded name matches, and for XHTML also
   without regard to case in no namespace and, with HTML5, between no
   namespace and XHTML (Serialization 4.0, 6.1.5). */
static sxml_status sxml_xml_writer_is_suppressed(const sxml_xml_writer *writer,
                                                 const sxml_element_facts *facts,
                                                 sxml_answer *is_suppressed_answer) {
    const sxml_element_name_view list = writer->parameters->suppress_indentation;
    is_suppressed_answer->is_true = sxml_element_names_contain(list, facts->description.node_name);
    for (size_t name_index = 0;
         writer->has_html_rules && !is_suppressed_answer->is_true && name_index < list.element_name_count;
         name_index += 1) {
        sxml_name_parts parts;
        const sxml_status status = sxml_name_pool_get_name_parts(
            writer->serializer->name_pool, list.element_names[name_index], &parts);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const uint32_t first = parts.namespace_uri.index;
        const uint32_t second = facts->name_parts.namespace_uri.index;
        const bool are_null = first == SXML_NAMESPACE_NONE && second == SXML_NAMESPACE_NONE;
        const bool are_html5_pair =
            writer->is_html5 && writer->xhtml_namespace.index != SXML_NAMESPACE_NONE &&
            ((first == SXML_NAMESPACE_NONE && second == writer->xhtml_namespace.index) ||
             (second == SXML_NAMESPACE_NONE && first == writer->xhtml_namespace.index));
        if (are_null || are_html5_pair) {
            const sxml_byte_view local_name = facts->name_parts.local_name;
            bool is_equal = local_name.byte_count == parts.local_name.byte_count;
            for (size_t offset = 0; is_equal && offset < local_name.byte_count; offset += 1) {
                uint8_t first_byte = local_name.bytes[offset];
                uint8_t second_byte = parts.local_name.bytes[offset];
                first_byte = (first_byte >= 'A' && first_byte <= 'Z')
                                 ? (uint8_t)(first_byte - 'A' + 'a')
                                 : first_byte;
                second_byte = (second_byte >= 'A' && second_byte <= 'Z')
                                  ? (uint8_t)(second_byte - 'A' + 'a')
                                  : second_byte;
                is_equal = first_byte == second_byte;
            }
            is_suppressed_answer->is_true = is_equal;
        }
    }
    return SXML_STATUS_OK;
}

/* Whether element has the attribute xml:space="preserve" (XML 1.0,
   section 2.10; Serialization 4.0, 5.1.4). */
static sxml_status sxml_xml_writer_preserves_space(const sxml_xml_writer *writer,
                                                   const sxml_element_facts *facts,
                                                   sxml_answer *preserves_answer) {
    preserves_answer->is_true = false;
    for (uint32_t attribute_number = 1; attribute_number <= facts->description.attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = facts->element.tree,
            .node_index = {.index = facts->element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description description;
        sxml_name_parts parts;
        sxml_status status = sxml_node_describe(attribute, &description);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                                   description.node_name, &parts);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (parts.namespace_uri.index == SXML_NAMESPACE_XML &&
            sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("space"))) {
            sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(attribute, &value);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            preserves_answer->is_true = sxml_byte_view_equals(value, SXML_BYTE_VIEW_FROM_LITERAL("preserve"));
        }
    }
    return SXML_STATUS_OK;
}

/* What an element's children allow for indentation. */
typedef struct sxml_children_survey {
    bool has_markup_child;
    bool has_text_content;
    bool has_inline_child;
    bool has_element_child;
} sxml_children_survey;

static bool sxml_xml_writer_is_inline(const sxml_xml_writer *writer,
                                      const sxml_element_facts *facts) {
    if (!writer->has_html_rules || !facts->is_html_element) {
        return false;
    }
    const sxml_byte_view local_name = facts->name_parts.local_name;
    /* Section 7.1: an element the HTML method does not know is written as
       span is, an inline element. */
    if (writer->is_html &&
        !sxml_name_list_contains(SXML_NAME_LIST(sxml_html5_element_names), local_name) &&
        !sxml_html_element_is_void(
            (sxml_html_element_query){.local_name = local_name, .is_html5 = false})) {
        return true;
    }
    if (sxml_equals_ignoring_case(local_name, SXML_BYTE_VIEW_FROM_LITERAL("ins")) ||
        sxml_equals_ignoring_case(local_name, SXML_BYTE_VIEW_FROM_LITERAL("del"))) {
        return !facts->has_element_child;
    }
    return sxml_name_list_contains(SXML_NAME_LIST(sxml_inline_elements), local_name);
}

/* Fills facts->is_html_element for an element (Serialization 4.0,
   section 6: in the XHTML namespace, or with HTML5 in no namespace with
   the name of an HTML5 element). */
static void sxml_xml_writer_recognize(const sxml_xml_writer *writer, sxml_element_facts *facts) {
    const uint32_t namespace_index = facts->name_parts.namespace_uri.index;
    const bool is_xhtml_namespace = namespace_index != SXML_NAMESPACE_NONE &&
                                    namespace_index == writer->xhtml_namespace.index;
    if (writer->is_html) {
        /* Section 7.1: in no namespace, or with HTML5 in the XHTML
           namespace. */
        facts->is_html_element = namespace_index == SXML_NAMESPACE_NONE ||
                                 (writer->is_html5 && is_xhtml_namespace);
        return;
    }
    facts->is_html_element =
        writer->is_xhtml &&
        (is_xhtml_namespace ||
         (writer->is_html5 && namespace_index == SXML_NAMESPACE_NONE &&
          sxml_name_list_contains(SXML_NAME_LIST(sxml_html5_element_names),
                                  facts->name_parts.local_name)));
}

static sxml_status sxml_xml_writer_describe_element(const sxml_xml_writer *writer,
                                                    sxml_node_reference element,
                                                    sxml_element_facts *facts) {
    facts->element = element;
    sxml_status status = sxml_node_describe(element, &facts->description);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                               facts->description.node_name, &facts->name_parts);
    }
    if (status == SXML_STATUS_OK) {
        sxml_xml_writer_recognize(writer, facts);
        facts->is_unprefixed =
            writer->has_html_rules && writer->is_html5 && !writer->is_canonical &&
            sxml_xml_writer_is_normalized_namespace(writer, facts->name_parts.namespace_uri);
    }
    return status;
}

/* Whether element has an element child. */
static sxml_status sxml_element_has_element_child(sxml_node_reference element,
                                                  sxml_answer *has_element_child_answer) {
    has_element_child_answer->is_true = false;
    sxml_node_description description;
    sxml_status status = sxml_node_describe(element, &description);
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child && !has_element_child_answer->is_true) {
        sxml_node_description child_description;
        status = sxml_node_describe(
            (sxml_node_reference){.tree = element.tree, .node_index = child, .namespace_number = 0},
            &child_description);
        if (status == SXML_STATUS_OK) {
            has_element_child_answer->is_true = child_description.node_kind == SXML_NODE_KIND_ELEMENT;
            has_child = child_description.has_next_sibling;
            child = child_description.next_sibling;
        }
    }
    return status;
}

static sxml_status sxml_xml_writer_survey_children(const sxml_xml_writer *writer,
                                                   const sxml_element_facts *facts,
                                                   sxml_children_survey *survey) {
    *survey = (sxml_children_survey){.has_markup_child = false};
    bool has_child = facts->description.has_children;
    sxml_node_index child = facts->description.first_child;
    while (has_child) {
        const sxml_node_reference reference = {
            .tree = facts->element.tree, .node_index = child, .namespace_number = 0};
        sxml_node_description description;
        sxml_status status = sxml_node_describe(reference, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(reference, &content);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            survey->has_text_content = survey->has_text_content || !sxml_text_is_whitespace(content);
        } else {
            survey->has_markup_child = true;
        }
        if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
            survey->has_element_child = true;
            if (writer->has_html_rules) {
                sxml_element_facts child_facts;
                status = sxml_xml_writer_describe_element(writer, reference, &child_facts);
                if (status == SXML_STATUS_OK) {
                    sxml_answer has_element_child = {.is_true = false};
                    status = sxml_element_has_element_child(reference, &has_element_child);
                    child_facts.has_element_child = has_element_child.is_true;
                }
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                survey->has_inline_child =
                    survey->has_inline_child || sxml_xml_writer_is_inline(writer, &child_facts);
            }
        }
        has_child = description.has_next_sibling;
        child = description.next_sibling;
    }
    return SXML_STATUS_OK;
}

/* Whether whitespace is added inside an element (Serialization 4.0,
   5.1.4 and 6.1.5): only where every constraint allows it, so not in
   mixed content, suppress-indentation, xml:space="preserve", formatted
   and inline HTML elements, nor below an element that is not indented. */
static sxml_status sxml_xml_writer_decides_indenting(const sxml_xml_writer *writer,
                                                     const sxml_element_facts *facts,
                                                     sxml_answer *is_indenting_answer) {
    is_indenting_answer->is_true = false;
    if (!writer->is_indenting || facts->is_suppressed) {
        return SXML_STATUS_OK;
    }
    sxml_answer is_excluded = {.is_true = false};
    sxml_status status = sxml_xml_writer_is_suppressed(writer, facts, &is_excluded);
    if (status == SXML_STATUS_OK && !is_excluded.is_true) {
        status = sxml_xml_writer_preserves_space(writer, facts, &is_excluded);
    }
    if (status != SXML_STATUS_OK || is_excluded.is_true) {
        return status;
    }
    if (facts->is_html_element && sxml_name_list_contains(SXML_NAME_LIST(sxml_formatted_elements),
                                                          facts->name_parts.local_name)) {
        return SXML_STATUS_OK;
    }
    sxml_children_survey survey;
    status = sxml_xml_writer_survey_children(writer, facts, &survey);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_element_facts surveyed = *facts;
    surveyed.has_element_child = survey.has_element_child;
    const bool has_markup = survey.has_markup_child || facts->has_content_type_meta;
    is_indenting_answer->is_true = has_markup && !survey.has_text_content && !survey.has_inline_child &&
                    !sxml_xml_writer_is_inline(writer, &surveyed);
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------- Attributes */

/* Whether attribute (in no namespace) of an HTML element holds a URI
   (Serialization 4.0, appendix D). */
static bool sxml_is_uri_attribute(sxml_byte_view element_name, sxml_byte_view attribute_name) {
    for (size_t entry_index = 0;
         entry_index < sizeof(sxml_uri_attributes) / sizeof(sxml_uri_attributes[0]);
         entry_index += 1) {
        if (sxml_equals_ignoring_case(attribute_name,
                                      sxml_uri_attributes[entry_index].attribute_name) &&
            sxml_equals_ignoring_case(element_name, sxml_uri_attributes[entry_index].element_name)) {
            return true;
        }
    }
    return false;
}

/* URI escaping (Serialization 4.0, section 4, rule a): NFC, then
   fn:escape-html-uri (F&O 4.0, 7.3: every character outside printable
   ASCII as %HH of its UTF-8 bytes), then the escaping of attributes. */
static sxml_status sxml_xml_writer_write_uri_value(sxml_xml_writer *writer, sxml_byte_view value) {
    sxml_serializer *serializer = writer->serializer;
    sxml_unicode_normalized_text normalized = {.allocator = serializer->allocator,
                                               .bytes = NULL,
                                               .byte_count = 0};
    sxml_string input;
    sxml_status status = sxml_string_from_utf8(value, &input);
    if (status == SXML_STATUS_OK) {
        status = sxml_unicode_normalize(
            &(sxml_unicode_normalization_request){
                .allocator = serializer->allocator,
                .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC,
                .input = input},
            &normalized);
    }
    const sxml_byte_view text = status == SXML_STATUS_OK
                                    ? (input.is_ascii ? value
                                                      : sxml_unicode_normalized_text_view(&normalized))
                                    : value;
    size_t run_start = 0;
    for (size_t offset = 0; status == SXML_STATUS_OK && offset <= text.byte_count; offset += 1) {
        const bool is_end = offset == text.byte_count;
        if (!is_end && text.bytes[offset] >= 0x20U && text.bytes[offset] <= 0x7EU) {
            continue;
        }
        status = sxml_serializer_write_character_data(
            serializer, &(sxml_character_data){
                            .text = {.bytes = text.bytes + run_start, .byte_count = offset - run_start},
                            .character_context = SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE});
        if (status == SXML_STATUS_OK && !is_end) {
            static const char hexadecimal_digits[] = "0123456789ABCDEF";
            const uint8_t escaped[3] = {'%', (uint8_t)hexadecimal_digits[text.bytes[offset] >> 4],
                                        (uint8_t)hexadecimal_digits[text.bytes[offset] & 0xFU]};
            status = sxml_xml_writer_markup(writer,
                                            (sxml_byte_view){.bytes = escaped, .byte_count = 3});
        }
        run_start = offset + 1;
    }
    sxml_unicode_normalized_text_release(&normalized);
    return status;
}

/* Writes name="value" for an attribute; facts is its element's, or NULL
   for an attribute written alone (the adaptive method). */
static sxml_status sxml_xml_writer_write_attribute(sxml_xml_writer *writer,
                                                   const sxml_element_facts *facts,
                                                   sxml_node_reference attribute) {
    sxml_node_description description;
    sxml_name_parts parts;
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_node_describe(attribute, &description);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                               description.node_name, &parts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_content(attribute, &value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_write_qname(
            writer, (sxml_written_qname){.namespace_prefix = description.namespace_prefix,
                                         .local_name = parts.local_name});
    }
    const bool is_html_attribute = writer->is_html && facts != NULL && facts->is_html_element &&
                                   parts.namespace_uri.index == SXML_NAMESPACE_NONE;
    if (status != SXML_STATUS_OK ||
        (is_html_attribute &&
         sxml_html_attribute_is_minimized((sxml_html_attribute_query){
             .attribute_name = parts.local_name, .value = value}))) {
        return status;
    }
    const sxml_xml_open_element *parent = writer->open_element_count == 0
                                              ? NULL
                                              : &writer->open_elements[writer->open_element_count - 1];
    const bool is_raw = facts != NULL && parent != NULL && parent->is_raw_text;
    status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("=\""));
    if (status == SXML_STATUS_OK && is_raw) {
        status = sxml_serializer_write_character_data(
            writer->serializer,
            &(sxml_character_data){.text = value,
                                   .character_context = SXML_CHARACTER_CONTEXT_RAW_TEXT});
    } else if (status == SXML_STATUS_OK) {
        const bool is_uri = writer->escapes_uri_attributes && facts != NULL &&
                            facts->is_html_element &&
                            parts.namespace_uri.index == SXML_NAMESPACE_NONE &&
                            sxml_is_uri_attribute(facts->name_parts.local_name, parts.local_name);
        status = is_uri ? sxml_xml_writer_write_uri_value(writer, value)
                        : sxml_serializer_write_character_data(
                              writer->serializer,
                              &(sxml_character_data){
                                  .text = value,
                                  .character_context = SXML_CHARACTER_CONTEXT_ATTRIBUTE});
    }
    return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\"")) : status;
}

/* --------------------------------------------------- Start and end tags */

static sxml_xml_open_element *sxml_xml_writer_top(sxml_xml_writer *writer) {
    return writer->open_element_count == 0
               ? NULL
               : &writer->open_elements[writer->open_element_count - 1];
}

/* Writes the meta element include-content-type adds as the first child
   of head (Serialization 4.0, 6.1.18). */
static sxml_status sxml_xml_writer_write_meta(sxml_xml_writer *writer,
                                              const sxml_element_facts *head) {
    const sxml_byte_view encoding_name = sxml_serializer_get_encoding_name(writer->serializer);
    sxml_status status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<"));
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_write_qname(
            writer,
            (sxml_written_qname){
                .namespace_prefix = {.index = head->is_unprefixed
                                                  ? 0
                                                  : head->description.namespace_prefix.index},
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("meta")});
    }
    if (status == SXML_STATUS_OK && writer->is_html5) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" charset=\""));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, encoding_name);
        }
        return status == SXML_STATUS_OK
                   ? sxml_xml_writer_markup(writer, writer->is_html
                                                        ? SXML_BYTE_VIEW_FROM_LITERAL("\">")
                                                        : SXML_BYTE_VIEW_FROM_LITERAL("\"/>"))
                   : status;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" http-equiv=\"Content-Type\" content=\""));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_character_data(
            writer->serializer,
            &(sxml_character_data){.text = writer->parameters->has_media_type
                                               ? writer->parameters->media_type
                                               : SXML_BYTE_VIEW_FROM_LITERAL("text/html"),
                                   .character_context = SXML_CHARACTER_CONTEXT_ATTRIBUTE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("; charset="));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, encoding_name);
    }
    return status == SXML_STATUS_OK
               ? sxml_xml_writer_markup(writer, writer->is_html ? SXML_BYTE_VIEW_FROM_LITERAL("\">")
                                                                : SXML_BYTE_VIEW_FROM_LITERAL("\" />"))
               : status;
}

/* Whether element, a child of a head that received a meta element, is a
   meta element to leave out: one with charset, or http-equiv equal to
   Content-Type without regard to case and surrounding spaces (6.1.18). */
static sxml_status sxml_xml_writer_is_replaced_meta(const sxml_xml_writer *writer,
                                                    sxml_node_reference element,
                                                    sxml_answer *is_replaced_answer) {
    is_replaced_answer->is_true = false;
    sxml_element_facts facts;
    sxml_status status = sxml_xml_writer_describe_element(writer, element, &facts);
    if (status != SXML_STATUS_OK || !facts.is_html_element ||
        !sxml_equals_ignoring_case(facts.name_parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("meta"))) {
        return status;
    }
    for (uint32_t attribute_number = 1; attribute_number <= facts.description.attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = element.tree,
            .node_index = {.index = element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description description;
        sxml_name_parts parts;
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_describe(attribute, &description);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                                   description.node_name, &parts);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(attribute, &value);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
            continue;
        }
        while (value.byte_count > 0 && value.bytes[0] == ' ') {
            value.bytes += 1;
            value.byte_count -= 1;
        }
        while (value.byte_count > 0 && value.bytes[value.byte_count - 1] == ' ') {
            value.byte_count -= 1;
        }
        if (sxml_equals_ignoring_case(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("charset")) ||
            (sxml_equals_ignoring_case(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("http-equiv")) &&
             sxml_equals_ignoring_case(value, SXML_BYTE_VIEW_FROM_LITERAL("content-type")))) {
            is_replaced_answer->is_true = true;
        }
    }
    return SXML_STATUS_OK;
}

/* Writes the namespace declarations and attributes of a start tag, in
   canonical order when canonical (Canonical XML 1.1, section 2.2). */
static sxml_status sxml_xml_writer_write_tag_content(sxml_xml_writer *writer,
                                                     const sxml_element_facts *facts) {
    sxml_status status = SXML_STATUS_OK;
    const uint32_t attribute_count = facts->description.attribute_count;
    writer->attribute_number_count = 0;
    if (attribute_count > writer->attribute_number_capacity) {
        void *memory = NULL;
        size_t byte_count = 0;
        status = sxml_size_multiply(attribute_count, sizeof(uint32_t), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                writer->serializer->allocator,
                (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(uint32_t)},
                &memory);
        }
        if (status == SXML_STATUS_OK) {
            sxml_allocator_release(writer->serializer->allocator, writer->attribute_numbers);
            writer->attribute_numbers = memory;
            writer->attribute_number_capacity = attribute_count;
        }
    }
    for (uint32_t attribute_number = 1; status == SXML_STATUS_OK && attribute_number <= attribute_count;
         attribute_number += 1) {
        writer->attribute_numbers[attribute_number - 1] = attribute_number;
    }
    writer->attribute_number_count = status == SXML_STATUS_OK ? attribute_count : 0;
    if (status == SXML_STATUS_OK && writer->is_canonical) {
        status = sxml_xml_writer_sort(
            writer, &(sxml_canonical_sort){.is_attributes = false,
                                           .element = facts->element,
                                           .count = writer->declared_count});
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_sort(
                writer, &(sxml_canonical_sort){.is_attributes = true,
                                               .element = facts->element,
                                               .count = attribute_count});
        }
    }
    const size_t item_count = writer->declared_count + attribute_count;
    const bool puts_on_lines = writer->is_indenting && writer->indents_attributes && item_count > 1;
    for (size_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        status = puts_on_lines && item_index > 0 ? sxml_xml_writer_indent(writer, facts->depth + 1)
                                                 : sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (item_index < writer->declared_count) {
            status = sxml_xml_writer_write_declaration(writer, writer->declared[item_index]);
            continue;
        }
        const uint32_t attribute_number =
            writer->attribute_numbers[item_index - writer->declared_count];
        status = sxml_xml_writer_write_attribute(
            writer, facts,
            (sxml_node_reference){
                .tree = facts->element.tree,
                .node_index = {.index = facts->element.node_index.index + attribute_number},
                .namespace_number = 0});
    }
    return status;
}

/* Whether a childless element is written as an empty-element tag: always
   for XML; for XHTML only when it is expected to be empty (section 6);
   never for Canonical XML. */
static bool sxml_xml_writer_minimizes(const sxml_xml_writer *writer,
                                      const sxml_element_facts *facts) {
    if (writer->is_canonical) {
        return false;
    }
    if (!writer->is_xhtml) {
        return true;
    }
    return facts->is_html_element &&
           sxml_html_element_is_void((sxml_html_element_query){
               .local_name = facts->name_parts.local_name, .is_html5 = writer->is_html5});
}

static sxml_status sxml_xml_writer_write_end_tag(sxml_xml_writer *writer,
                                                 const sxml_element_facts *facts) {
    sxml_status status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("</"));
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_write_qname(
            writer,
            (sxml_written_qname){
                .namespace_prefix = {.index = facts->is_unprefixed
                                                  ? 0
                                                  : facts->description.namespace_prefix.index},
                .local_name = facts->name_parts.local_name});
    }
    return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(">")) : status;
}

/* Writes an element's start tag and, when it has content, pushes it as
   an open element; parent is the open element it is in, or NULL. */
static sxml_status sxml_xml_writer_open_element(sxml_xml_writer *writer,
                                                sxml_node_reference element,
                                                const sxml_xml_open_element *parent) {
    sxml_element_facts facts;
    sxml_status status = sxml_xml_writer_describe_element(writer, element, &facts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    facts.depth = parent == NULL ? 0 : parent->depth + 1;
    facts.is_suppressed = parent != NULL && !parent->is_indenting;
    facts.has_element_child = false;
    facts.has_content_type_meta = writer->includes_content_type && facts.is_html_element &&
                                  sxml_equals_ignoring_case(facts.name_parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("head"));
    sxml_answer indents = {.is_true = false};
    status = sxml_xml_writer_decides_indenting(writer, &facts, &indents);
    const bool is_indenting = indents.is_true;
    const size_t binding_mark = writer->binding_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_collect_wanted(writer, &facts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_collect_declared(writer);
    }
    for (size_t declared_index = 0;
         status == SXML_STATUS_OK && writer->is_canonical && declared_index < writer->declared_count;
         declared_index += 1) {
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        const sxml_namespace_uri_identifier identifier = writer->declared[declared_index].namespace_uri;
        if (identifier.index != SXML_NAMESPACE_NONE) {
            status = sxml_name_pool_get_namespace_uri(writer->serializer->name_pool, identifier,
                                                      &namespace_uri);
        }
        if (status == SXML_STATUS_OK && !sxml_namespace_uri_is_absolute(namespace_uri)) {
            status = sxml_xml_writer_raise(writer, SXML_XML_ERROR("SERE0024", "Canonical XML does not allow a relative namespace URI"));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_write_qname(
            writer,
            (sxml_written_qname){
                .namespace_prefix = {.index = facts.is_unprefixed
                                                  ? 0
                                                  : facts.description.namespace_prefix.index},
                .local_name = facts.name_parts.local_name});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_write_tag_content(writer, &facts);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_html_syntax = writer->is_html && facts.is_html_element;
    /* Read before the stack grows: parent points into it. */
    const bool is_in_raw_text = parent != NULL && parent->is_raw_text;
    if (!facts.description.has_children && !facts.has_content_type_meta && is_html_syntax) {
        /* Section 7.1: no end tag for an empty element with an empty
           content model; never an empty-element tag. */
        writer->binding_count = binding_mark;
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(">"));
        const bool is_void = sxml_html_element_is_void((sxml_html_element_query){
            .local_name = facts.name_parts.local_name, .is_html5 = writer->is_html5});
        return status == SXML_STATUS_OK && !is_void ? sxml_xml_writer_write_end_tag(writer, &facts)
                                                    : status;
    }
    if (!facts.description.has_children && !facts.has_content_type_meta) {
        writer->binding_count = binding_mark;
        if (sxml_xml_writer_minimizes(writer, &facts)) {
            return sxml_xml_writer_markup(
                writer, writer->is_xhtml && !writer->is_html5 ? SXML_BYTE_VIEW_FROM_LITERAL(" />") : SXML_BYTE_VIEW_FROM_LITERAL("/>"));
        }
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(">"));
        return status == SXML_STATUS_OK ? sxml_xml_writer_write_end_tag(writer, &facts) : status;
    }
    status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(">"));
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_grow(
            writer->serializer->allocator,
            &(sxml_serializer_growth){.elements = (void **)&writer->open_elements,
                                      .element_capacity = &writer->open_element_capacity,
                                      .element_count = writer->open_element_count,
                                      .element_byte_count = sizeof(sxml_xml_open_element),
                                      .alignment = alignof(sxml_xml_open_element)});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    writer->open_elements[writer->open_element_count] = (sxml_xml_open_element){
        .element = element.node_index,
        .last_descendant = facts.description.last_descendant,
        .binding_mark = binding_mark,
        .depth = facts.depth,
        .is_indenting = is_indenting,
        .has_written_child = facts.has_content_type_meta,
        /* Section 7.5.7: cdata-section-elements applies to XML islands
           only with the HTML method. */
        .is_cdata_parent = !writer->is_canonical && !is_html_syntax &&
                           sxml_element_names_contain(writer->parameters->cdata_section_elements,
                                                      facts.description.node_name),
        .is_content_type_head = facts.has_content_type_meta,
        .is_unprefixed = facts.is_unprefixed,
        .is_raw_text = is_in_raw_text ||
                       (is_html_syntax &&
                        sxml_html_element_has_raw_text(facts.name_parts.local_name)),
    };
    writer->open_element_count += 1;
    if (facts.has_content_type_meta) {
        if (is_indenting) {
            status = sxml_xml_writer_indent(writer, facts.depth + 1);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_write_meta(writer, &facts);
        }
    }
    return status;
}

/* Closes the open elements that end before node_index (all for
   UINT64_MAX), writing their end tags. */
typedef struct sxml_closing {
    sxml_tree *tree;
    uint64_t node_index;
} sxml_closing;

static sxml_status sxml_xml_writer_close_elements(sxml_xml_writer *writer, sxml_closing closing) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && writer->open_element_count > 0) {
        const sxml_xml_open_element open = writer->open_elements[writer->open_element_count - 1];
        if (open.last_descendant.index >= closing.node_index) {
            break;
        }
        writer->open_element_count -= 1;
        writer->binding_count = open.binding_mark;
        if (open.is_indenting && open.has_written_child) {
            status = sxml_xml_writer_indent(writer, open.depth);
        }
        sxml_element_facts facts;
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_describe_element(
                writer,
                (sxml_node_reference){.tree = closing.tree, .node_index = open.element,
                                      .namespace_number = 0},
                &facts);
        }
        if (status == SXML_STATUS_OK) {
            facts.is_unprefixed = open.is_unprefixed;
            status = sxml_xml_writer_write_end_tag(writer, &facts);
        }
    }
    return status;
}

/* ------------------------------------------------------------- Leaves */

/* SERE0003 (Serialization 4.0, 5.1): a comment with "--" or ending in
   "-", or a processing instruction with "?>", cannot be written as XML. */
static sxml_status sxml_xml_writer_check_construct(sxml_xml_writer *writer, sxml_node_kind kind,
                                                   sxml_byte_view content) {
    bool is_bad = false;
    for (size_t offset = 0; !is_bad && offset + 1 < content.byte_count; offset += 1) {
        is_bad = kind == SXML_NODE_KIND_COMMENT
                     ? content.bytes[offset] == '-' && content.bytes[offset + 1] == '-'
                     : content.bytes[offset] == '?' && content.bytes[offset + 1] == '>';
    }
    if (kind == SXML_NODE_KIND_COMMENT && content.byte_count > 0 &&
        content.bytes[content.byte_count - 1] == '-') {
        is_bad = true;
    }
    return is_bad ? sxml_xml_writer_raise(
                        writer, SXML_XML_ERROR("SERE0003", "a comment or processing instruction "
                                                           "cannot be written as well-formed XML"))
                  : SXML_STATUS_OK;
}

/* Writes a text node's content, a comment or a processing instruction. */
static sxml_status sxml_xml_writer_write_leaf(sxml_xml_writer *writer, sxml_node_reference node,
                                              const sxml_node_description *description) {
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_node_get_content(node, &content);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description->node_kind == SXML_NODE_KIND_TEXT) {
        const sxml_xml_open_element *parent = sxml_xml_writer_top(writer);
        sxml_character_context character_context = SXML_CHARACTER_CONTEXT_TEXT;
        if (parent != NULL && parent->is_cdata_parent) {
            character_context = SXML_CHARACTER_CONTEXT_CDATA_SECTION;
        } else if (parent != NULL && parent->is_raw_text) {
            character_context = SXML_CHARACTER_CONTEXT_RAW_TEXT;
        }
        return sxml_serializer_write_character_data(
            writer->serializer,
            &(sxml_character_data){.text = content, .character_context = character_context});
    }
    if (writer->checks_xml_constructs) {
        status = sxml_xml_writer_check_construct(writer, description->node_kind, content);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (description->node_kind == SXML_NODE_KIND_COMMENT) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<!--"));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, content);
        }
        return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("-->")) : status;
    }
    sxml_name_parts parts;
    status = sxml_name_pool_get_name_parts(writer->serializer->name_pool, description->node_name,
                                           &parts);
    if (status == SXML_STATUS_OK && writer->is_html) {
        return sxml_serializer_write_html_processing_instruction(
            writer->serializer,
            &(sxml_html_processing_instruction){
                .target = parts.local_name, .data = content, .is_html5 = writer->is_html5});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<?"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, parts.local_name);
    }
    if (status == SXML_STATUS_OK && content.byte_count > 0) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, content);
        }
    }
    return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("?>")) : status;
}

/* ------------------------------------------------------------- Subtrees */

/* Writes the subtree of a document or element node in document order: a
   loop over node indices up to the last descendant, with the open
   elements on a stack (D71). */
static sxml_status sxml_xml_writer_write_subtree(sxml_xml_writer *writer,
                                                 sxml_node_reference node) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t last_index = description.last_descendant.index;
    writer->open_element_count = 0;
    for (uint64_t node_index = node.node_index.index;
         status == SXML_STATUS_OK && node_index <= last_index;) {
        status = sxml_xml_writer_close_elements(
            writer, (sxml_closing){.tree = node.tree, .node_index = node_index});
        const sxml_node_reference current = {.tree = node.tree,
                                             .node_index = {.index = (uint32_t)node_index},
                                             .namespace_number = 0};
        sxml_node_description current_description;
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(current, &current_description);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_xml_open_element *parent = sxml_xml_writer_top(writer);
        const sxml_node_kind kind = current_description.node_kind;
        if (kind == SXML_NODE_KIND_ELEMENT && parent != NULL && parent->is_content_type_head) {
            sxml_answer is_replaced = {.is_true = false};
            status = sxml_xml_writer_is_replaced_meta(writer, current, &is_replaced);
            if (status == SXML_STATUS_OK && is_replaced.is_true) {
                node_index = (uint64_t)current_description.last_descendant.index + 1;
                continue;
            }
        }
        if (kind == SXML_NODE_KIND_TEXT && parent != NULL && parent->is_indenting) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(current, &content);
            if (status == SXML_STATUS_OK && sxml_text_is_whitespace(content)) {
                /* Whitespace may be elided where it may be added (5.1.4). */
                node_index += 1;
                continue;
            }
        }
        const bool is_markup = kind == SXML_NODE_KIND_ELEMENT || kind == SXML_NODE_KIND_COMMENT ||
                               kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION;
        if (status == SXML_STATUS_OK && is_markup && parent != NULL) {
            if (parent->is_indenting) {
                status = sxml_xml_writer_indent(writer, parent->depth + 1);
            }
            parent->has_written_child = true;
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        switch (kind) {
        case SXML_NODE_KIND_ELEMENT:
            status = sxml_xml_writer_open_element(writer, current, parent);
            node_index += 1U + current_description.attribute_count;
            break;
        case SXML_NODE_KIND_TEXT:
        case SXML_NODE_KIND_COMMENT:
        case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
            status = sxml_xml_writer_write_leaf(writer, current, &current_description);
            node_index += 1;
            break;
        case SXML_NODE_KIND_DOCUMENT:
        case SXML_NODE_KIND_ATTRIBUTE:
        case SXML_NODE_KIND_NAMESPACE:
        case SXML_NODE_KIND_ANY:
            node_index += 1;
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_close_elements(
            writer, (sxml_closing){.tree = node.tree, .node_index = UINT64_MAX});
    }
    return status;
}

/* ------------------------------------------------------ Setting up */

/* Whether an html-version, an xs:decimal, is 5.0 or more. */
static bool sxml_html_version_is_html5(sxml_byte_view html_version) {
    size_t offset = 0;
    if (offset < html_version.byte_count && html_version.bytes[offset] == '+') {
        offset += 1;
    } else if (offset < html_version.byte_count && html_version.bytes[offset] == '-') {
        return false;
    }
    while (offset < html_version.byte_count && html_version.bytes[offset] == '0') {
        offset += 1;
    }
    size_t digit_count = 0;
    uint8_t first_digit = 0;
    while (offset + digit_count < html_version.byte_count &&
           html_version.bytes[offset + digit_count] >= '0' &&
           html_version.bytes[offset + digit_count] <= '9') {
        if (digit_count == 0) {
            first_digit = html_version.bytes[offset];
        }
        digit_count += 1;
    }
    return digit_count > 1 || (digit_count == 1 && first_digit >= '5');
}

bool sxml_serializer_requests_html5(const sxml_serialization_parameters *parameters) {
    if (parameters->has_html_version) {
        return sxml_html_version_is_html5(parameters->html_version);
    }
    return !parameters->has_version || sxml_html_version_is_html5(parameters->version);
}

static void sxml_xml_writer_start(sxml_serializer *serializer, sxml_xml_writer *writer) {
    const sxml_serialization_parameters *parameters = serializer->parameters;
    const bool is_xml_like = parameters->serialization_method == SXML_SERIALIZATION_METHOD_XML ||
                             parameters->serialization_method == SXML_SERIALIZATION_METHOD_XHTML;
    const bool is_xhtml = parameters->serialization_method == SXML_SERIALIZATION_METHOD_XHTML;
    const bool is_html = parameters->serialization_method == SXML_SERIALIZATION_METHOD_HTML;
    const bool has_html_rules = is_xhtml || is_html;
    const bool is_canonical = is_xml_like && parameters->canonical == SXML_SERIALIZATION_FLAG_YES;
    const bool is_version_1_1 =
        parameters->has_version &&
        sxml_byte_view_equals(parameters->version, SXML_BYTE_VIEW_FROM_LITERAL("1.1"));
    *writer = (sxml_xml_writer){
        .serializer = serializer,
        .parameters = parameters,
        .is_xhtml = is_xhtml,
        .is_html = is_html,
        .has_html_rules = has_html_rules,
        .checks_xml_constructs = is_xml_like,
        .is_html5 = (is_xhtml && (!parameters->has_html_version ||
                                  sxml_html_version_is_html5(parameters->html_version))) ||
                    (is_html && sxml_serializer_requests_html5(parameters)),
        .is_canonical = is_canonical,
        .is_indenting = (is_xml_like || is_html) && !is_canonical &&
                        parameters->indent == SXML_SERIALIZATION_FLAG_YES,
        .indents_attributes = parameters->indent_attributes == SXML_SERIALIZATION_FLAG_YES,
        .undeclares_prefixes = is_xml_like && !is_canonical && is_version_1_1 &&
                               parameters->undeclare_prefixes == SXML_SERIALIZATION_FLAG_YES,
        .includes_content_type =
            has_html_rules && parameters->include_content_type != SXML_SERIALIZATION_FLAG_NO,
        .escapes_uri_attributes = has_html_rules && !is_canonical &&
                                  parameters->escape_uri_attributes != SXML_SERIALIZATION_FLAG_NO,
        .indent_unit = parameters->has_indent_unit ? parameters->indent_unit
                                                   : SXML_BYTE_VIEW_FROM_LITERAL("  "),
        .xhtml_namespace = {.index = SXML_NAMESPACE_NONE},
        .svg_namespace = {.index = SXML_NAMESPACE_NONE},
        .mathml_namespace = {.index = SXML_NAMESPACE_NONE},
        .open_elements = NULL,
        .bindings = NULL,
        .wanted = NULL,
        .declared = NULL,
        .attribute_numbers = NULL,
    };
    if (has_html_rules) {
        (void)sxml_name_pool_find_namespace_uri(
            serializer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/1999/xhtml"),
            &writer->xhtml_namespace);
        (void)sxml_name_pool_find_namespace_uri(
            serializer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/2000/svg"),
            &writer->svg_namespace);
        (void)sxml_name_pool_find_namespace_uri(
            serializer->name_pool,
            SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/1998/Math/MathML"),
            &writer->mathml_namespace);
    }
}

/* The checks of Serialization 4.0, 5.1.2, 5.1.9 and 5.1.11 (not for
   Canonical XML, which ignores those parameters). */
static sxml_status sxml_xml_writer_check_parameters(sxml_xml_writer *writer) {
    const sxml_serialization_parameters *parameters = writer->parameters;
    const sxml_byte_view version =
        parameters->has_version ? parameters->version : SXML_BYTE_VIEW_FROM_LITERAL("1.0");
    const bool is_version_1_0 = sxml_byte_view_equals(version, SXML_BYTE_VIEW_FROM_LITERAL("1.0"));
    if (!is_version_1_0 && !sxml_byte_view_equals(version, SXML_BYTE_VIEW_FROM_LITERAL("1.1"))) {
        return sxml_xml_writer_raise(writer, SXML_XML_ERROR("SESU0013", "the XML version must be 1.0 or 1.1"));
    }
    const bool omits_declaration = parameters->omit_xml_declaration != SXML_SERIALIZATION_FLAG_NO;
    if (omits_declaration && (parameters->standalone != SXML_STANDALONE_OMIT ||
                              (!is_version_1_0 && parameters->has_doctype_system))) {
        return sxml_xml_writer_raise(writer, SXML_XML_ERROR("SEPM0009", "omit-xml-declaration is yes while standalone is given, or while doctype-system "
            "is given with an XML version other than 1.0"));
    }
    if (is_version_1_0 && parameters->undeclare_prefixes == SXML_SERIALIZATION_FLAG_YES) {
        return sxml_xml_writer_raise(writer, SXML_XML_ERROR("SEPM0010", "undeclare-prefixes needs XML version 1.1"));
    }
    return SXML_STATUS_OK;
}

/* SEPM0004 (Serialization 4.0, section 5): doctype-system or standalone
   with a result tree that has text or more than one element at the top. */
static sxml_status sxml_xml_writer_check_document_shape(sxml_xml_writer *writer,
                                                        sxml_sequence value) {
    const sxml_serialization_parameters *parameters = writer->parameters;
    if (!parameters->has_doctype_system && parameters->standalone == SXML_STANDALONE_OMIT) {
        return SXML_STATUS_OK;
    }
    sxml_sequence_normalizer normalizer;
    sxml_sequence_normalizer_start(writer->serializer, value, &normalizer);
    size_t element_count = 0;
    bool has_text = false;
    sxml_normalized_event event = {.event_kind = SXML_NORMALIZED_EVENT_KIND_TEXT};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && event.event_kind != SXML_NORMALIZED_EVENT_KIND_END) {
        status = sxml_sequence_normalizer_next(&normalizer, &event);
        if (status != SXML_STATUS_OK) {
            break;
        }
        has_text = has_text || event.event_kind == SXML_NORMALIZED_EVENT_KIND_TEXT;
        if (event.event_kind == SXML_NORMALIZED_EVENT_KIND_NODE) {
            sxml_node_description description;
            status = sxml_node_describe(event.node, &description);
            element_count += status == SXML_STATUS_OK &&
                                     description.node_kind == SXML_NODE_KIND_ELEMENT
                                 ? 1U
                                 : 0U;
        }
    }
    sxml_sequence_normalizer_finish(&normalizer);
    if (status == SXML_STATUS_OK && (has_text || element_count > 1)) {
        status = sxml_xml_writer_raise(writer, SXML_XML_ERROR("SEPM0004", "doctype-system and standalone need a result tree with one element and no text "
            "at the top"));
    }
    return status;
}

/* Writes the XML declaration (Serialization 4.0, 5.1.9). */
static sxml_status sxml_xml_writer_write_declaration_of_xml(sxml_xml_writer *writer) {
    const sxml_serialization_parameters *parameters = writer->parameters;
    sxml_status status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<?xml version=\""));
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, parameters->has_version
                                                    ? parameters->version
                                                    : SXML_BYTE_VIEW_FROM_LITERAL("1.0"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\" encoding=\""));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer,
                                        sxml_serializer_get_encoding_name(writer->serializer));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
    }
    if (status == SXML_STATUS_OK && parameters->standalone != SXML_STANDALONE_OMIT) {
        status = sxml_xml_writer_markup(writer, parameters->standalone == SXML_STANDALONE_YES
                                                    ? SXML_BYTE_VIEW_FROM_LITERAL(" standalone=\"yes\"")
                                                    : SXML_BYTE_VIEW_FROM_LITERAL(" standalone=\"no\""));
    }
    return status == SXML_STATUS_OK ? sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("?>")) : status;
}

/* Where the top level of the result tree is. */
typedef struct sxml_top_level {
    bool has_written;
    bool was_previous_text;
    bool has_nonwhitespace_text;
    size_t element_count;
} sxml_top_level;

/* Writes the document type declaration of the HTML method before the
   first element (Serialization 4.0, 7.5.8): PUBLIC and SYSTEM identifiers
   as given, or with HTML5 "<!DOCTYPE html>" before an html element. */
static sxml_status sxml_xml_writer_write_html_doctype(sxml_xml_writer *writer,
                                                     sxml_top_level *top_level,
                                                     const sxml_element_facts *facts) {
    const sxml_serialization_parameters *parameters = writer->parameters;
    const bool has_identifier = parameters->has_doctype_public || parameters->has_doctype_system;
    const bool is_html5_doctype =
        !has_identifier && writer->is_html5 && facts->is_html_element &&
        sxml_equals_ignoring_case(facts->name_parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("html")) &&
        !top_level->has_nonwhitespace_text;
    if (!has_identifier && !is_html5_doctype) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (writer->is_indenting && top_level->has_written) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<!DOCTYPE html"));
    }
    if (status == SXML_STATUS_OK && parameters->has_doctype_public) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" PUBLIC \""));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, parameters->doctype_public);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
        }
    } else if (status == SXML_STATUS_OK && parameters->has_doctype_system) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" SYSTEM"));
    }
    if (status == SXML_STATUS_OK && parameters->has_doctype_system) {
        bool has_quotation_mark = false;
        for (size_t offset = 0; offset < parameters->doctype_system.byte_count; offset += 1) {
            has_quotation_mark = has_quotation_mark || parameters->doctype_system.bytes[offset] == '"';
        }
        const sxml_byte_view quote = has_quotation_mark ? SXML_BYTE_VIEW_FROM_LITERAL("'")
                                                        : SXML_BYTE_VIEW_FROM_LITERAL("\"");
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, quote);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, parameters->doctype_system);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, quote);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(">"));
    }
    top_level->has_written = true;
    top_level->was_previous_text = false;
    return status;
}

/* Writes the document type declaration before the first element, if any
   (Serialization 4.0, 5.1.10 and, for XHTML with HTML5, 6.1.11). */
static sxml_status sxml_xml_writer_write_doctype(sxml_xml_writer *writer,
                                                 sxml_top_level *top_level,
                                                 sxml_node_reference element) {
    const sxml_serialization_parameters *parameters = writer->parameters;
    sxml_element_facts facts;
    sxml_status status = sxml_xml_writer_describe_element(writer, element, &facts);
    if (status != SXML_STATUS_OK || writer->is_canonical) {
        return status;
    }
    if (writer->is_html) {
        return sxml_xml_writer_write_html_doctype(writer, top_level, &facts);
    }
    const bool is_html5_doctype = !parameters->has_doctype_system && writer->is_xhtml &&
                                  writer->is_html5 && facts.is_html_element &&
                                  sxml_equals_ignoring_case(facts.name_parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("html")) &&
                                  !top_level->has_nonwhitespace_text;
    if (!parameters->has_doctype_system && !is_html5_doctype) {
        return SXML_STATUS_OK;
    }
    if (writer->is_indenting && top_level->has_written) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("<!DOCTYPE "));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_write_qname(
            writer,
            (sxml_written_qname){
                .namespace_prefix = {.index = facts.is_unprefixed
                                                  ? 0
                                                  : facts.description.namespace_prefix.index},
                .local_name = facts.name_parts.local_name});
    }
    if (status == SXML_STATUS_OK && parameters->has_doctype_system) {
        status = sxml_xml_writer_markup(writer, parameters->has_doctype_public ? SXML_BYTE_VIEW_FROM_LITERAL(" PUBLIC \"")
                                                                               : SXML_BYTE_VIEW_FROM_LITERAL(" SYSTEM"));
        if (status == SXML_STATUS_OK && parameters->has_doctype_public) {
            status = sxml_xml_writer_markup(writer, parameters->doctype_public);
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
            }
        }
        bool has_quotation_mark = false;
        for (size_t offset = 0; offset < parameters->doctype_system.byte_count; offset += 1) {
            has_quotation_mark = has_quotation_mark || parameters->doctype_system.bytes[offset] == '"';
        }
        const sxml_byte_view quote = has_quotation_mark ? SXML_BYTE_VIEW_FROM_LITERAL("'") : SXML_BYTE_VIEW_FROM_LITERAL("\"");
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, quote);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, parameters->doctype_system);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_markup(writer, quote);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(">"));
    }
    top_level->has_written = true;
    top_level->was_previous_text = false;
    return status;
}

/* Writes one child of the result tree. */
static sxml_status sxml_xml_writer_write_top_level(sxml_xml_writer *writer,
                                                   sxml_top_level *top_level,
                                                   const sxml_normalized_event *event) {
    sxml_status status = SXML_STATUS_OK;
    if (event->event_kind == SXML_NORMALIZED_EVENT_KIND_TEXT) {
        const bool is_whitespace = sxml_text_is_whitespace(event->text);
        top_level->has_nonwhitespace_text = top_level->has_nonwhitespace_text || !is_whitespace;
        if (writer->is_canonical) {
            /* Canonical XML 1.1, section 2.3: whitespace outside the
               document element is not written; other text makes the
               output no document. */
            return is_whitespace ? SXML_STATUS_OK
                                 : sxml_xml_writer_raise(writer, SXML_XML_ERROR("SERE0024", "Canonical XML needs a well-formed "
                                                         "document: text at the top level"));
        }
        top_level->has_written = true;
        top_level->was_previous_text = true;
        return sxml_serializer_write_character_data(
            writer->serializer, &(sxml_character_data){.text = event->text,
                                                       .character_context =
                                                           SXML_CHARACTER_CONTEXT_TEXT});
    }
    sxml_node_description description;
    status = sxml_node_describe(event->node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_element = description.node_kind == SXML_NODE_KIND_ELEMENT;
    if (is_element) {
        top_level->element_count += 1;
        if (writer->is_canonical && top_level->element_count > 1) {
            return sxml_xml_writer_raise(writer, SXML_XML_ERROR("SERE0024", "Canonical XML needs a well-formed document: more "
                                         "than one element at the top level"));
        }
        if (top_level->element_count == 1) {
            status = sxml_xml_writer_write_doctype(writer, top_level, event->node);
        }
    }
    if (status == SXML_STATUS_OK && writer->is_canonical && top_level->has_written) {
        /* Canonical XML 1.1, section 2.3: a line feed after each node
           before the document element and before each node after it. */
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    }
    if (status == SXML_STATUS_OK && writer->is_indenting && top_level->has_written &&
        !top_level->was_previous_text) {
        status = sxml_xml_writer_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    }
    if (status == SXML_STATUS_OK) {
        status = is_element ? sxml_xml_writer_write_subtree(writer, event->node)
                            : sxml_xml_writer_write_leaf(writer, event->node, &description);
    }
    top_level->has_written = true;
    top_level->was_previous_text = false;
    return status;
}

sxml_status sxml_serialize_xml_method(sxml_serializer *serializer, sxml_sequence value) {
    sxml_xml_writer writer;
    sxml_xml_writer_start(serializer, &writer);
    sxml_status status = SXML_STATUS_OK;
    if (!writer.is_canonical && !writer.is_html) {
        status = sxml_xml_writer_check_parameters(&writer);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_writer_check_document_shape(&writer, value);
        }
    }
    sxml_top_level top_level = {.has_written = false,
                                .was_previous_text = false,
                                .has_nonwhitespace_text = false,
                                .element_count = 0};
    if (status == SXML_STATUS_OK && !writer.is_canonical && !writer.is_html &&
        serializer->parameters->omit_xml_declaration == SXML_SERIALIZATION_FLAG_NO) {
        status = sxml_xml_writer_write_declaration_of_xml(&writer);
        top_level.has_written = true;
    }
    sxml_sequence_normalizer normalizer;
    sxml_sequence_normalizer_start(serializer, value, &normalizer);
    sxml_normalized_event event = {.event_kind = SXML_NORMALIZED_EVENT_KIND_TEXT};
    while (status == SXML_STATUS_OK) {
        status = sxml_sequence_normalizer_next(&normalizer, &event);
        if (status != SXML_STATUS_OK || event.event_kind == SXML_NORMALIZED_EVENT_KIND_END) {
            break;
        }
        status = sxml_xml_writer_write_top_level(&writer, &top_level, &event);
    }
    sxml_sequence_normalizer_finish(&normalizer);
    if (status == SXML_STATUS_OK && writer.is_canonical && top_level.element_count == 0) {
        status = sxml_xml_writer_raise(&writer, SXML_XML_ERROR("SERE0024", "Canonical XML needs a well-formed document: no element"));
    }
    sxml_xml_writer_finish(&writer);
    return status;
}

/* ------------------------------------------------------ Adaptive nodes */

sxml_status sxml_serializer_write_adaptive_node(sxml_serializer *serializer,
                                                sxml_node_reference node) {
    sxml_xml_writer writer;
    sxml_xml_writer_start(serializer, &writer);
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        sxml_xml_writer_finish(&writer);
        return status;
    }
    switch (description.node_kind) {
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ELEMENT:
        status = sxml_xml_writer_write_subtree(&writer, node);
        break;
    case SXML_NODE_KIND_ATTRIBUTE:
        status = sxml_xml_writer_write_attribute(&writer, NULL, node);
        break;
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        status = sxml_xml_writer_write_leaf(&writer, node, &description);
        break;
    case SXML_NODE_KIND_NAMESPACE:
        if (node.namespace_number == 0) {
            /* A stored, parentless namespace node (D98): its name is the
               prefix, its content the URI. */
            sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                     .local_name = {.bytes = NULL, .byte_count = 0}};
            sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
            if (description.node_name.index != 0) {
                status = sxml_name_pool_get_name_parts(serializer->name_pool,
                                                       description.node_name, &parts);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_node_get_content(node, &namespace_uri);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_writer_markup(&writer, parts.local_name.byte_count > 0
                                                             ? SXML_BYTE_VIEW_FROM_LITERAL("xmlns:")
                                                             : SXML_BYTE_VIEW_FROM_LITERAL("xmlns"));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_writer_markup(&writer, parts.local_name);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_writer_markup(&writer, SXML_BYTE_VIEW_FROM_LITERAL("=\""));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_write_character_data(
                    serializer,
                    &(sxml_character_data){.text = namespace_uri,
                                           .character_context =
                                               SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE});
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_writer_markup(&writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
            }
        } else {
            sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                                    .namespace_binding_count = 0};
            status = sxml_node_get_in_scope_namespaces(
                (sxml_node_reference){.tree = node.tree,
                                      .node_index = node.node_index,
                                      .namespace_number = 0},
                &bindings);
            if (status == SXML_STATUS_OK &&
                node.namespace_number > bindings.namespace_binding_count) {
                status = SXML_STATUS_INDEX_OUT_OF_RANGE;
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_writer_write_declaration(
                    &writer, bindings.namespace_bindings[node.namespace_number - 1]);
            }
        }
        break;
    case SXML_NODE_KIND_ANY:
        status = SXML_STATUS_INVALID_ARGUMENT;
        break;
    }
    sxml_xml_writer_finish(&writer);
    return status;
}
