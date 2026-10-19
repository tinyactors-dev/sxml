/*
 * HTML tree construction (src/html_internal.h; HTML Living Standard,
 * 13.2.6 "Tree construction"; D192) on the HTML working tree (glossary
 * `html_working_tree`): a node table whose links tree construction
 * rewrites when the adoption agency algorithm and foster parenting move
 * nodes that are already built.
 *
 * The parser has no scripting (13.2.4.5: the scripting mode is
 * Disabled), no sanitizer, no fragment context, allows no declarative
 * shadow roots and does no content patching, so those branches of the
 * specification are not taken. A template element's contents are its
 * children here, as the XDM mapping wants them (F&O 4.0, 17.3.1).
 *
 * "Process the token using the rules for" another insertion mode is a
 * step of a dispatch loop (sxml_html_process_token), not a call: no
 * function here calls itself through another (D71).
 *
 * Golden file: src/xml.c (a parser that builds a tree from a token
 * stream, with explicit stacks).
 */
#include "html_internal.h"

#include <string.h>

/* The tag names tree construction tests (13.2.6), sorted. */
typedef enum sxml_html_tag {
    SXML_HTML_TAG_OTHER,
    SXML_HTML_TAG_A,
    SXML_HTML_TAG_ADDRESS,
    SXML_HTML_TAG_ANNOTATION_XML,
    SXML_HTML_TAG_APPLET,
    SXML_HTML_TAG_AREA,
    SXML_HTML_TAG_ARTICLE,
    SXML_HTML_TAG_ASIDE,
    SXML_HTML_TAG_B,
    SXML_HTML_TAG_BASE,
    SXML_HTML_TAG_BASEFONT,
    SXML_HTML_TAG_BGSOUND,
    SXML_HTML_TAG_BIG,
    SXML_HTML_TAG_BLOCKQUOTE,
    SXML_HTML_TAG_BODY,
    SXML_HTML_TAG_BR,
    SXML_HTML_TAG_BUTTON,
    SXML_HTML_TAG_CAPTION,
    SXML_HTML_TAG_CENTER,
    SXML_HTML_TAG_CODE,
    SXML_HTML_TAG_COL,
    SXML_HTML_TAG_COLGROUP,
    SXML_HTML_TAG_DD,
    SXML_HTML_TAG_DESC,
    SXML_HTML_TAG_DETAILS,
    SXML_HTML_TAG_DIALOG,
    SXML_HTML_TAG_DIR,
    SXML_HTML_TAG_DIV,
    SXML_HTML_TAG_DL,
    SXML_HTML_TAG_DT,
    SXML_HTML_TAG_EM,
    SXML_HTML_TAG_EMBED,
    SXML_HTML_TAG_FIELDSET,
    SXML_HTML_TAG_FIGCAPTION,
    SXML_HTML_TAG_FIGURE,
    SXML_HTML_TAG_FONT,
    SXML_HTML_TAG_FOOTER,
    SXML_HTML_TAG_FOREIGNOBJECT,
    SXML_HTML_TAG_FORM,
    SXML_HTML_TAG_FRAME,
    SXML_HTML_TAG_FRAMESET,
    SXML_HTML_TAG_H1,
    SXML_HTML_TAG_H2,
    SXML_HTML_TAG_H3,
    SXML_HTML_TAG_H4,
    SXML_HTML_TAG_H5,
    SXML_HTML_TAG_H6,
    SXML_HTML_TAG_HEAD,
    SXML_HTML_TAG_HEADER,
    SXML_HTML_TAG_HGROUP,
    SXML_HTML_TAG_HR,
    SXML_HTML_TAG_HTML,
    SXML_HTML_TAG_I,
    SXML_HTML_TAG_IFRAME,
    SXML_HTML_TAG_IMAGE,
    SXML_HTML_TAG_IMG,
    SXML_HTML_TAG_INPUT,
    SXML_HTML_TAG_KEYGEN,
    SXML_HTML_TAG_LI,
    SXML_HTML_TAG_LINK,
    SXML_HTML_TAG_LISTING,
    SXML_HTML_TAG_MAIN,
    SXML_HTML_TAG_MALIGNMARK,
    SXML_HTML_TAG_MARQUEE,
    SXML_HTML_TAG_MATH,
    SXML_HTML_TAG_MENU,
    SXML_HTML_TAG_META,
    SXML_HTML_TAG_MGLYPH,
    SXML_HTML_TAG_MI,
    SXML_HTML_TAG_MN,
    SXML_HTML_TAG_MO,
    SXML_HTML_TAG_MS,
    SXML_HTML_TAG_MTEXT,
    SXML_HTML_TAG_NAV,
    SXML_HTML_TAG_NOBR,
    SXML_HTML_TAG_NOEMBED,
    SXML_HTML_TAG_NOFRAMES,
    SXML_HTML_TAG_NOSCRIPT,
    SXML_HTML_TAG_OBJECT,
    SXML_HTML_TAG_OL,
    SXML_HTML_TAG_OPTGROUP,
    SXML_HTML_TAG_OPTION,
    SXML_HTML_TAG_P,
    SXML_HTML_TAG_PARAM,
    SXML_HTML_TAG_PLAINTEXT,
    SXML_HTML_TAG_PRE,
    SXML_HTML_TAG_RB,
    SXML_HTML_TAG_RP,
    SXML_HTML_TAG_RT,
    SXML_HTML_TAG_RTC,
    SXML_HTML_TAG_RUBY,
    SXML_HTML_TAG_S,
    SXML_HTML_TAG_SCRIPT,
    SXML_HTML_TAG_SEARCH,
    SXML_HTML_TAG_SECTION,
    SXML_HTML_TAG_SELECT,
    SXML_HTML_TAG_SMALL,
    SXML_HTML_TAG_SOURCE,
    SXML_HTML_TAG_SPAN,
    SXML_HTML_TAG_STRIKE,
    SXML_HTML_TAG_STRONG,
    SXML_HTML_TAG_STYLE,
    SXML_HTML_TAG_SUB,
    SXML_HTML_TAG_SUMMARY,
    SXML_HTML_TAG_SUP,
    SXML_HTML_TAG_SVG,
    SXML_HTML_TAG_TABLE,
    SXML_HTML_TAG_TBODY,
    SXML_HTML_TAG_TD,
    SXML_HTML_TAG_TEMPLATE,
    SXML_HTML_TAG_TEXTAREA,
    SXML_HTML_TAG_TFOOT,
    SXML_HTML_TAG_TH,
    SXML_HTML_TAG_THEAD,
    SXML_HTML_TAG_TITLE,
    SXML_HTML_TAG_TR,
    SXML_HTML_TAG_TRACK,
    SXML_HTML_TAG_TT,
    SXML_HTML_TAG_U,
    SXML_HTML_TAG_UL,
    SXML_HTML_TAG_VAR,
    SXML_HTML_TAG_WBR,
    SXML_HTML_TAG_XMP,
} sxml_html_tag;

typedef struct sxml_html_tag_entry {
    sxml_byte_view tag_name;
    sxml_html_tag tag;
} sxml_html_tag_entry;

static const sxml_html_tag_entry sxml_html_tags[] = {
    {SXML_BYTE_VIEW_INITIALIZER("a"), SXML_HTML_TAG_A},
    {SXML_BYTE_VIEW_INITIALIZER("address"), SXML_HTML_TAG_ADDRESS},
    {SXML_BYTE_VIEW_INITIALIZER("annotation-xml"), SXML_HTML_TAG_ANNOTATION_XML},
    {SXML_BYTE_VIEW_INITIALIZER("applet"), SXML_HTML_TAG_APPLET},
    {SXML_BYTE_VIEW_INITIALIZER("area"), SXML_HTML_TAG_AREA},
    {SXML_BYTE_VIEW_INITIALIZER("article"), SXML_HTML_TAG_ARTICLE},
    {SXML_BYTE_VIEW_INITIALIZER("aside"), SXML_HTML_TAG_ASIDE},
    {SXML_BYTE_VIEW_INITIALIZER("b"), SXML_HTML_TAG_B},
    {SXML_BYTE_VIEW_INITIALIZER("base"), SXML_HTML_TAG_BASE},
    {SXML_BYTE_VIEW_INITIALIZER("basefont"), SXML_HTML_TAG_BASEFONT},
    {SXML_BYTE_VIEW_INITIALIZER("bgsound"), SXML_HTML_TAG_BGSOUND},
    {SXML_BYTE_VIEW_INITIALIZER("big"), SXML_HTML_TAG_BIG},
    {SXML_BYTE_VIEW_INITIALIZER("blockquote"), SXML_HTML_TAG_BLOCKQUOTE},
    {SXML_BYTE_VIEW_INITIALIZER("body"), SXML_HTML_TAG_BODY},
    {SXML_BYTE_VIEW_INITIALIZER("br"), SXML_HTML_TAG_BR},
    {SXML_BYTE_VIEW_INITIALIZER("button"), SXML_HTML_TAG_BUTTON},
    {SXML_BYTE_VIEW_INITIALIZER("caption"), SXML_HTML_TAG_CAPTION},
    {SXML_BYTE_VIEW_INITIALIZER("center"), SXML_HTML_TAG_CENTER},
    {SXML_BYTE_VIEW_INITIALIZER("code"), SXML_HTML_TAG_CODE},
    {SXML_BYTE_VIEW_INITIALIZER("col"), SXML_HTML_TAG_COL},
    {SXML_BYTE_VIEW_INITIALIZER("colgroup"), SXML_HTML_TAG_COLGROUP},
    {SXML_BYTE_VIEW_INITIALIZER("dd"), SXML_HTML_TAG_DD},
    {SXML_BYTE_VIEW_INITIALIZER("desc"), SXML_HTML_TAG_DESC},
    {SXML_BYTE_VIEW_INITIALIZER("details"), SXML_HTML_TAG_DETAILS},
    {SXML_BYTE_VIEW_INITIALIZER("dialog"), SXML_HTML_TAG_DIALOG},
    {SXML_BYTE_VIEW_INITIALIZER("dir"), SXML_HTML_TAG_DIR},
    {SXML_BYTE_VIEW_INITIALIZER("div"), SXML_HTML_TAG_DIV},
    {SXML_BYTE_VIEW_INITIALIZER("dl"), SXML_HTML_TAG_DL},
    {SXML_BYTE_VIEW_INITIALIZER("dt"), SXML_HTML_TAG_DT},
    {SXML_BYTE_VIEW_INITIALIZER("em"), SXML_HTML_TAG_EM},
    {SXML_BYTE_VIEW_INITIALIZER("embed"), SXML_HTML_TAG_EMBED},
    {SXML_BYTE_VIEW_INITIALIZER("fieldset"), SXML_HTML_TAG_FIELDSET},
    {SXML_BYTE_VIEW_INITIALIZER("figcaption"), SXML_HTML_TAG_FIGCAPTION},
    {SXML_BYTE_VIEW_INITIALIZER("figure"), SXML_HTML_TAG_FIGURE},
    {SXML_BYTE_VIEW_INITIALIZER("font"), SXML_HTML_TAG_FONT},
    {SXML_BYTE_VIEW_INITIALIZER("footer"), SXML_HTML_TAG_FOOTER},
    {SXML_BYTE_VIEW_INITIALIZER("foreignobject"), SXML_HTML_TAG_FOREIGNOBJECT},
    {SXML_BYTE_VIEW_INITIALIZER("form"), SXML_HTML_TAG_FORM},
    {SXML_BYTE_VIEW_INITIALIZER("frame"), SXML_HTML_TAG_FRAME},
    {SXML_BYTE_VIEW_INITIALIZER("frameset"), SXML_HTML_TAG_FRAMESET},
    {SXML_BYTE_VIEW_INITIALIZER("h1"), SXML_HTML_TAG_H1},
    {SXML_BYTE_VIEW_INITIALIZER("h2"), SXML_HTML_TAG_H2},
    {SXML_BYTE_VIEW_INITIALIZER("h3"), SXML_HTML_TAG_H3},
    {SXML_BYTE_VIEW_INITIALIZER("h4"), SXML_HTML_TAG_H4},
    {SXML_BYTE_VIEW_INITIALIZER("h5"), SXML_HTML_TAG_H5},
    {SXML_BYTE_VIEW_INITIALIZER("h6"), SXML_HTML_TAG_H6},
    {SXML_BYTE_VIEW_INITIALIZER("head"), SXML_HTML_TAG_HEAD},
    {SXML_BYTE_VIEW_INITIALIZER("header"), SXML_HTML_TAG_HEADER},
    {SXML_BYTE_VIEW_INITIALIZER("hgroup"), SXML_HTML_TAG_HGROUP},
    {SXML_BYTE_VIEW_INITIALIZER("hr"), SXML_HTML_TAG_HR},
    {SXML_BYTE_VIEW_INITIALIZER("html"), SXML_HTML_TAG_HTML},
    {SXML_BYTE_VIEW_INITIALIZER("i"), SXML_HTML_TAG_I},
    {SXML_BYTE_VIEW_INITIALIZER("iframe"), SXML_HTML_TAG_IFRAME},
    {SXML_BYTE_VIEW_INITIALIZER("image"), SXML_HTML_TAG_IMAGE},
    {SXML_BYTE_VIEW_INITIALIZER("img"), SXML_HTML_TAG_IMG},
    {SXML_BYTE_VIEW_INITIALIZER("input"), SXML_HTML_TAG_INPUT},
    {SXML_BYTE_VIEW_INITIALIZER("keygen"), SXML_HTML_TAG_KEYGEN},
    {SXML_BYTE_VIEW_INITIALIZER("li"), SXML_HTML_TAG_LI},
    {SXML_BYTE_VIEW_INITIALIZER("link"), SXML_HTML_TAG_LINK},
    {SXML_BYTE_VIEW_INITIALIZER("listing"), SXML_HTML_TAG_LISTING},
    {SXML_BYTE_VIEW_INITIALIZER("main"), SXML_HTML_TAG_MAIN},
    {SXML_BYTE_VIEW_INITIALIZER("malignmark"), SXML_HTML_TAG_MALIGNMARK},
    {SXML_BYTE_VIEW_INITIALIZER("marquee"), SXML_HTML_TAG_MARQUEE},
    {SXML_BYTE_VIEW_INITIALIZER("math"), SXML_HTML_TAG_MATH},
    {SXML_BYTE_VIEW_INITIALIZER("menu"), SXML_HTML_TAG_MENU},
    {SXML_BYTE_VIEW_INITIALIZER("meta"), SXML_HTML_TAG_META},
    {SXML_BYTE_VIEW_INITIALIZER("mglyph"), SXML_HTML_TAG_MGLYPH},
    {SXML_BYTE_VIEW_INITIALIZER("mi"), SXML_HTML_TAG_MI},
    {SXML_BYTE_VIEW_INITIALIZER("mn"), SXML_HTML_TAG_MN},
    {SXML_BYTE_VIEW_INITIALIZER("mo"), SXML_HTML_TAG_MO},
    {SXML_BYTE_VIEW_INITIALIZER("ms"), SXML_HTML_TAG_MS},
    {SXML_BYTE_VIEW_INITIALIZER("mtext"), SXML_HTML_TAG_MTEXT},
    {SXML_BYTE_VIEW_INITIALIZER("nav"), SXML_HTML_TAG_NAV},
    {SXML_BYTE_VIEW_INITIALIZER("nobr"), SXML_HTML_TAG_NOBR},
    {SXML_BYTE_VIEW_INITIALIZER("noembed"), SXML_HTML_TAG_NOEMBED},
    {SXML_BYTE_VIEW_INITIALIZER("noframes"), SXML_HTML_TAG_NOFRAMES},
    {SXML_BYTE_VIEW_INITIALIZER("noscript"), SXML_HTML_TAG_NOSCRIPT},
    {SXML_BYTE_VIEW_INITIALIZER("object"), SXML_HTML_TAG_OBJECT},
    {SXML_BYTE_VIEW_INITIALIZER("ol"), SXML_HTML_TAG_OL},
    {SXML_BYTE_VIEW_INITIALIZER("optgroup"), SXML_HTML_TAG_OPTGROUP},
    {SXML_BYTE_VIEW_INITIALIZER("option"), SXML_HTML_TAG_OPTION},
    {SXML_BYTE_VIEW_INITIALIZER("p"), SXML_HTML_TAG_P},
    {SXML_BYTE_VIEW_INITIALIZER("param"), SXML_HTML_TAG_PARAM},
    {SXML_BYTE_VIEW_INITIALIZER("plaintext"), SXML_HTML_TAG_PLAINTEXT},
    {SXML_BYTE_VIEW_INITIALIZER("pre"), SXML_HTML_TAG_PRE},
    {SXML_BYTE_VIEW_INITIALIZER("rb"), SXML_HTML_TAG_RB},
    {SXML_BYTE_VIEW_INITIALIZER("rp"), SXML_HTML_TAG_RP},
    {SXML_BYTE_VIEW_INITIALIZER("rt"), SXML_HTML_TAG_RT},
    {SXML_BYTE_VIEW_INITIALIZER("rtc"), SXML_HTML_TAG_RTC},
    {SXML_BYTE_VIEW_INITIALIZER("ruby"), SXML_HTML_TAG_RUBY},
    {SXML_BYTE_VIEW_INITIALIZER("s"), SXML_HTML_TAG_S},
    {SXML_BYTE_VIEW_INITIALIZER("script"), SXML_HTML_TAG_SCRIPT},
    {SXML_BYTE_VIEW_INITIALIZER("search"), SXML_HTML_TAG_SEARCH},
    {SXML_BYTE_VIEW_INITIALIZER("section"), SXML_HTML_TAG_SECTION},
    {SXML_BYTE_VIEW_INITIALIZER("select"), SXML_HTML_TAG_SELECT},
    {SXML_BYTE_VIEW_INITIALIZER("small"), SXML_HTML_TAG_SMALL},
    {SXML_BYTE_VIEW_INITIALIZER("source"), SXML_HTML_TAG_SOURCE},
    {SXML_BYTE_VIEW_INITIALIZER("span"), SXML_HTML_TAG_SPAN},
    {SXML_BYTE_VIEW_INITIALIZER("strike"), SXML_HTML_TAG_STRIKE},
    {SXML_BYTE_VIEW_INITIALIZER("strong"), SXML_HTML_TAG_STRONG},
    {SXML_BYTE_VIEW_INITIALIZER("style"), SXML_HTML_TAG_STYLE},
    {SXML_BYTE_VIEW_INITIALIZER("sub"), SXML_HTML_TAG_SUB},
    {SXML_BYTE_VIEW_INITIALIZER("summary"), SXML_HTML_TAG_SUMMARY},
    {SXML_BYTE_VIEW_INITIALIZER("sup"), SXML_HTML_TAG_SUP},
    {SXML_BYTE_VIEW_INITIALIZER("svg"), SXML_HTML_TAG_SVG},
    {SXML_BYTE_VIEW_INITIALIZER("table"), SXML_HTML_TAG_TABLE},
    {SXML_BYTE_VIEW_INITIALIZER("tbody"), SXML_HTML_TAG_TBODY},
    {SXML_BYTE_VIEW_INITIALIZER("td"), SXML_HTML_TAG_TD},
    {SXML_BYTE_VIEW_INITIALIZER("template"), SXML_HTML_TAG_TEMPLATE},
    {SXML_BYTE_VIEW_INITIALIZER("textarea"), SXML_HTML_TAG_TEXTAREA},
    {SXML_BYTE_VIEW_INITIALIZER("tfoot"), SXML_HTML_TAG_TFOOT},
    {SXML_BYTE_VIEW_INITIALIZER("th"), SXML_HTML_TAG_TH},
    {SXML_BYTE_VIEW_INITIALIZER("thead"), SXML_HTML_TAG_THEAD},
    {SXML_BYTE_VIEW_INITIALIZER("title"), SXML_HTML_TAG_TITLE},
    {SXML_BYTE_VIEW_INITIALIZER("tr"), SXML_HTML_TAG_TR},
    {SXML_BYTE_VIEW_INITIALIZER("track"), SXML_HTML_TAG_TRACK},
    {SXML_BYTE_VIEW_INITIALIZER("tt"), SXML_HTML_TAG_TT},
    {SXML_BYTE_VIEW_INITIALIZER("u"), SXML_HTML_TAG_U},
    {SXML_BYTE_VIEW_INITIALIZER("ul"), SXML_HTML_TAG_UL},
    {SXML_BYTE_VIEW_INITIALIZER("var"), SXML_HTML_TAG_VAR},
    {SXML_BYTE_VIEW_INITIALIZER("wbr"), SXML_HTML_TAG_WBR},
    {SXML_BYTE_VIEW_INITIALIZER("xmp"), SXML_HTML_TAG_XMP},
};

static sxml_html_tag find_tag(sxml_byte_view name) {
    size_t low_index = 0;
    size_t high_index = sizeof(sxml_html_tags) / sizeof(sxml_html_tags[0]);
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const sxml_byte_view entry = sxml_html_tags[middle_index].tag_name;
        const size_t common =
            entry.byte_count < name.byte_count ? entry.byte_count : name.byte_count;
        int order = common == 0 ? 0 : memcmp(entry.bytes, name.bytes, common);
        if (order == 0) {
            order = entry.byte_count < name.byte_count   ? -1
                    : entry.byte_count > name.byte_count ? 1
                                                         : 0;
        }
        if (order == 0) {
            return sxml_html_tags[middle_index].tag;
        }
        if (order < 0) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return SXML_HTML_TAG_OTHER;
}

/* A list of tags, as the specification's "one of" lists. */
typedef struct sxml_html_tag_list {
    const uint8_t *tags;
    size_t tag_count;
} sxml_html_tag_list;

#define SXML_HTML_TAG_LIST_OF(name, ...)                                                   \
    static const uint8_t name##_tags[] = {__VA_ARGS__};                       \
    static const sxml_html_tag_list name = {                                  \
        .tags = name##_tags, .tag_count = sizeof(name##_tags)}

static bool tag_is_in(uint32_t tag, const sxml_html_tag_list *list) {
    for (size_t tag_index = 0; tag_index < list->tag_count; tag_index += 1) {
        if (list->tags[tag_index] == tag) {
            return true;
        }
    }
    return false;
}

/* 13.2.6.3: the elements "generate implied end tags" closes. */
SXML_HTML_TAG_LIST_OF(implied_end_tags, SXML_HTML_TAG_DD, SXML_HTML_TAG_DT, SXML_HTML_TAG_LI, SXML_HTML_TAG_OPTGROUP, SXML_HTML_TAG_OPTION, SXML_HTML_TAG_P, SXML_HTML_TAG_RB,
         SXML_HTML_TAG_RP, SXML_HTML_TAG_RT, SXML_HTML_TAG_RTC);
/* ... and "thoroughly". */
SXML_HTML_TAG_LIST_OF(thorough_implied_end_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_DD, SXML_HTML_TAG_DT, SXML_HTML_TAG_LI,
         SXML_HTML_TAG_OPTGROUP, SXML_HTML_TAG_OPTION, SXML_HTML_TAG_P, SXML_HTML_TAG_RB, SXML_HTML_TAG_RP, SXML_HTML_TAG_RT, SXML_HTML_TAG_RTC, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TD,
         SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
/* 13.2.4.2: the HTML elements of the special category. */
SXML_HTML_TAG_LIST_OF(special_html_tags, SXML_HTML_TAG_ADDRESS, SXML_HTML_TAG_APPLET, SXML_HTML_TAG_AREA, SXML_HTML_TAG_ARTICLE, SXML_HTML_TAG_ASIDE, SXML_HTML_TAG_BASE,
         SXML_HTML_TAG_BASEFONT, SXML_HTML_TAG_BGSOUND, SXML_HTML_TAG_BLOCKQUOTE, SXML_HTML_TAG_BODY, SXML_HTML_TAG_BR, SXML_HTML_TAG_BUTTON, SXML_HTML_TAG_CAPTION,
         SXML_HTML_TAG_CENTER, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_DD, SXML_HTML_TAG_DETAILS, SXML_HTML_TAG_DIR, SXML_HTML_TAG_DIV, SXML_HTML_TAG_DL, SXML_HTML_TAG_DT,
         SXML_HTML_TAG_EMBED, SXML_HTML_TAG_FIELDSET, SXML_HTML_TAG_FIGCAPTION, SXML_HTML_TAG_FIGURE, SXML_HTML_TAG_FOOTER, SXML_HTML_TAG_FORM, SXML_HTML_TAG_FRAME,
         SXML_HTML_TAG_FRAMESET, SXML_HTML_TAG_H1, SXML_HTML_TAG_H2, SXML_HTML_TAG_H3, SXML_HTML_TAG_H4, SXML_HTML_TAG_H5, SXML_HTML_TAG_H6, SXML_HTML_TAG_HEAD, SXML_HTML_TAG_HEADER,
         SXML_HTML_TAG_HGROUP, SXML_HTML_TAG_HR, SXML_HTML_TAG_HTML, SXML_HTML_TAG_IFRAME, SXML_HTML_TAG_IMG, SXML_HTML_TAG_INPUT, SXML_HTML_TAG_KEYGEN, SXML_HTML_TAG_LI,
         SXML_HTML_TAG_LINK, SXML_HTML_TAG_LISTING, SXML_HTML_TAG_MAIN, SXML_HTML_TAG_MARQUEE, SXML_HTML_TAG_MENU, SXML_HTML_TAG_META, SXML_HTML_TAG_NAV, SXML_HTML_TAG_NOEMBED,
         SXML_HTML_TAG_NOFRAMES, SXML_HTML_TAG_NOSCRIPT, SXML_HTML_TAG_OBJECT, SXML_HTML_TAG_OL, SXML_HTML_TAG_P, SXML_HTML_TAG_PARAM, SXML_HTML_TAG_PLAINTEXT, SXML_HTML_TAG_PRE,
         SXML_HTML_TAG_SCRIPT, SXML_HTML_TAG_SEARCH, SXML_HTML_TAG_SECTION, SXML_HTML_TAG_SELECT, SXML_HTML_TAG_SOURCE, SXML_HTML_TAG_STYLE, SXML_HTML_TAG_SUMMARY,
         SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TD, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_TEXTAREA, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD,
         SXML_HTML_TAG_TITLE, SXML_HTML_TAG_TR, SXML_HTML_TAG_TRACK, SXML_HTML_TAG_UL, SXML_HTML_TAG_WBR, SXML_HTML_TAG_XMP);
/* 13.2.4.2: the boundaries of "in scope", by namespace. */
SXML_HTML_TAG_LIST_OF(scope_html_tags, SXML_HTML_TAG_APPLET, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_HTML, SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TD, SXML_HTML_TAG_TH,
         SXML_HTML_TAG_MARQUEE, SXML_HTML_TAG_OBJECT, SXML_HTML_TAG_SELECT, SXML_HTML_TAG_TEMPLATE);
SXML_HTML_TAG_LIST_OF(mathml_text_integration_tags, SXML_HTML_TAG_MI, SXML_HTML_TAG_MO, SXML_HTML_TAG_MN, SXML_HTML_TAG_MS, SXML_HTML_TAG_MTEXT);
SXML_HTML_TAG_LIST_OF(scope_mathml_tags, SXML_HTML_TAG_MI, SXML_HTML_TAG_MO, SXML_HTML_TAG_MN, SXML_HTML_TAG_MS, SXML_HTML_TAG_MTEXT, SXML_HTML_TAG_ANNOTATION_XML);
SXML_HTML_TAG_LIST_OF(scope_svg_tags, SXML_HTML_TAG_FOREIGNOBJECT, SXML_HTML_TAG_DESC, SXML_HTML_TAG_TITLE);
/* 13.2.4.3: the formatting elements. */
SXML_HTML_TAG_LIST_OF(formatting_tags, SXML_HTML_TAG_A, SXML_HTML_TAG_B, SXML_HTML_TAG_BIG, SXML_HTML_TAG_CODE, SXML_HTML_TAG_EM, SXML_HTML_TAG_FONT, SXML_HTML_TAG_I, SXML_HTML_TAG_NOBR,
         SXML_HTML_TAG_S, SXML_HTML_TAG_SMALL, SXML_HTML_TAG_STRIKE, SXML_HTML_TAG_STRONG, SXML_HTML_TAG_TT, SXML_HTML_TAG_U);
SXML_HTML_TAG_LIST_OF(heading_tags, SXML_HTML_TAG_H1, SXML_HTML_TAG_H2, SXML_HTML_TAG_H3, SXML_HTML_TAG_H4, SXML_HTML_TAG_H5, SXML_HTML_TAG_H6);
/* 13.2.6.4.7, "in body": where the stack may end at end of file or
   </body> without a parse error. */
SXML_HTML_TAG_LIST_OF(closable_at_end_tags, SXML_HTML_TAG_DD, SXML_HTML_TAG_DT, SXML_HTML_TAG_LI, SXML_HTML_TAG_OPTGROUP, SXML_HTML_TAG_OPTION, SXML_HTML_TAG_P, SXML_HTML_TAG_RB,
         SXML_HTML_TAG_RP, SXML_HTML_TAG_RT, SXML_HTML_TAG_RTC, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TD, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR,
         SXML_HTML_TAG_BODY, SXML_HTML_TAG_HTML);

/* ------------------------------------------------------------ Parser */

typedef enum sxml_html_insertion_mode {
    SXML_HTML_MODE_INITIAL,
    SXML_HTML_MODE_BEFORE_HTML,
    SXML_HTML_MODE_BEFORE_HEAD,
    SXML_HTML_MODE_IN_HEAD,
    SXML_HTML_MODE_IN_HEAD_NOSCRIPT,
    SXML_HTML_MODE_AFTER_HEAD,
    SXML_HTML_MODE_IN_BODY,
    SXML_HTML_MODE_TEXT,
    SXML_HTML_MODE_IN_TABLE,
    SXML_HTML_MODE_IN_TABLE_TEXT,
    SXML_HTML_MODE_IN_CAPTION,
    SXML_HTML_MODE_IN_COLUMN_GROUP,
    SXML_HTML_MODE_IN_TABLE_BODY,
    SXML_HTML_MODE_IN_ROW,
    SXML_HTML_MODE_IN_CELL,
    SXML_HTML_MODE_IN_TEMPLATE,
    SXML_HTML_MODE_AFTER_BODY,
    SXML_HTML_MODE_IN_FRAMESET,
    SXML_HTML_MODE_AFTER_FRAMESET,
    SXML_HTML_MODE_AFTER_AFTER_BODY,
    SXML_HTML_MODE_AFTER_AFTER_FRAMESET,
    /* Not an insertion mode: the rules for foreign content (13.2.6.5). */
    SXML_HTML_MODE_FOREIGN_CONTENT,
} sxml_html_insertion_mode;

/* The document's mode (DOM, "document mode"). */
typedef enum sxml_html_quirks_mode {
    SXML_HTML_QUIRKS_MODE_NO_QUIRKS,
    SXML_HTML_QUIRKS_MODE_LIMITED_QUIRKS,
    SXML_HTML_QUIRKS_MODE_QUIRKS,
} sxml_html_quirks_mode;

/* A marker in the list of active formatting elements (13.2.4.3). */
#define SXML_HTML_FORMATTING_MARKER (UINT32_MAX - 1U)

typedef struct sxml_html_parser {
    sxml_html_tokenizer tokenizer;
    sxml_html_working_tree *tree;
    sxml_allocator allocator;
    /* uint32_t node numbers, the current node last. */
    sxml_xml_array open_elements;
    /* uint32_t node numbers or SXML_HTML_FORMATTING_MARKER. */
    sxml_xml_array formatting_elements;
    /* uint8_t insertion modes. */
    sxml_xml_array template_modes;
    sxml_html_insertion_mode mode;
    sxml_html_insertion_mode original_mode;
    uint32_t head_element;
    uint32_t form_element;
    bool is_frameset_ok;
    bool is_foster_parenting;
    sxml_html_quirks_mode quirks_mode;
    /* The pending table character tokens (13.2.6.4.10). */
    sxml_xml_buffer pending_table_text;
    bool has_pending_non_whitespace;
    /* After <pre>, <listing> and <textarea>: a first LF is dropped. */
    bool ignores_next_line_feed;
    /* "after head": the head element was pushed for one token. */
    bool removes_head_after_token;
    /* The current start tag's self-closing flag was acknowledged. */
    bool has_acknowledged_self_closing;
    bool has_parse_error;
    bool has_stopped;
    /* The encoding of the input and its confidence (13.2.3.2); a meta
       element that changes it stops parsing (13.2.3.4). */
    uint8_t encoding_number;
    bool is_encoding_tentative;
    bool changes_encoding;
    uint8_t new_encoding_number;
} sxml_html_parser;

static void report_parse_error(sxml_html_parser *parser) {
    parser->has_parse_error = true;
}

static sxml_html_node *node_at(const sxml_html_parser *parser, uint32_t node_number) {
    return sxml_html_working_tree_node(parser->tree, node_number);
}

static bool is_html_element(const sxml_html_parser *parser, uint32_t node_number, uint32_t tag) {
    const sxml_html_node *node = node_at(parser, node_number);
    return node->node_kind == SXML_HTML_NODE_KIND_ELEMENT &&
           node->element_namespace == SXML_HTML_NAMESPACE_HTML && node->tag == tag;
}

static bool is_html_element_in(const sxml_html_parser *parser, uint32_t node_number,
                               const sxml_html_tag_list *list) {
    const sxml_html_node *node = node_at(parser, node_number);
    return node->node_kind == SXML_HTML_NODE_KIND_ELEMENT &&
           node->element_namespace == SXML_HTML_NAMESPACE_HTML && tag_is_in(node->tag, list);
}

static bool is_special(const sxml_html_parser *parser, uint32_t node_number) {
    const sxml_html_node *node = node_at(parser, node_number);
    switch (node->element_namespace) {
    case SXML_HTML_NAMESPACE_HTML:
        return tag_is_in(node->tag, &special_html_tags);
    case SXML_HTML_NAMESPACE_MATHML:
        return tag_is_in(node->tag, &scope_mathml_tags);
    case SXML_HTML_NAMESPACE_SVG:
        return tag_is_in(node->tag, &scope_svg_tags);
    }
    return false;
}

static bool is_mathml_text_integration_point(const sxml_html_parser *parser,
                                             uint32_t node_number) {
    const sxml_html_node *node = node_at(parser, node_number);
    return node->element_namespace == SXML_HTML_NAMESPACE_MATHML &&
           tag_is_in(node->tag, &mathml_text_integration_tags);
}

static bool is_html_integration_point(const sxml_html_parser *parser, uint32_t node_number) {
    const sxml_html_node *node = node_at(parser, node_number);
    return (node->element_namespace == SXML_HTML_NAMESPACE_MATHML &&
            node->is_html_integration_point) ||
           (node->element_namespace == SXML_HTML_NAMESPACE_SVG &&
            tag_is_in(node->tag, &scope_svg_tags));
}

/* -------------------------------------------------------- Working tree */

sxml_html_node *sxml_html_working_tree_node(const sxml_html_working_tree *tree,
                                            uint32_t node_number) {
    return sxml_xml_array_at(&tree->nodes, node_number);
}

/* Adds a node of kind, linked nowhere, and writes its number. */
static sxml_status make_node(sxml_html_working_tree *tree, sxml_html_node_kind node_kind,
                             uint32_t *node_number) {
    if (tree->nodes.element_count >= (size_t)SXML_HTML_FORMATTING_MARKER) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&tree->nodes, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_html_node *node = element;
    node->node_kind = node_kind;
    node->element_namespace = SXML_HTML_NAMESPACE_HTML;
    node->tag = SXML_HTML_TAG_OTHER;
    node->parent = SXML_HTML_NO_NODE;
    node->first_child = SXML_HTML_NO_NODE;
    node->last_child = SXML_HTML_NO_NODE;
    node->previous_sibling = SXML_HTML_NO_NODE;
    node->next_sibling = SXML_HTML_NO_NODE;
    node->first_segment = SXML_HTML_NO_NODE;
    node->last_segment = SXML_HTML_NO_NODE;
    *node_number = (uint32_t)(tree->nodes.element_count - 1);
    return SXML_STATUS_OK;
}

sxml_status sxml_html_working_tree_initialize(sxml_html_working_tree *tree,
                                              sxml_allocator allocator) {
    tree->nodes = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = sizeof(sxml_html_node)});
    tree->attributes = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = sizeof(sxml_html_attribute)});
    tree->segments = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = sizeof(sxml_html_text_segment)});
    tree->text = sxml_xml_buffer_empty(allocator);
    uint32_t document = 0;
    return make_node(tree, SXML_HTML_NODE_KIND_DOCUMENT, &document);
}

void sxml_html_working_tree_finalize(sxml_html_working_tree *tree) {
    if (tree == NULL) {
        return;
    }
    sxml_xml_array_release(&tree->nodes);
    sxml_xml_array_release(&tree->attributes);
    sxml_xml_array_release(&tree->segments);
    sxml_xml_buffer_release(&tree->text);
}

/* Copies bytes into the tree's text and writes their range. */
static sxml_status store_text(sxml_html_working_tree *tree, sxml_byte_view bytes,
                              sxml_byte_range *range) {
    *range = (sxml_byte_range){.byte_offset = tree->text.byte_count, .byte_count = bytes.byte_count};
    return sxml_xml_buffer_append(&tree->text, bytes);
}

/* Where to insert a node: "before" may be SXML_HTML_NO_NODE (append). */
typedef struct sxml_html_location {
    uint32_t parent;
    uint32_t before;
} sxml_html_location;

/* Takes node out of its parent's children. */
static void detach_node(sxml_html_working_tree *tree, uint32_t node_number) {
    sxml_html_node *node = sxml_html_working_tree_node(tree, node_number);
    if (node->parent == SXML_HTML_NO_NODE) {
        return;
    }
    sxml_html_node *parent = sxml_html_working_tree_node(tree, node->parent);
    if (node->previous_sibling != SXML_HTML_NO_NODE) {
        sxml_html_working_tree_node(tree, node->previous_sibling)->next_sibling =
            node->next_sibling;
    } else {
        parent->first_child = node->next_sibling;
    }
    if (node->next_sibling != SXML_HTML_NO_NODE) {
        sxml_html_working_tree_node(tree, node->next_sibling)->previous_sibling =
            node->previous_sibling;
    } else {
        parent->last_child = node->previous_sibling;
    }
    node->parent = SXML_HTML_NO_NODE;
    node->previous_sibling = SXML_HTML_NO_NODE;
    node->next_sibling = SXML_HTML_NO_NODE;
}

/* Inserts node, which has no parent, at location (DOM "insert"). */
static void insert_node_at(sxml_html_working_tree *tree, uint32_t node_number,
                           sxml_html_location location) {
    detach_node(tree, node_number);
    sxml_html_node *node = sxml_html_working_tree_node(tree, node_number);
    sxml_html_node *parent = sxml_html_working_tree_node(tree, location.parent);
    node->parent = location.parent;
    if (location.before == SXML_HTML_NO_NODE) {
        node->previous_sibling = parent->last_child;
        node->next_sibling = SXML_HTML_NO_NODE;
        if (parent->last_child != SXML_HTML_NO_NODE) {
            sxml_html_working_tree_node(tree, parent->last_child)->next_sibling = node_number;
        } else {
            parent->first_child = node_number;
        }
        parent->last_child = node_number;
        return;
    }
    sxml_html_node *before = sxml_html_working_tree_node(tree, location.before);
    node->next_sibling = location.before;
    node->previous_sibling = before->previous_sibling;
    if (before->previous_sibling != SXML_HTML_NO_NODE) {
        sxml_html_working_tree_node(tree, before->previous_sibling)->next_sibling = node_number;
    } else {
        parent->first_child = node_number;
    }
    before->previous_sibling = node_number;
}

/* --------------------------------------------- Stack of open elements */

static size_t open_count(const sxml_html_parser *parser) {
    return parser->open_elements.element_count;
}

static uint32_t open_element_at(const sxml_html_parser *parser, size_t position) {
    return *(const uint32_t *)sxml_xml_array_at(&parser->open_elements, position);
}

static void set_open_element_at(sxml_html_parser *parser, size_t position, uint32_t node_number) {
    *(uint32_t *)sxml_xml_array_at(&parser->open_elements, position) = node_number;
}

/* The current node; SXML_HTML_NO_NODE when the stack is empty. */
static uint32_t current_node(const sxml_html_parser *parser) {
    return open_count(parser) == 0 ? SXML_HTML_NO_NODE
                                   : open_element_at(parser, open_count(parser) - 1);
}

static sxml_status push_open_element(sxml_html_parser *parser, uint32_t node_number) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&parser->open_elements, &element);
    if (status == SXML_STATUS_OK) {
        *(uint32_t *)element = node_number;
        node_at(parser, node_number)->is_open = true;
    }
    return status;
}

static void pop_open_element(sxml_html_parser *parser) {
    if (open_count(parser) == 0) {
        return;
    }
    node_at(parser, current_node(parser))->is_open = false;
    parser->open_elements.element_count -= 1;
}

/* The position of node in the stack, or SIZE_MAX. */
static size_t open_position_of(const sxml_html_parser *parser, uint32_t node_number) {
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        if (open_element_at(parser, position - 1) == node_number) {
            return position - 1;
        }
    }
    return SIZE_MAX;
}

static void remove_open_element(sxml_html_parser *parser, uint32_t node_number) {
    const size_t position = open_position_of(parser, node_number);
    if (position == SIZE_MAX) {
        return;
    }
    node_at(parser, node_number)->is_open = false;
    for (size_t moved = position; moved + 1 < open_count(parser); moved += 1) {
        set_open_element_at(parser, moved, open_element_at(parser, moved + 1));
    }
    parser->open_elements.element_count -= 1;
}

/* Pops until an HTML element with tag has been popped. */
static void pop_until_html_tag(sxml_html_parser *parser, uint32_t tag) {
    while (open_count(parser) > 0) {
        const uint32_t node = current_node(parser);
        pop_open_element(parser);
        if (is_html_element(parser, node, tag)) {
            return;
        }
    }
}

/* Pops until node has been popped. */
static void pop_until_node(sxml_html_parser *parser, uint32_t node_number) {
    while (open_count(parser) > 0) {
        const uint32_t node = current_node(parser);
        pop_open_element(parser);
        if (node == node_number) {
            return;
        }
    }
}

/* The kinds of scope (13.2.4.2). */
typedef enum sxml_html_scope {
    SXML_HTML_SCOPE_DEFAULT,
    SXML_HTML_SCOPE_LIST_ITEM,
    SXML_HTML_SCOPE_BUTTON,
    SXML_HTML_SCOPE_TABLE,
} sxml_html_scope;

static bool is_scope_boundary(const sxml_html_parser *parser, uint32_t node_number,
                              sxml_html_scope scope) {
    const sxml_html_node *node = node_at(parser, node_number);
    if (scope == SXML_HTML_SCOPE_TABLE) {
        return node->element_namespace == SXML_HTML_NAMESPACE_HTML &&
               (node->tag == SXML_HTML_TAG_HTML || node->tag == SXML_HTML_TAG_TABLE || node->tag == SXML_HTML_TAG_TEMPLATE);
    }
    switch (node->element_namespace) {
    case SXML_HTML_NAMESPACE_HTML:
        return tag_is_in(node->tag, &scope_html_tags) ||
               (scope == SXML_HTML_SCOPE_LIST_ITEM && (node->tag == SXML_HTML_TAG_OL || node->tag == SXML_HTML_TAG_UL)) ||
               (scope == SXML_HTML_SCOPE_BUTTON && node->tag == SXML_HTML_TAG_BUTTON);
    case SXML_HTML_NAMESPACE_MATHML:
        return tag_is_in(node->tag, &scope_mathml_tags);
    case SXML_HTML_NAMESPACE_SVG:
        return tag_is_in(node->tag, &scope_svg_tags);
    }
    return false;
}

/* What "has ... in scope" looks for: an HTML element with a tag, one of
   a list, or one node. */
typedef struct sxml_html_scope_target {
    uint32_t tag;
    const sxml_html_tag_list *tags;
    uint32_t node;
} sxml_html_scope_target;

static bool is_scope_target(const sxml_html_parser *parser, uint32_t node_number,
                            const sxml_html_scope_target *target) {
    if (target->node != SXML_HTML_NO_NODE) {
        return node_number == target->node;
    }
    if (target->tags != NULL) {
        return is_html_element_in(parser, node_number, target->tags);
    }
    return is_html_element(parser, node_number, target->tag);
}

typedef struct sxml_html_scope_query {
    sxml_html_scope_target target;
    sxml_html_scope scope;
} sxml_html_scope_query;

static bool has_in_scope(const sxml_html_parser *parser, const sxml_html_scope_query *query) {
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        const uint32_t node = open_element_at(parser, position - 1);
        if (is_scope_target(parser, node, &query->target)) {
            return true;
        }
        if (is_scope_boundary(parser, node, query->scope)) {
            return false;
        }
    }
    return false;
}

static bool has_tag_in_scope(const sxml_html_parser *parser, uint32_t tag) {
    return has_in_scope(parser, &(sxml_html_scope_query){
                                    .target = {.tag = tag, .tags = NULL, .node = SXML_HTML_NO_NODE},
                                    .scope = SXML_HTML_SCOPE_DEFAULT});
}

static bool has_tag_in_specific_scope(const sxml_html_parser *parser, uint32_t tag,
                                      sxml_html_scope scope) {
    return has_in_scope(parser, &(sxml_html_scope_query){
                                    .target = {.tag = tag, .tags = NULL, .node = SXML_HTML_NO_NODE},
                                    .scope = scope});
}

static bool has_node_in_scope(const sxml_html_parser *parser, uint32_t node_number) {
    return has_in_scope(parser,
                        &(sxml_html_scope_query){
                            .target = {.tag = SXML_HTML_TAG_OTHER, .tags = NULL, .node = node_number},
                            .scope = SXML_HTML_SCOPE_DEFAULT});
}

static bool has_template_on_stack(const sxml_html_parser *parser) {
    for (size_t position = 0; position < open_count(parser); position += 1) {
        if (is_html_element(parser, open_element_at(parser, position), SXML_HTML_TAG_TEMPLATE)) {
            return true;
        }
    }
    return false;
}

/* 13.2.6.3: "generate implied end tags", except an HTML element with
   except_tag (SXML_HTML_TAG_OTHER for none). */
static void generate_implied_end_tags(sxml_html_parser *parser, uint32_t except_tag) {
    while (open_count(parser) > 0) {
        const uint32_t node = current_node(parser);
        if (!is_html_element_in(parser, node, &implied_end_tags) ||
            (except_tag != SXML_HTML_TAG_OTHER && is_html_element(parser, node, except_tag))) {
            return;
        }
        pop_open_element(parser);
    }
}

static void generate_all_implied_end_tags_thoroughly(sxml_html_parser *parser) {
    while (open_count(parser) > 0 &&
           is_html_element_in(parser, current_node(parser), &thorough_implied_end_tags)) {
        pop_open_element(parser);
    }
}

/* ------------------------------------ List of active formatting elements */

static size_t formatting_count(const sxml_html_parser *parser) {
    return parser->formatting_elements.element_count;
}

static uint32_t formatting_at(const sxml_html_parser *parser, size_t position) {
    return *(const uint32_t *)sxml_xml_array_at(&parser->formatting_elements, position);
}

static void set_formatting_at(sxml_html_parser *parser, size_t position, uint32_t entry) {
    *(uint32_t *)sxml_xml_array_at(&parser->formatting_elements, position) = entry;
}

static sxml_status append_formatting_entry(sxml_html_parser *parser, uint32_t entry) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&parser->formatting_elements, &element);
    if (status == SXML_STATUS_OK) {
        *(uint32_t *)element = entry;
    }
    return status;
}

static void remove_formatting_at(sxml_html_parser *parser, size_t position) {
    for (size_t moved = position; moved + 1 < formatting_count(parser); moved += 1) {
        set_formatting_at(parser, moved, formatting_at(parser, moved + 1));
    }
    parser->formatting_elements.element_count -= 1;
}

static sxml_status insert_formatting_at(sxml_html_parser *parser, size_t position,
                                        uint32_t entry) {
    const sxml_status status = append_formatting_entry(parser, entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t moved = formatting_count(parser) - 1; moved > position; moved -= 1) {
        set_formatting_at(parser, moved, formatting_at(parser, moved - 1));
    }
    set_formatting_at(parser, position, entry);
    return SXML_STATUS_OK;
}

static size_t formatting_position_of(const sxml_html_parser *parser, uint32_t node_number) {
    for (size_t position = formatting_count(parser); position > 0; position -= 1) {
        if (formatting_at(parser, position - 1) == node_number) {
            return position - 1;
        }
    }
    return SIZE_MAX;
}

static void remove_formatting_element(sxml_html_parser *parser, uint32_t node_number) {
    const size_t position = formatting_position_of(parser, node_number);
    if (position != SIZE_MAX) {
        remove_formatting_at(parser, position);
    }
}

/* The attribute view of an element, in the tree. */
static const sxml_html_attribute *element_attributes(const sxml_html_parser *parser,
                                                     const sxml_html_node *node) {
    return node->attribute_count == 0
               ? NULL
               : sxml_xml_array_at(&parser->tree->attributes, node->first_attribute);
}

static sxml_byte_view tree_text(const sxml_html_parser *parser, sxml_byte_range range) {
    return sxml_xml_buffer_view(&parser->tree->text, range);
}

/* Two elements "have the same tag name, namespace, and attributes"
   (13.2.4.3), attributes compared as on their tokens, in any order. */
static bool are_same_formatting_elements(const sxml_html_parser *parser, uint32_t first_number,
                                         uint32_t second_number) {
    const sxml_html_node *first = node_at(parser, first_number);
    const sxml_html_node *second = node_at(parser, second_number);
    if (first->element_namespace != second->element_namespace ||
        first->attribute_count != second->attribute_count ||
        !sxml_byte_view_equals(tree_text(parser, first->name), tree_text(parser, second->name))) {
        return false;
    }
    const sxml_html_attribute *first_attributes = element_attributes(parser, first);
    const sxml_html_attribute *second_attributes = element_attributes(parser, second);
    for (uint32_t first_index = 0; first_index < first->attribute_count; first_index += 1) {
        bool is_matched = false;
        for (uint32_t second_index = 0; second_index < second->attribute_count && !is_matched;
             second_index += 1) {
            is_matched =
                first_attributes[first_index].attribute_namespace ==
                    second_attributes[second_index].attribute_namespace &&
                sxml_byte_view_equals(
                    tree_text(parser, first_attributes[first_index].attribute_name),
                    tree_text(parser, second_attributes[second_index].attribute_name)) &&
                sxml_byte_view_equals(
                    tree_text(parser, first_attributes[first_index].attribute_value),
                    tree_text(parser, second_attributes[second_index].attribute_value));
        }
        if (!is_matched) {
            return false;
        }
    }
    return true;
}

/* 13.2.4.3: "push onto the list of active formatting elements", with the
   Noah's Ark clause. */
static sxml_status push_formatting_element(sxml_html_parser *parser, uint32_t node_number) {
    size_t same_count = 0;
    size_t earliest = SIZE_MAX;
    for (size_t position = formatting_count(parser); position > 0; position -= 1) {
        const uint32_t entry = formatting_at(parser, position - 1);
        if (entry == SXML_HTML_FORMATTING_MARKER) {
            break;
        }
        if (are_same_formatting_elements(parser, entry, node_number)) {
            same_count += 1;
            earliest = position - 1;
        }
    }
    if (same_count >= 3) {
        remove_formatting_at(parser, earliest);
    }
    return append_formatting_entry(parser, node_number);
}

static void clear_formatting_to_last_marker(sxml_html_parser *parser) {
    while (formatting_count(parser) > 0) {
        const uint32_t entry = formatting_at(parser, formatting_count(parser) - 1);
        parser->formatting_elements.element_count -= 1;
        if (entry == SXML_HTML_FORMATTING_MARKER) {
            return;
        }
    }
}

/* The last element with tag in the list after the last marker, or
   SIZE_MAX. */
static size_t find_formatting_after_marker(const sxml_html_parser *parser, uint32_t tag) {
    for (size_t position = formatting_count(parser); position > 0; position -= 1) {
        const uint32_t entry = formatting_at(parser, position - 1);
        if (entry == SXML_HTML_FORMATTING_MARKER) {
            return SIZE_MAX;
        }
        if (is_html_element(parser, entry, tag)) {
            return position - 1;
        }
    }
    return SIZE_MAX;
}

/* ------------------------------------------------- Creating elements */

/* 13.2.6.5: SVG element names in their case. */
typedef struct sxml_html_name_adjustment {
    sxml_byte_view lower_name;
    sxml_byte_view adjusted_name;
} sxml_html_name_adjustment;

#define NAME_ADJUSTMENT(lower, adjusted)                                      \
    {SXML_BYTE_VIEW_INITIALIZER(lower), SXML_BYTE_VIEW_INITIALIZER(adjusted)}

static const sxml_html_name_adjustment svg_element_names[] = {
    NAME_ADJUSTMENT("altglyph", "altGlyph"),
    NAME_ADJUSTMENT("altglyphdef", "altGlyphDef"),
    NAME_ADJUSTMENT("altglyphitem", "altGlyphItem"),
    NAME_ADJUSTMENT("animatecolor", "animateColor"),
    NAME_ADJUSTMENT("animatemotion", "animateMotion"),
    NAME_ADJUSTMENT("animatetransform", "animateTransform"),
    NAME_ADJUSTMENT("clippath", "clipPath"),
    NAME_ADJUSTMENT("feblend", "feBlend"),
    NAME_ADJUSTMENT("fecolormatrix", "feColorMatrix"),
    NAME_ADJUSTMENT("fecomponenttransfer", "feComponentTransfer"),
    NAME_ADJUSTMENT("fecomposite", "feComposite"),
    NAME_ADJUSTMENT("feconvolvematrix", "feConvolveMatrix"),
    NAME_ADJUSTMENT("fediffuselighting", "feDiffuseLighting"),
    NAME_ADJUSTMENT("fedisplacementmap", "feDisplacementMap"),
    NAME_ADJUSTMENT("fedistantlight", "feDistantLight"),
    NAME_ADJUSTMENT("fedropshadow", "feDropShadow"),
    NAME_ADJUSTMENT("feflood", "feFlood"),
    NAME_ADJUSTMENT("fefunca", "feFuncA"),
    NAME_ADJUSTMENT("fefuncb", "feFuncB"),
    NAME_ADJUSTMENT("fefuncg", "feFuncG"),
    NAME_ADJUSTMENT("fefuncr", "feFuncR"),
    NAME_ADJUSTMENT("fegaussianblur", "feGaussianBlur"),
    NAME_ADJUSTMENT("feimage", "feImage"),
    NAME_ADJUSTMENT("femerge", "feMerge"),
    NAME_ADJUSTMENT("femergenode", "feMergeNode"),
    NAME_ADJUSTMENT("femorphology", "feMorphology"),
    NAME_ADJUSTMENT("feoffset", "feOffset"),
    NAME_ADJUSTMENT("fepointlight", "fePointLight"),
    NAME_ADJUSTMENT("fespecularlighting", "feSpecularLighting"),
    NAME_ADJUSTMENT("fespotlight", "feSpotLight"),
    NAME_ADJUSTMENT("fetile", "feTile"),
    NAME_ADJUSTMENT("feturbulence", "feTurbulence"),
    NAME_ADJUSTMENT("foreignobject", "foreignObject"),
    NAME_ADJUSTMENT("glyphref", "glyphRef"),
    NAME_ADJUSTMENT("lineargradient", "linearGradient"),
    NAME_ADJUSTMENT("radialgradient", "radialGradient"),
    NAME_ADJUSTMENT("textpath", "textPath"),
};

/* 13.2.6.1, "adjust SVG attributes". */
static const sxml_html_name_adjustment svg_attribute_names[] = {
    NAME_ADJUSTMENT("attributename", "attributeName"),
    NAME_ADJUSTMENT("attributetype", "attributeType"),
    NAME_ADJUSTMENT("basefrequency", "baseFrequency"),
    NAME_ADJUSTMENT("baseprofile", "baseProfile"),
    NAME_ADJUSTMENT("calcmode", "calcMode"),
    NAME_ADJUSTMENT("clippathunits", "clipPathUnits"),
    NAME_ADJUSTMENT("diffuseconstant", "diffuseConstant"),
    NAME_ADJUSTMENT("edgemode", "edgeMode"),
    NAME_ADJUSTMENT("filterunits", "filterUnits"),
    NAME_ADJUSTMENT("glyphref", "glyphRef"),
    NAME_ADJUSTMENT("gradienttransform", "gradientTransform"),
    NAME_ADJUSTMENT("gradientunits", "gradientUnits"),
    NAME_ADJUSTMENT("kernelmatrix", "kernelMatrix"),
    NAME_ADJUSTMENT("kernelunitlength", "kernelUnitLength"),
    NAME_ADJUSTMENT("keypoints", "keyPoints"),
    NAME_ADJUSTMENT("keysplines", "keySplines"),
    NAME_ADJUSTMENT("keytimes", "keyTimes"),
    NAME_ADJUSTMENT("lengthadjust", "lengthAdjust"),
    NAME_ADJUSTMENT("limitingconeangle", "limitingConeAngle"),
    NAME_ADJUSTMENT("markerheight", "markerHeight"),
    NAME_ADJUSTMENT("markerunits", "markerUnits"),
    NAME_ADJUSTMENT("markerwidth", "markerWidth"),
    NAME_ADJUSTMENT("maskcontentunits", "maskContentUnits"),
    NAME_ADJUSTMENT("maskunits", "maskUnits"),
    NAME_ADJUSTMENT("numoctaves", "numOctaves"),
    NAME_ADJUSTMENT("pathlength", "pathLength"),
    NAME_ADJUSTMENT("patterncontentunits", "patternContentUnits"),
    NAME_ADJUSTMENT("patterntransform", "patternTransform"),
    NAME_ADJUSTMENT("patternunits", "patternUnits"),
    NAME_ADJUSTMENT("pointsatx", "pointsAtX"),
    NAME_ADJUSTMENT("pointsaty", "pointsAtY"),
    NAME_ADJUSTMENT("pointsatz", "pointsAtZ"),
    NAME_ADJUSTMENT("preservealpha", "preserveAlpha"),
    NAME_ADJUSTMENT("preserveaspectratio", "preserveAspectRatio"),
    NAME_ADJUSTMENT("primitiveunits", "primitiveUnits"),
    NAME_ADJUSTMENT("refx", "refX"),
    NAME_ADJUSTMENT("refy", "refY"),
    NAME_ADJUSTMENT("repeatcount", "repeatCount"),
    NAME_ADJUSTMENT("repeatdur", "repeatDur"),
    NAME_ADJUSTMENT("requiredextensions", "requiredExtensions"),
    NAME_ADJUSTMENT("requiredfeatures", "requiredFeatures"),
    NAME_ADJUSTMENT("specularconstant", "specularConstant"),
    NAME_ADJUSTMENT("specularexponent", "specularExponent"),
    NAME_ADJUSTMENT("spreadmethod", "spreadMethod"),
    NAME_ADJUSTMENT("startoffset", "startOffset"),
    NAME_ADJUSTMENT("stddeviation", "stdDeviation"),
    NAME_ADJUSTMENT("stitchtiles", "stitchTiles"),
    NAME_ADJUSTMENT("surfacescale", "surfaceScale"),
    NAME_ADJUSTMENT("systemlanguage", "systemLanguage"),
    NAME_ADJUSTMENT("tablevalues", "tableValues"),
    NAME_ADJUSTMENT("targetx", "targetX"),
    NAME_ADJUSTMENT("targety", "targetY"),
    NAME_ADJUSTMENT("textlength", "textLength"),
    NAME_ADJUSTMENT("viewbox", "viewBox"),
    NAME_ADJUSTMENT("viewtarget", "viewTarget"),
    NAME_ADJUSTMENT("xchannelselector", "xChannelSelector"),
    NAME_ADJUSTMENT("ychannelselector", "yChannelSelector"),
    NAME_ADJUSTMENT("zoomandpan", "zoomAndPan"),
};

/* A table's name for lower_name, or lower_name itself. */
typedef struct sxml_html_adjustment_table {
    const sxml_html_name_adjustment *adjustments;
    size_t adjustment_count;
} sxml_html_adjustment_table;

static sxml_byte_view adjust_name(sxml_html_adjustment_table table, sxml_byte_view lower_name) {
    for (size_t adjustment_index = 0; adjustment_index < table.adjustment_count;
         adjustment_index += 1) {
        if (sxml_byte_view_equals(table.adjustments[adjustment_index].lower_name, lower_name)) {
            return table.adjustments[adjustment_index].adjusted_name;
        }
    }
    return lower_name;
}

/* 13.2.6.1, "adjust foreign attributes": the namespaced attributes. */
typedef struct sxml_html_foreign_attribute {
    sxml_byte_view token_name;
    sxml_byte_view local_name;
    sxml_html_attribute_namespace attribute_namespace;
} sxml_html_foreign_attribute;

static const sxml_html_foreign_attribute foreign_attributes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("xlink:actuate"), SXML_BYTE_VIEW_INITIALIZER("actuate"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xlink:arcrole"), SXML_BYTE_VIEW_INITIALIZER("arcrole"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xlink:href"), SXML_BYTE_VIEW_INITIALIZER("href"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xlink:role"), SXML_BYTE_VIEW_INITIALIZER("role"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xlink:show"), SXML_BYTE_VIEW_INITIALIZER("show"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xlink:title"), SXML_BYTE_VIEW_INITIALIZER("title"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xlink:type"), SXML_BYTE_VIEW_INITIALIZER("type"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK},
    {SXML_BYTE_VIEW_INITIALIZER("xml:lang"), SXML_BYTE_VIEW_INITIALIZER("lang"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XML},
    {SXML_BYTE_VIEW_INITIALIZER("xml:space"), SXML_BYTE_VIEW_INITIALIZER("space"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XML},
    {SXML_BYTE_VIEW_INITIALIZER("xmlns"), SXML_BYTE_VIEW_INITIALIZER("xmlns"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XMLNS},
    {SXML_BYTE_VIEW_INITIALIZER("xmlns:xlink"), SXML_BYTE_VIEW_INITIALIZER("xlink"),
     SXML_HTML_ATTRIBUTE_NAMESPACE_XMLNS},
};

static bool is_ascii_case_insensitive_match(sxml_byte_view text, sxml_byte_view lower_word) {
    if (text.byte_count != lower_word.byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        uint8_t byte = text.bytes[byte_index];
        if (byte >= 'A' && byte <= 'Z') {
            byte = (uint8_t)(byte + 0x20U);
        }
        if (byte != lower_word.bytes[byte_index]) {
            return false;
        }
    }
    return true;
}

/* The value of the token's attribute named name, if it has one. */
typedef struct sxml_html_attribute_lookup {
    bool is_found;
    sxml_byte_view attribute_value;
} sxml_html_attribute_lookup;

static sxml_html_attribute_lookup find_token_attribute(const sxml_html_token *token,
                                                       sxml_byte_view name) {
    for (size_t attribute_index = 0; attribute_index < token->attributes.attribute_count;
         attribute_index += 1) {
        const sxml_html_token_attribute *attribute =
            &token->attributes.attributes[attribute_index];
        if (sxml_byte_view_equals(attribute->attribute_name, name)) {
            return (sxml_html_attribute_lookup){.is_found = true,
                                                .attribute_value = attribute->attribute_value};
        }
    }
    return (sxml_html_attribute_lookup){.is_found = false,
                                        .attribute_value = {.bytes = NULL, .byte_count = 0}};
}

/* 13.2.3.4 "change the encoding" to new_encoding_number. Re-parsing from
   memory is what the standard prefers; changing the decoder on the fly is
   optional, and not done. */
static void change_encoding(sxml_html_parser *parser, uint8_t new_encoding_number) {
    const uint8_t current = parser->encoding_number;
    parser->is_encoding_tentative = false;
    if (current == SXML_HTML_ENCODING_UTF_16BE || current == SXML_HTML_ENCODING_UTF_16LE) {
        return;
    }
    uint8_t encoding_number = new_encoding_number;
    if (encoding_number == SXML_HTML_ENCODING_UTF_16BE ||
        encoding_number == SXML_HTML_ENCODING_UTF_16LE) {
        encoding_number = SXML_HTML_ENCODING_UTF_8;
    } else if (encoding_number == SXML_HTML_ENCODING_X_USER_DEFINED) {
        encoding_number = SXML_HTML_ENCODING_WINDOWS_1252;
    }
    if (encoding_number == current) {
        return;
    }
    parser->changes_encoding = true;
    parser->new_encoding_number = encoding_number;
    parser->has_stopped = true;
}

/* 13.2.6.4.4 "in head", a start tag "meta" (also from "in body"): while
   the confidence is tentative, a charset attribute that names an
   encoding, or else an http-equiv "Content-Type" whose content declares
   one (2.5.3), changes the encoding. */
static void consider_meta_encoding(sxml_html_parser *parser, const sxml_html_token *token) {
    if (!parser->is_encoding_tentative) {
        return;
    }
    const sxml_html_attribute_lookup charset =
        find_token_attribute(token, SXML_BYTE_VIEW_FROM_LITERAL("charset"));
    sxml_html_encoding_lookup encoding = {.is_found = false, .encoding_number = 0};
    if (charset.is_found) {
        encoding = sxml_html_get_encoding(charset.attribute_value);
    }
    if (!encoding.is_found) {
        const sxml_html_attribute_lookup http_equiv =
            find_token_attribute(token, SXML_BYTE_VIEW_FROM_LITERAL("http-equiv"));
        const sxml_html_attribute_lookup content =
            find_token_attribute(token, SXML_BYTE_VIEW_FROM_LITERAL("content"));
        if (http_equiv.is_found && content.is_found &&
            is_ascii_case_insensitive_match(http_equiv.attribute_value,
                                            SXML_BYTE_VIEW_FROM_LITERAL("content-type"))) {
            encoding = sxml_html_extract_meta_encoding(content.attribute_value);
        }
    }
    if (encoding.is_found) {
        change_encoding(parser, encoding.encoding_number);
    }
}

/* An element to create for a token (13.2.6.1). */
typedef struct sxml_html_element_request {
    const sxml_html_token *token;
    sxml_html_namespace element_namespace;
} sxml_html_element_request;

/* Copies one token attribute into the tree, adjusted for the element's
   namespace. */
static sxml_status store_attribute(sxml_html_parser *parser, sxml_html_namespace element_namespace,
                                   const sxml_html_token_attribute *token_attribute) {
    sxml_byte_view name = token_attribute->attribute_name;
    sxml_html_attribute_namespace attribute_namespace = SXML_HTML_ATTRIBUTE_NAMESPACE_NONE;
    if (element_namespace == SXML_HTML_NAMESPACE_MATHML &&
        sxml_byte_view_equals(name, SXML_BYTE_VIEW_FROM_LITERAL("definitionurl"))) {
        name = SXML_BYTE_VIEW_FROM_LITERAL("definitionURL");
    } else if (element_namespace == SXML_HTML_NAMESPACE_SVG) {
        name = adjust_name((sxml_html_adjustment_table){
                               .adjustments = svg_attribute_names,
                               .adjustment_count = sizeof(svg_attribute_names) /
                                                   sizeof(svg_attribute_names[0])},
                           name);
    }
    if (element_namespace != SXML_HTML_NAMESPACE_HTML) {
        for (size_t foreign_index = 0;
             foreign_index < sizeof(foreign_attributes) / sizeof(foreign_attributes[0]);
             foreign_index += 1) {
            if (sxml_byte_view_equals(foreign_attributes[foreign_index].token_name, name)) {
                attribute_namespace = foreign_attributes[foreign_index].attribute_namespace;
                name = foreign_attributes[foreign_index].local_name;
                break;
            }
        }
    }
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&parser->tree->attributes, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_html_attribute *attribute = element;
    attribute->attribute_namespace = attribute_namespace;
    status = store_text(parser->tree, name, &attribute->attribute_name);
    if (status == SXML_STATUS_OK) {
        status = store_text(parser->tree, token_attribute->attribute_value,
                            &attribute->attribute_value);
    }
    return status;
}

/* 13.2.6.1, "create an element for a token": the element, not inserted. */
static sxml_status create_element(sxml_html_parser *parser,
                                  const sxml_html_element_request *request,
                                  uint32_t *element_number) {
    const sxml_html_token *token = request->token;
    uint32_t node_number = 0;
    sxml_status status = make_node(parser->tree, SXML_HTML_NODE_KIND_ELEMENT, &node_number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view name = token->name;
    if (request->element_namespace == SXML_HTML_NAMESPACE_SVG) {
        name = adjust_name(
            (sxml_html_adjustment_table){.adjustments = svg_element_names,
                                         .adjustment_count = sizeof(svg_element_names) /
                                                             sizeof(svg_element_names[0])},
            name);
    }
    sxml_byte_range name_range = {.byte_offset = 0, .byte_count = 0};
    status = store_text(parser->tree, name, &name_range);
    const size_t first_attribute = parser->tree->attributes.element_count;
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < token->attributes.attribute_count;
         attribute_index += 1) {
        status = store_attribute(parser, request->element_namespace,
                                 &token->attributes.attributes[attribute_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_html_node *node = node_at(parser, node_number);
    node->element_namespace = request->element_namespace;
    node->tag = find_tag(token->name);
    node->name = name_range;
    node->first_attribute = (uint32_t)first_attribute;
    node->attribute_count = (uint32_t)(parser->tree->attributes.element_count - first_attribute);
    if (request->element_namespace == SXML_HTML_NAMESPACE_MATHML &&
        node->tag == SXML_HTML_TAG_ANNOTATION_XML) {
        const sxml_html_attribute_lookup encoding =
            find_token_attribute(token, SXML_BYTE_VIEW_FROM_LITERAL("encoding"));
        node->is_html_integration_point =
            encoding.is_found &&
            (is_ascii_case_insensitive_match(encoding.attribute_value,
                                             SXML_BYTE_VIEW_FROM_LITERAL("text/html")) ||
             is_ascii_case_insensitive_match(
                 encoding.attribute_value, SXML_BYTE_VIEW_FROM_LITERAL("application/xhtml+xml")));
    }
    *element_number = node_number;
    return SXML_STATUS_OK;
}

/* A new element for the token "for which element was created" (the list
   of active formatting elements, the adoption agency algorithm): its
   name and its attributes, shared, as they never change. */
static sxml_status clone_element(sxml_html_parser *parser, uint32_t original_number,
                                 uint32_t *element_number) {
    uint32_t node_number = 0;
    const sxml_status status = make_node(parser->tree, SXML_HTML_NODE_KIND_ELEMENT, &node_number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_html_node *original = node_at(parser, original_number);
    sxml_html_node *node = node_at(parser, node_number);
    node->element_namespace = original->element_namespace;
    node->tag = original->tag;
    node->name = original->name;
    node->first_attribute = original->first_attribute;
    node->attribute_count = original->attribute_count;
    node->is_html_integration_point = original->is_html_integration_point;
    *element_number = node_number;
    return SXML_STATUS_OK;
}

/* 13.2.6.1, "the appropriate place for inserting a node", with
   override_target or SXML_HTML_NO_NODE for the current node. */
static sxml_html_location appropriate_place(const sxml_html_parser *parser,
                                            uint32_t override_target) {
    const uint32_t target =
        override_target != SXML_HTML_NO_NODE ? override_target : current_node(parser);
    SXML_HTML_TAG_LIST_OF(foster_parenting_targets, SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    if (!parser->is_foster_parenting ||
        !is_html_element_in(parser, target, &foster_parenting_targets)) {
        return (sxml_html_location){.parent = target, .before = SXML_HTML_NO_NODE};
    }
    size_t last_template = SIZE_MAX;
    size_t last_table = SIZE_MAX;
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        const uint32_t node = open_element_at(parser, position - 1);
        if (last_template == SIZE_MAX && is_html_element(parser, node, SXML_HTML_TAG_TEMPLATE)) {
            last_template = position - 1;
        }
        if (last_table == SIZE_MAX && is_html_element(parser, node, SXML_HTML_TAG_TABLE)) {
            last_table = position - 1;
        }
    }
    if (last_template != SIZE_MAX && (last_table == SIZE_MAX || last_template > last_table)) {
        return (sxml_html_location){.parent = open_element_at(parser, last_template),
                                    .before = SXML_HTML_NO_NODE};
    }
    if (last_table == SIZE_MAX) {
        return (sxml_html_location){.parent = open_element_at(parser, 0),
                                    .before = SXML_HTML_NO_NODE};
    }
    const uint32_t table = open_element_at(parser, last_table);
    const uint32_t table_parent = node_at(parser, table)->parent;
    if (table_parent != SXML_HTML_NO_NODE) {
        return (sxml_html_location){.parent = table_parent, .before = table};
    }
    return (sxml_html_location){.parent = open_element_at(parser, last_table - 1),
                                .before = SXML_HTML_NO_NODE};
}

static bool document_has_element_child(const sxml_html_parser *parser) {
    for (uint32_t child = node_at(parser, 0)->first_child; child != SXML_HTML_NO_NODE;
         child = node_at(parser, child)->next_sibling) {
        if (node_at(parser, child)->node_kind == SXML_HTML_NODE_KIND_ELEMENT) {
            return true;
        }
    }
    return false;
}

/* "Insert an element at the adjusted insertion location", then push it
   onto the stack of open elements. */
static sxml_status insert_element_at(sxml_html_parser *parser, uint32_t element,
                                     sxml_html_location location) {
    if (!(location.parent == 0 && document_has_element_child(parser))) {
        insert_node_at(parser->tree, element, location);
    }
    return push_open_element(parser, element);
}

/* "Insert a foreign element" for token in element_namespace (and "insert
   an HTML element"). */
static sxml_status insert_element_for_token(sxml_html_parser *parser,
                                            const sxml_html_element_request *request,
                                            uint32_t *element_number) {
    uint32_t element = 0;
    sxml_status status = create_element(parser, request, &element);
    if (status == SXML_STATUS_OK) {
        status = insert_element_at(parser, element, appropriate_place(parser, SXML_HTML_NO_NODE));
    }
    if (status == SXML_STATUS_OK && element_number != NULL) {
        *element_number = element;
    }
    return status;
}

static sxml_status insert_html_element(sxml_html_parser *parser, const sxml_html_token *token) {
    return insert_element_for_token(
        parser,
        &(sxml_html_element_request){.token = token,
                                     .element_namespace = SXML_HTML_NAMESPACE_HTML},
        NULL);
}

/* A start tag token named name, with no attributes. */
static sxml_html_token start_tag_named(sxml_byte_view name) {
    sxml_html_token token;
    memset(&token, 0, sizeof(token));
    token.token_kind = SXML_HTML_TOKEN_KIND_START_TAG;
    token.name = name;
    return token;
}

/* "Insert an HTML element for a name start tag token with no
   attributes". */
static sxml_status insert_implied_element(sxml_html_parser *parser, sxml_byte_view name,
                                          uint32_t *element_number) {
    const sxml_html_token token = start_tag_named(name);
    return insert_element_for_token(
        parser,
        &(sxml_html_element_request){.token = &token,
                                     .element_namespace = SXML_HTML_NAMESPACE_HTML},
        element_number);
}

/* "Insert a character": data at the adjusted insertion location. */
static sxml_status insert_characters(sxml_html_parser *parser, sxml_byte_view data) {
    if (data.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_html_location location = appropriate_place(parser, SXML_HTML_NO_NODE);
    if (location.parent == 0) {
        /* The Document cannot have text children. */
        return SXML_STATUS_OK;
    }
    sxml_html_working_tree *tree = parser->tree;
    const uint32_t previous = location.before != SXML_HTML_NO_NODE
                                  ? node_at(parser, location.before)->previous_sibling
                                  : node_at(parser, location.parent)->last_child;
    uint32_t text_node = SXML_HTML_NO_NODE;
    if (previous != SXML_HTML_NO_NODE &&
        node_at(parser, previous)->node_kind == SXML_HTML_NODE_KIND_TEXT) {
        text_node = previous;
        sxml_html_text_segment *last =
            sxml_xml_array_at(&tree->segments, node_at(parser, previous)->last_segment);
        if (last->text_range.byte_offset + last->text_range.byte_count == tree->text.byte_count) {
            last->text_range.byte_count += data.byte_count;
            return sxml_xml_buffer_append(&tree->text, data);
        }
    } else {
        const sxml_status status = make_node(tree, SXML_HTML_NODE_KIND_TEXT, &text_node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        insert_node_at(tree, text_node, location);
    }
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&tree->segments, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t segment_number = (uint32_t)(tree->segments.element_count - 1);
    sxml_html_text_segment *segment = element;
    segment->next_segment = SXML_HTML_NO_NODE;
    status = store_text(tree, data, &segment->text_range);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_html_node *text = node_at(parser, text_node);
    if (text->last_segment == SXML_HTML_NO_NODE) {
        text->first_segment = segment_number;
    } else {
        ((sxml_html_text_segment *)sxml_xml_array_at(&tree->segments, text->last_segment))
            ->next_segment = segment_number;
    }
    text->last_segment = segment_number;
    return SXML_STATUS_OK;
}

/* "Insert a comment" (comment token) or "insert a processing
   instruction", at location or, when location.parent is
   SXML_HTML_NO_NODE, the adjusted insertion location. */
static sxml_status insert_comment_like(sxml_html_parser *parser, const sxml_html_token *token,
                                       sxml_html_location location) {
    if (location.parent == SXML_HTML_NO_NODE) {
        location = appropriate_place(parser, SXML_HTML_NO_NODE);
    }
    const bool is_comment = token->token_kind == SXML_HTML_TOKEN_KIND_COMMENT;
    uint32_t node_number = 0;
    sxml_status status = make_node(parser->tree,
                                   is_comment ? SXML_HTML_NODE_KIND_COMMENT
                                              : SXML_HTML_NODE_KIND_PROCESSING_INSTRUCTION,
                                   &node_number);
    sxml_byte_range data = {.byte_offset = 0, .byte_count = 0};
    sxml_byte_range name = {.byte_offset = 0, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = store_text(parser->tree, token->data, &data);
    }
    if (status == SXML_STATUS_OK && !is_comment) {
        status = store_text(parser->tree, token->name, &name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    node_at(parser, node_number)->data = data;
    node_at(parser, node_number)->name = name;
    insert_node_at(parser->tree, node_number, location);
    return SXML_STATUS_OK;
}

static sxml_status insert_comment(sxml_html_parser *parser, const sxml_html_token *token) {
    return insert_comment_like(
        parser, token, (sxml_html_location){.parent = SXML_HTML_NO_NODE, .before = SXML_HTML_NO_NODE});
}

static sxml_status insert_comment_in(sxml_html_parser *parser, const sxml_html_token *token,
                                     uint32_t parent) {
    return insert_comment_like(parser, token,
                               (sxml_html_location){.parent = parent, .before = SXML_HTML_NO_NODE});
}

/* 13.2.4.3, "reconstruct the active formatting elements". */
static sxml_status reconstruct_formatting_elements(sxml_html_parser *parser) {
    const size_t count = formatting_count(parser);
    if (count == 0) {
        return SXML_STATUS_OK;
    }
    const uint32_t last = formatting_at(parser, count - 1);
    if (last == SXML_HTML_FORMATTING_MARKER || node_at(parser, last)->is_open) {
        return SXML_STATUS_OK;
    }
    size_t position = count - 1;
    while (position > 0) {
        const uint32_t earlier = formatting_at(parser, position - 1);
        if (earlier == SXML_HTML_FORMATTING_MARKER || node_at(parser, earlier)->is_open) {
            break;
        }
        position -= 1;
    }
    for (; position < count; position += 1) {
        uint32_t element = 0;
        sxml_status status = clone_element(parser, formatting_at(parser, position), &element);
        if (status == SXML_STATUS_OK) {
            status =
                insert_element_at(parser, element, appropriate_place(parser, SXML_HTML_NO_NODE));
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        set_formatting_at(parser, position, element);
    }
    return SXML_STATUS_OK;
}

/* --------------------------------------------------- Processing steps */

/* What a step of processing a token asks for next. */
typedef enum sxml_html_step_action {
    /* The token is done. */
    SXML_HTML_STEP_DONE,
    /* "Reprocess the token": through the dispatcher, in the insertion
       mode now current. */
    SXML_HTML_STEP_REPROCESS,
    /* "Process the token using the rules for" step.rules, which does not
       change the insertion mode. */
    SXML_HTML_STEP_USE_RULES,
    /* "Stop parsing" (13.2.7). */
    SXML_HTML_STEP_STOP,
} sxml_html_step_action;

typedef struct sxml_html_step {
    sxml_html_step_action action;
    sxml_html_insertion_mode rules;
} sxml_html_step;

/* A token as tree construction reads it: a run of characters of one
   class (U+0000, whitespace, other), or another token, with its tag. */
typedef struct sxml_html_tree_token {
    const sxml_html_token *token;
    uint32_t tag;
} sxml_html_tree_token;

static void finish_step(sxml_html_step *step) {
    step->action = SXML_HTML_STEP_DONE;
}

static void reprocess(sxml_html_step *step) {
    step->action = SXML_HTML_STEP_REPROCESS;
}

static void use_rules(sxml_html_step *step, sxml_html_insertion_mode rules) {
    step->action = SXML_HTML_STEP_USE_RULES;
    step->rules = rules;
}

static sxml_html_token_kind kind_of(const sxml_html_tree_token *input) {
    return input->token->token_kind;
}

static bool is_start_tag(const sxml_html_tree_token *input, uint32_t tag) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_START_TAG && input->tag == tag;
}

static bool is_end_tag(const sxml_html_tree_token *input, uint32_t tag) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && input->tag == tag;
}

static bool is_start_tag_in(const sxml_html_tree_token *input, const sxml_html_tag_list *list) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_START_TAG && tag_is_in(input->tag, list);
}

static bool is_end_tag_in(const sxml_html_tree_token *input, const sxml_html_tag_list *list) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && tag_is_in(input->tag, list);
}

/* Characters of the classes the insertion modes tell apart. */
static bool is_whitespace_byte(uint8_t byte) {
    return byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

static bool is_whitespace_run(const sxml_html_tree_token *input) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_CHARACTERS &&
           is_whitespace_byte(input->token->data.bytes[0]);
}

static bool is_null_run(const sxml_html_tree_token *input) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_CHARACTERS && input->token->data.bytes[0] == 0;
}

static bool is_comment_like(const sxml_html_tree_token *input) {
    return kind_of(input) == SXML_HTML_TOKEN_KIND_COMMENT ||
           kind_of(input) == SXML_HTML_TOKEN_KIND_PROCESSING_INSTRUCTION;
}

static void acknowledge_self_closing(sxml_html_parser *parser) {
    parser->has_acknowledged_self_closing = true;
}

/* An HTML element whose name is the token's (for names tree
   construction has no tag for). */
static bool is_html_element_named(const sxml_html_parser *parser, uint32_t node_number,
                                  sxml_byte_view name) {
    const sxml_html_node *node = node_at(parser, node_number);
    return node->node_kind == SXML_HTML_NODE_KIND_ELEMENT &&
           node->element_namespace == SXML_HTML_NAMESPACE_HTML &&
           sxml_byte_view_equals(tree_text(parser, node->name), name);
}

static uint32_t current_template_mode(const sxml_html_parser *parser) {
    return *(const uint8_t *)sxml_xml_array_at(&parser->template_modes,
                                               parser->template_modes.element_count - 1);
}

static sxml_status push_template_mode(sxml_html_parser *parser, sxml_html_insertion_mode mode) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&parser->template_modes, &element);
    if (status == SXML_STATUS_OK) {
        *(uint8_t *)element = (uint8_t)mode;
    }
    return status;
}

static void pop_template_mode(sxml_html_parser *parser) {
    if (parser->template_modes.element_count > 0) {
        parser->template_modes.element_count -= 1;
    }
}

/* 13.2.4.1, "reset the insertion mode appropriately" (no fragment). */
static void reset_insertion_mode(sxml_html_parser *parser) {
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        const bool is_last = position == 1;
        const sxml_html_node *node = node_at(parser, open_element_at(parser, position - 1));
        if (node->element_namespace != SXML_HTML_NAMESPACE_HTML) {
            continue;
        }
        switch (node->tag) {
        case SXML_HTML_TAG_TD:
        case SXML_HTML_TAG_TH:
            if (!is_last) {
                parser->mode = SXML_HTML_MODE_IN_CELL;
                return;
            }
            break;
        case SXML_HTML_TAG_TR:
            parser->mode = SXML_HTML_MODE_IN_ROW;
            return;
        case SXML_HTML_TAG_TBODY:
        case SXML_HTML_TAG_THEAD:
        case SXML_HTML_TAG_TFOOT:
            parser->mode = SXML_HTML_MODE_IN_TABLE_BODY;
            return;
        case SXML_HTML_TAG_CAPTION:
            parser->mode = SXML_HTML_MODE_IN_CAPTION;
            return;
        case SXML_HTML_TAG_COLGROUP:
            parser->mode = SXML_HTML_MODE_IN_COLUMN_GROUP;
            return;
        case SXML_HTML_TAG_TABLE:
            parser->mode = SXML_HTML_MODE_IN_TABLE;
            return;
        case SXML_HTML_TAG_TEMPLATE:
            parser->mode = (sxml_html_insertion_mode)current_template_mode(parser);
            return;
        case SXML_HTML_TAG_HEAD:
            if (!is_last) {
                parser->mode = SXML_HTML_MODE_IN_HEAD;
                return;
            }
            break;
        case SXML_HTML_TAG_BODY:
            parser->mode = SXML_HTML_MODE_IN_BODY;
            return;
        case SXML_HTML_TAG_FRAMESET:
            parser->mode = SXML_HTML_MODE_IN_FRAMESET;
            return;
        case SXML_HTML_TAG_HTML:
            parser->mode =
                parser->head_element == SXML_HTML_NO_NODE ? SXML_HTML_MODE_BEFORE_HEAD : SXML_HTML_MODE_AFTER_HEAD;
            return;
        default:
            break;
        }
    }
    parser->mode = SXML_HTML_MODE_IN_BODY;
}

/* "Close a p element" (13.2.6.4.7). */
static void close_p_element(sxml_html_parser *parser) {
    generate_implied_end_tags(parser, SXML_HTML_TAG_P);
    if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_P)) {
        report_parse_error(parser);
    }
    pop_until_html_tag(parser, SXML_HTML_TAG_P);
}

static void close_p_in_button_scope(sxml_html_parser *parser) {
    if (has_tag_in_specific_scope(parser, SXML_HTML_TAG_P, SXML_HTML_SCOPE_BUTTON)) {
        close_p_element(parser);
    }
}

/* 13.2.6.2: the generic raw text and RCDATA element parsing algorithms. */
static sxml_status parse_text_element(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                      sxml_html_content_state state) {
    const sxml_status status = insert_html_element(parser, input->token);
    sxml_html_tokenizer_switch_state(&parser->tokenizer, state);
    parser->original_mode = parser->mode;
    parser->mode = SXML_HTML_MODE_TEXT;
    return status;
}

/* Adds the token's attributes the element lacks ("start tag html" and
   "start tag body" in body). */
static sxml_status add_missing_attributes(sxml_html_parser *parser, uint32_t element_number,
                                          const sxml_html_token *token) {
    for (size_t attribute_index = 0; attribute_index < token->attributes.attribute_count;
         attribute_index += 1) {
        const sxml_html_token_attribute *new_attribute =
            &token->attributes.attributes[attribute_index];
        const sxml_html_node *element = node_at(parser, element_number);
        const sxml_html_attribute *attributes = element_attributes(parser, element);
        bool is_present = false;
        for (uint32_t existing = 0; existing < element->attribute_count && !is_present;
             existing += 1) {
            is_present = sxml_byte_view_equals(
                tree_text(parser, attributes[existing].attribute_name),
                new_attribute->attribute_name);
        }
        if (is_present) {
            continue;
        }
        /* The element's attributes stay contiguous: copy them to the end
           of the table and add the new one after them. */
        const uint32_t first = (uint32_t)parser->tree->attributes.element_count;
        const uint32_t old_first = element->first_attribute;
        const uint32_t old_count = element->attribute_count;
        for (uint32_t copied = 0; copied < old_count; copied += 1) {
            void *slot = NULL;
            const sxml_status status = sxml_xml_array_push(&parser->tree->attributes, &slot);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            *(sxml_html_attribute *)slot = *(const sxml_html_attribute *)sxml_xml_array_at(
                &parser->tree->attributes, old_first + copied);
        }
        const sxml_status status =
            store_attribute(parser, SXML_HTML_NAMESPACE_HTML, new_attribute);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_html_node *updated = node_at(parser, element_number);
        updated->first_attribute = first;
        updated->attribute_count = old_count + 1;
    }
    return SXML_STATUS_OK;
}

/* "Any other end tag" in body (13.2.6.4.7). */
static void close_any_other_end_tag(sxml_html_parser *parser, const sxml_html_tree_token *input) {
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        const uint32_t node = open_element_at(parser, position - 1);
        if (is_html_element_named(parser, node, input->token->name)) {
            generate_implied_end_tags(parser, input->tag);
            if (node != current_node(parser)) {
                report_parse_error(parser);
            }
            pop_until_node(parser, node);
            return;
        }
        if (is_special(parser, node)) {
            report_parse_error(parser);
            return;
        }
    }
}

/* 13.2.6.4.7, the adoption agency algorithm, for an end tag (or the start
   tags a and nobr) whose tag is a formatting element's. */
static sxml_status run_adoption_agency(sxml_html_parser *parser,
                                       const sxml_html_tree_token *input) {
    const uint32_t subject = input->tag;
    const uint32_t current = current_node(parser);
    if (is_html_element(parser, current, subject) &&
        formatting_position_of(parser, current) == SIZE_MAX) {
        pop_open_element(parser);
        return SXML_STATUS_OK;
    }
    for (uint32_t outer_count = 0; outer_count < 8; outer_count += 1) {
        const size_t formatting_position = find_formatting_after_marker(parser, subject);
        if (formatting_position == SIZE_MAX) {
            close_any_other_end_tag(parser, input);
            return SXML_STATUS_OK;
        }
        const uint32_t formatting_element = formatting_at(parser, formatting_position);
        if (!node_at(parser, formatting_element)->is_open) {
            report_parse_error(parser);
            remove_formatting_at(parser, formatting_position);
            return SXML_STATUS_OK;
        }
        if (!has_node_in_scope(parser, formatting_element)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        if (formatting_element != current_node(parser)) {
            report_parse_error(parser);
        }
        const size_t formatting_stack_position = open_position_of(parser, formatting_element);
        size_t furthest_position = SIZE_MAX;
        for (size_t position = formatting_stack_position + 1; position < open_count(parser);
             position += 1) {
            if (is_special(parser, open_element_at(parser, position))) {
                furthest_position = position;
                break;
            }
        }
        if (furthest_position == SIZE_MAX) {
            pop_until_node(parser, formatting_element);
            remove_formatting_element(parser, formatting_element);
            return SXML_STATUS_OK;
        }
        const uint32_t furthest_block = open_element_at(parser, furthest_position);
        const uint32_t common_ancestor = open_element_at(parser, formatting_stack_position - 1);
        /* The bookmark: the new element goes after this entry, or in the
           formatting element's place while it is SXML_HTML_NO_NODE. */
        uint32_t bookmark_after = SXML_HTML_NO_NODE;
        uint32_t last_node = furthest_block;
        size_t node_position = furthest_position;
        for (uint32_t inner_count = 1;; inner_count += 1) {
            node_position -= 1;
            uint32_t node = open_element_at(parser, node_position);
            if (node == formatting_element) {
                break;
            }
            size_t node_formatting_position = formatting_position_of(parser, node);
            if (inner_count > 3 && node_formatting_position != SIZE_MAX) {
                remove_formatting_at(parser, node_formatting_position);
                node_formatting_position = SIZE_MAX;
            }
            if (node_formatting_position == SIZE_MAX) {
                remove_open_element(parser, node);
                continue;
            }
            uint32_t replacement = 0;
            const sxml_status status = clone_element(parser, node, &replacement);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            set_formatting_at(parser, node_formatting_position, replacement);
            node_at(parser, node)->is_open = false;
            set_open_element_at(parser, node_position, replacement);
            node_at(parser, replacement)->is_open = true;
            node = replacement;
            if (last_node == furthest_block) {
                bookmark_after = node;
            }
            insert_node_at(parser->tree, last_node,
                           (sxml_html_location){.parent = node, .before = SXML_HTML_NO_NODE});
            last_node = node;
        }
        insert_node_at(parser->tree, last_node, appropriate_place(parser, common_ancestor));
        uint32_t new_element = 0;
        sxml_status status = clone_element(parser, formatting_element, &new_element);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        while (node_at(parser, furthest_block)->first_child != SXML_HTML_NO_NODE) {
            insert_node_at(parser->tree, node_at(parser, furthest_block)->first_child,
                           (sxml_html_location){.parent = new_element, .before = SXML_HTML_NO_NODE});
        }
        insert_node_at(parser->tree, new_element,
                       (sxml_html_location){.parent = furthest_block, .before = SXML_HTML_NO_NODE});
        if (bookmark_after == SXML_HTML_NO_NODE) {
            set_formatting_at(parser, formatting_position_of(parser, formatting_element),
                              new_element);
        } else {
            remove_formatting_element(parser, formatting_element);
            status = insert_formatting_at(parser, formatting_position_of(parser, bookmark_after) + 1,
                                          new_element);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        remove_open_element(parser, formatting_element);
        const size_t below = open_position_of(parser, furthest_block) + 1;
        status = push_open_element(parser, new_element);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        for (size_t moved = open_count(parser) - 1; moved > below; moved -= 1) {
            set_open_element_at(parser, moved, open_element_at(parser, moved - 1));
        }
        set_open_element_at(parser, below, new_element);
    }
    return SXML_STATUS_OK;
}

/* "If there is a node in the stack of open elements that is not" one of
   those that may stay open at the end: a parse error. */
static void check_open_elements_at_end(sxml_html_parser *parser) {
    for (size_t position = 0; position < open_count(parser); position += 1) {
        if (!is_html_element_in(parser, open_element_at(parser, position),
                                &closable_at_end_tags)) {
            report_parse_error(parser);
            return;
        }
    }
}

/* ------------------------------------------------------- Quirks mode */

static bool starts_with_ignoring_case(sxml_byte_view text, sxml_byte_view prefix) {
    if (text.byte_count < prefix.byte_count) {
        return false;
    }
    return is_ascii_case_insensitive_match(
        (sxml_byte_view){.bytes = text.bytes, .byte_count = prefix.byte_count}, prefix);
}

/* 13.2.6.4.1: the public identifier prefixes that set quirks mode, in
   lower case. */
static const sxml_byte_view quirks_public_prefixes[] = {
    SXML_BYTE_VIEW_INITIALIZER("+//silmaril//dtd html pro v0r11 19970101//"),
    SXML_BYTE_VIEW_INITIALIZER("-//as//dtd html 3.0 aswedit + extensions//"),
    SXML_BYTE_VIEW_INITIALIZER("-//advasoft ltd//dtd html 3.0 aswedit + extensions//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.0 level 1//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.0 level 2//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.0 strict level 1//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.0 strict level 2//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.0 strict//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 2.1e//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 3.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 3.2 final//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 3.2//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html 3//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html level 0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html level 1//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html level 2//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html level 3//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html strict level 0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html strict level 1//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html strict level 2//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html strict level 3//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html strict//"),
    SXML_BYTE_VIEW_INITIALIZER("-//ietf//dtd html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//metrius//dtd metrius presentational//"),
    SXML_BYTE_VIEW_INITIALIZER("-//microsoft//dtd internet explorer 2.0 html strict//"),
    SXML_BYTE_VIEW_INITIALIZER("-//microsoft//dtd internet explorer 2.0 html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//microsoft//dtd internet explorer 2.0 tables//"),
    SXML_BYTE_VIEW_INITIALIZER("-//microsoft//dtd internet explorer 3.0 html strict//"),
    SXML_BYTE_VIEW_INITIALIZER("-//microsoft//dtd internet explorer 3.0 html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//microsoft//dtd internet explorer 3.0 tables//"),
    SXML_BYTE_VIEW_INITIALIZER("-//netscape comm. corp.//dtd html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//netscape comm. corp.//dtd strict html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//o'reilly and associates//dtd html 2.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//o'reilly and associates//dtd html extended 1.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//o'reilly and associates//dtd html extended relaxed 1.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//sq//dtd html 2.0 hotmetal + extensions//"),
    SXML_BYTE_VIEW_INITIALIZER("-//softquad software//dtd hotmetal pro "
                               "6.0::19990601::extensions to html 4.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//softquad//dtd hotmetal pro 4.0::19971010::extensions to "
                               "html 4.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//spyglass//dtd html 2.0 extended//"),
    SXML_BYTE_VIEW_INITIALIZER("-//sun microsystems corp.//dtd hotjava html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//sun microsystems corp.//dtd hotjava strict html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 3 1995-03-24//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 3.2 draft//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 3.2 final//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 3.2//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 3.2s draft//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 4.0 frameset//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html 4.0 transitional//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html experimental 19960712//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd html experimental 970421//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3c//dtd w3 html//"),
    SXML_BYTE_VIEW_INITIALIZER("-//w3o//dtd w3 html 3.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//webtechs//dtd mozilla html 2.0//"),
    SXML_BYTE_VIEW_INITIALIZER("-//webtechs//dtd mozilla html//"),
};

static sxml_html_quirks_mode quirks_mode_of_doctype(const sxml_html_token *token) {
    const sxml_byte_view public_identifier = token->public_identifier;
    const bool has_public = token->has_public_identifier;
    const bool has_system_identifier =
        token->has_system_identifier && token->system_identifier.byte_count > 0;
    if (token->forces_quirks || !token->has_name ||
        !sxml_byte_view_equals(token->name, SXML_BYTE_VIEW_FROM_LITERAL("html"))) {
        return SXML_HTML_QUIRKS_MODE_QUIRKS;
    }
    if (has_public &&
        (is_ascii_case_insensitive_match(public_identifier,
                                         SXML_BYTE_VIEW_FROM_LITERAL("-//w3o//dtd w3 html strict 3.0//en//")) ||
         is_ascii_case_insensitive_match(public_identifier,
                                         SXML_BYTE_VIEW_FROM_LITERAL("-/w3c/dtd html 4.0 transitional/en")) ||
         is_ascii_case_insensitive_match(public_identifier, SXML_BYTE_VIEW_FROM_LITERAL("html")))) {
        return SXML_HTML_QUIRKS_MODE_QUIRKS;
    }
    if (token->has_system_identifier &&
        is_ascii_case_insensitive_match(
            token->system_identifier,
            SXML_BYTE_VIEW_FROM_LITERAL("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))) {
        return SXML_HTML_QUIRKS_MODE_QUIRKS;
    }
    if (!has_public) {
        return SXML_HTML_QUIRKS_MODE_NO_QUIRKS;
    }
    for (size_t prefix_index = 0;
         prefix_index < sizeof(quirks_public_prefixes) / sizeof(quirks_public_prefixes[0]);
         prefix_index += 1) {
        if (starts_with_ignoring_case(public_identifier, quirks_public_prefixes[prefix_index])) {
            return SXML_HTML_QUIRKS_MODE_QUIRKS;
        }
    }
    const bool is_html_401_loose =
        starts_with_ignoring_case(public_identifier,
                                  SXML_BYTE_VIEW_FROM_LITERAL("-//w3c//dtd html 4.01 frameset//")) ||
        starts_with_ignoring_case(public_identifier,
                                  SXML_BYTE_VIEW_FROM_LITERAL("-//w3c//dtd html 4.01 transitional//"));
    if (is_html_401_loose) {
        return has_system_identifier ? SXML_HTML_QUIRKS_MODE_LIMITED_QUIRKS : SXML_HTML_QUIRKS_MODE_QUIRKS;
    }
    if (starts_with_ignoring_case(public_identifier,
                                  SXML_BYTE_VIEW_FROM_LITERAL("-//w3c//dtd xhtml 1.0 frameset//")) ||
        starts_with_ignoring_case(public_identifier,
                                  SXML_BYTE_VIEW_FROM_LITERAL("-//w3c//dtd xhtml 1.0 transitional//"))) {
        return SXML_HTML_QUIRKS_MODE_LIMITED_QUIRKS;
    }
    return SXML_HTML_QUIRKS_MODE_NO_QUIRKS;
}

/* ---------------------------------------------------- Insertion modes */

/* 13.2.6.4.1 "initial". */
static sxml_status run_initial(sxml_html_parser *parser, const sxml_html_tree_token *input,
                               sxml_html_step *step) {
    finish_step(step);
    if (is_whitespace_run(input)) {
        return SXML_STATUS_OK;
    }
    if (is_comment_like(input)) {
        return insert_comment_in(parser, input->token, 0);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        const sxml_html_token *token = input->token;
        if (!token->has_name || !sxml_byte_view_equals(token->name, SXML_BYTE_VIEW_FROM_LITERAL("html")) ||
            token->has_public_identifier ||
            (token->has_system_identifier &&
             !sxml_byte_view_equals(token->system_identifier,
                                    SXML_BYTE_VIEW_FROM_LITERAL("about:legacy-compat")))) {
            report_parse_error(parser);
        }
        parser->quirks_mode = quirks_mode_of_doctype(token);
        parser->mode = SXML_HTML_MODE_BEFORE_HTML;
        return SXML_STATUS_OK;
    }
    report_parse_error(parser);
    parser->quirks_mode = SXML_HTML_QUIRKS_MODE_QUIRKS;
    parser->mode = SXML_HTML_MODE_BEFORE_HTML;
    reprocess(step);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.2 "before html". */
static sxml_status run_before_html(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                   sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(implying_end_tags, SXML_HTML_TAG_HEAD, SXML_HTML_TAG_BODY, SXML_HTML_TAG_HTML, SXML_HTML_TAG_BR);
    finish_step(step);
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_comment_like(input)) {
        return insert_comment_in(parser, input->token, 0);
    }
    if (is_whitespace_run(input)) {
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && !tag_is_in(input->tag, &implying_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    const sxml_html_token implied = start_tag_named(SXML_BYTE_VIEW_FROM_LITERAL("html"));
    const bool is_html_start = is_start_tag(input, SXML_HTML_TAG_HTML);
    uint32_t element = 0;
    sxml_status status = create_element(
        parser,
        &(sxml_html_element_request){.token = is_html_start ? input->token : &implied,
                                     .element_namespace = SXML_HTML_NAMESPACE_HTML},
        &element);
    if (status == SXML_STATUS_OK) {
        status = insert_element_at(parser, element,
                                   (sxml_html_location){.parent = 0, .before = SXML_HTML_NO_NODE});
    }
    parser->mode = SXML_HTML_MODE_BEFORE_HEAD;
    if (!is_html_start) {
        reprocess(step);
    }
    return status;
}

/* 13.2.6.4.3 "before head". */
static sxml_status run_before_head(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                   sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(implying_end_tags, SXML_HTML_TAG_HEAD, SXML_HTML_TAG_BODY, SXML_HTML_TAG_HTML, SXML_HTML_TAG_BR);
    finish_step(step);
    if (is_whitespace_run(input)) {
        return SXML_STATUS_OK;
    }
    if (is_comment_like(input)) {
        return insert_comment(parser, input->token);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HTML)) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && !tag_is_in(input->tag, &implying_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (is_start_tag(input, SXML_HTML_TAG_HEAD)) {
        status = insert_element_for_token(
            parser,
            &(sxml_html_element_request){.token = input->token,
                                         .element_namespace = SXML_HTML_NAMESPACE_HTML},
            &parser->head_element);
    } else {
        status = insert_implied_element(parser, SXML_BYTE_VIEW_FROM_LITERAL("head"),
                                        &parser->head_element);
        reprocess(step);
    }
    parser->mode = SXML_HTML_MODE_IN_HEAD;
    return status;
}

/* 13.2.6.4.4 "in head". */
static sxml_status run_in_head(sxml_html_parser *parser, const sxml_html_tree_token *input,
                               sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(void_head_tags, SXML_HTML_TAG_BASE, SXML_HTML_TAG_BASEFONT, SXML_HTML_TAG_BGSOUND, SXML_HTML_TAG_LINK, SXML_HTML_TAG_META);
    SXML_HTML_TAG_LIST_OF(raw_text_tags, SXML_HTML_TAG_NOFRAMES, SXML_HTML_TAG_STYLE);
    SXML_HTML_TAG_LIST_OF(implying_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_HTML, SXML_HTML_TAG_BR);
    finish_step(step);
    if (is_whitespace_run(input)) {
        return insert_characters(parser, input->token->data);
    }
    if (is_comment_like(input)) {
        return insert_comment(parser, input->token);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HTML)) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (is_start_tag_in(input, &void_head_tags)) {
        const sxml_status status = insert_html_element(parser, input->token);
        pop_open_element(parser);
        acknowledge_self_closing(parser);
        if (is_start_tag(input, SXML_HTML_TAG_META)) {
            consider_meta_encoding(parser, input->token);
        }
        return status;
    }
    if (is_start_tag(input, SXML_HTML_TAG_TITLE)) {
        return parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_RCDATA);
    }
    if (is_start_tag_in(input, &raw_text_tags)) {
        return parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_RAWTEXT);
    }
    if (is_start_tag(input, SXML_HTML_TAG_NOSCRIPT)) {
        /* The scripting mode is Disabled. */
        parser->mode = SXML_HTML_MODE_IN_HEAD_NOSCRIPT;
        return insert_html_element(parser, input->token);
    }
    if (is_start_tag(input, SXML_HTML_TAG_SCRIPT)) {
        return parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_SCRIPT_DATA);
    }
    if (is_end_tag(input, SXML_HTML_TAG_HEAD)) {
        pop_open_element(parser);
        parser->mode = SXML_HTML_MODE_AFTER_HEAD;
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_TEMPLATE)) {
        sxml_status status = append_formatting_entry(parser, SXML_HTML_FORMATTING_MARKER);
        parser->is_frameset_ok = false;
        parser->mode = SXML_HTML_MODE_IN_TEMPLATE;
        if (status == SXML_STATUS_OK) {
            status = push_template_mode(parser, SXML_HTML_MODE_IN_TEMPLATE);
        }
        return status == SXML_STATUS_OK ? insert_html_element(parser, input->token) : status;
    }
    if (is_end_tag(input, SXML_HTML_TAG_TEMPLATE)) {
        if (!has_template_on_stack(parser)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        generate_all_implied_end_tags_thoroughly(parser);
        if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_TEMPLATE)) {
            report_parse_error(parser);
        }
        pop_until_html_tag(parser, SXML_HTML_TAG_TEMPLATE);
        clear_formatting_to_last_marker(parser);
        pop_template_mode(parser);
        reset_insertion_mode(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HEAD) ||
        (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && !tag_is_in(input->tag, &implying_end_tags))) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    pop_open_element(parser);
    parser->mode = SXML_HTML_MODE_AFTER_HEAD;
    reprocess(step);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.5 "in head noscript". */
static sxml_status run_in_head_noscript(sxml_html_parser *parser,
                                        const sxml_html_tree_token *input, sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(head_tags, SXML_HTML_TAG_BASEFONT, SXML_HTML_TAG_BGSOUND, SXML_HTML_TAG_LINK, SXML_HTML_TAG_META, SXML_HTML_TAG_NOFRAMES, SXML_HTML_TAG_STYLE);
    finish_step(step);
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HTML)) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (is_end_tag(input, SXML_HTML_TAG_NOSCRIPT)) {
        pop_open_element(parser);
        parser->mode = SXML_HTML_MODE_IN_HEAD;
        return SXML_STATUS_OK;
    }
    if (is_whitespace_run(input) || is_comment_like(input) || is_start_tag_in(input, &head_tags)) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HEAD) || is_start_tag(input, SXML_HTML_TAG_NOSCRIPT) ||
        (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && input->tag != SXML_HTML_TAG_BR)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    report_parse_error(parser);
    pop_open_element(parser);
    parser->mode = SXML_HTML_MODE_IN_HEAD;
    reprocess(step);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.6 "after head". */
static sxml_status run_after_head(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                  sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(head_tags, SXML_HTML_TAG_BASE, SXML_HTML_TAG_BASEFONT, SXML_HTML_TAG_BGSOUND, SXML_HTML_TAG_LINK, SXML_HTML_TAG_META, SXML_HTML_TAG_NOFRAMES,
             SXML_HTML_TAG_SCRIPT, SXML_HTML_TAG_STYLE, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_TITLE);
    SXML_HTML_TAG_LIST_OF(implying_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_HTML, SXML_HTML_TAG_BR);
    finish_step(step);
    if (is_whitespace_run(input)) {
        return insert_characters(parser, input->token->data);
    }
    if (is_comment_like(input)) {
        return insert_comment(parser, input->token);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HTML)) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_BODY)) {
        parser->is_frameset_ok = false;
        parser->mode = SXML_HTML_MODE_IN_BODY;
        return insert_html_element(parser, input->token);
    }
    if (is_start_tag(input, SXML_HTML_TAG_FRAMESET)) {
        parser->mode = SXML_HTML_MODE_IN_FRAMESET;
        return insert_html_element(parser, input->token);
    }
    if (is_start_tag_in(input, &head_tags)) {
        report_parse_error(parser);
        parser->removes_head_after_token = true;
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return push_open_element(parser, parser->head_element);
    }
    if (is_end_tag(input, SXML_HTML_TAG_TEMPLATE)) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HEAD) ||
        (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG && !tag_is_in(input->tag, &implying_end_tags))) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    const sxml_status status =
        insert_implied_element(parser, SXML_BYTE_VIEW_FROM_LITERAL("body"), NULL);
    parser->is_frameset_ok = true;
    parser->mode = SXML_HTML_MODE_IN_BODY;
    reprocess(step);
    return status;
}

/* 13.2.6.4.8 "text". */
static sxml_status run_text(sxml_html_parser *parser, const sxml_html_tree_token *input,
                            sxml_html_step *step) {
    finish_step(step);
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_CHARACTERS) {
        return insert_characters(parser, input->token->data);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_OF_FILE) {
        report_parse_error(parser);
        pop_open_element(parser);
        parser->mode = parser->original_mode;
        reprocess(step);
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG) {
        pop_open_element(parser);
        parser->mode = parser->original_mode;
    }
    return SXML_STATUS_OK;
}

/* "In body": character tokens. */
static sxml_status insert_body_characters(sxml_html_parser *parser,
                                          const sxml_html_tree_token *input) {
    if (is_null_run(input)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    const sxml_status status = reconstruct_formatting_elements(parser);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_whitespace_run(input)) {
        parser->is_frameset_ok = false;
    }
    return insert_characters(parser, input->token->data);
}

/* Inserts an element that is popped at once, acknowledging the
   self-closing flag (area, br, img, input, param, ...). */
static sxml_status insert_void_element(sxml_html_parser *parser, const sxml_html_token *token) {
    const sxml_status status = insert_html_element(parser, token);
    pop_open_element(parser);
    acknowledge_self_closing(parser);
    return status;
}

/* "A start tag whose tag name is one of: area, br, embed, img, keygen,
   wbr" (also </br> and <image>). */
static sxml_status insert_body_void_element(sxml_html_parser *parser,
                                            const sxml_html_token *token) {
    const sxml_status status = reconstruct_formatting_elements(parser);
    parser->is_frameset_ok = false;
    return status == SXML_STATUS_OK ? insert_void_element(parser, token) : status;
}

static sxml_status insert_after_reconstructing(sxml_html_parser *parser,
                                               const sxml_html_token *token) {
    const sxml_status status = reconstruct_formatting_elements(parser);
    return status == SXML_STATUS_OK ? insert_html_element(parser, token) : status;
}

/* "li", "dd" and "dt" start tags: closes an open item of the same kind. */
static sxml_status start_list_item(sxml_html_parser *parser, const sxml_html_tree_token *input) {
    parser->is_frameset_ok = false;
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        const uint32_t node = open_element_at(parser, position - 1);
        uint32_t closed_tag = SXML_HTML_TAG_OTHER;
        if (input->tag == SXML_HTML_TAG_LI && is_html_element(parser, node, SXML_HTML_TAG_LI)) {
            closed_tag = SXML_HTML_TAG_LI;
        } else if (input->tag != SXML_HTML_TAG_LI && is_html_element(parser, node, SXML_HTML_TAG_DD)) {
            closed_tag = SXML_HTML_TAG_DD;
        } else if (input->tag != SXML_HTML_TAG_LI && is_html_element(parser, node, SXML_HTML_TAG_DT)) {
            closed_tag = SXML_HTML_TAG_DT;
        }
        if (closed_tag != SXML_HTML_TAG_OTHER) {
            generate_implied_end_tags(parser, closed_tag);
            if (!is_html_element(parser, current_node(parser), closed_tag)) {
                report_parse_error(parser);
            }
            pop_until_html_tag(parser, closed_tag);
            break;
        }
        if (is_special(parser, node) && !is_html_element(parser, node, SXML_HTML_TAG_ADDRESS) &&
            !is_html_element(parser, node, SXML_HTML_TAG_DIV) && !is_html_element(parser, node, SXML_HTML_TAG_P)) {
            break;
        }
    }
    close_p_in_button_scope(parser);
    return insert_html_element(parser, input->token);
}

/* Inserts a foreign element for <math> or <svg> in body. */
static sxml_status start_foreign_root(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                      sxml_html_namespace element_namespace) {
    sxml_status status = reconstruct_formatting_elements(parser);
    if (status == SXML_STATUS_OK) {
        status = insert_element_for_token(
            parser,
            &(sxml_html_element_request){.token = input->token,
                                         .element_namespace = element_namespace},
            NULL);
    }
    if (input->token->is_self_closing) {
        pop_open_element(parser);
        acknowledge_self_closing(parser);
    }
    return status;
}

static bool has_type_hidden(const sxml_html_token *token) {
    const sxml_html_attribute_lookup type =
        find_token_attribute(token, SXML_BYTE_VIEW_FROM_LITERAL("type"));
    return type.is_found &&
           is_ascii_case_insensitive_match(type.attribute_value, SXML_BYTE_VIEW_FROM_LITERAL("hidden"));
}

/* "In body": start tags. */
static sxml_status start_tag_in_body(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                     sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(head_tags, SXML_HTML_TAG_BASE, SXML_HTML_TAG_BASEFONT, SXML_HTML_TAG_BGSOUND, SXML_HTML_TAG_LINK, SXML_HTML_TAG_META, SXML_HTML_TAG_NOFRAMES,
             SXML_HTML_TAG_SCRIPT, SXML_HTML_TAG_STYLE, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_TITLE);
    SXML_HTML_TAG_LIST_OF(block_tags, SXML_HTML_TAG_ADDRESS, SXML_HTML_TAG_ARTICLE, SXML_HTML_TAG_ASIDE, SXML_HTML_TAG_BLOCKQUOTE, SXML_HTML_TAG_CENTER,
             SXML_HTML_TAG_DETAILS, SXML_HTML_TAG_DIALOG, SXML_HTML_TAG_DIR, SXML_HTML_TAG_DIV, SXML_HTML_TAG_DL, SXML_HTML_TAG_FIELDSET, SXML_HTML_TAG_FIGCAPTION,
             SXML_HTML_TAG_FIGURE, SXML_HTML_TAG_FOOTER, SXML_HTML_TAG_HEADER, SXML_HTML_TAG_HGROUP, SXML_HTML_TAG_MAIN, SXML_HTML_TAG_MENU, SXML_HTML_TAG_NAV, SXML_HTML_TAG_OL,
             SXML_HTML_TAG_P, SXML_HTML_TAG_SEARCH, SXML_HTML_TAG_SECTION, SXML_HTML_TAG_SUMMARY, SXML_HTML_TAG_UL);
    SXML_HTML_TAG_LIST_OF(plain_formatting_tags, SXML_HTML_TAG_B, SXML_HTML_TAG_BIG, SXML_HTML_TAG_CODE, SXML_HTML_TAG_EM, SXML_HTML_TAG_FONT, SXML_HTML_TAG_I, SXML_HTML_TAG_S,
             SXML_HTML_TAG_SMALL, SXML_HTML_TAG_STRIKE, SXML_HTML_TAG_STRONG, SXML_HTML_TAG_TT, SXML_HTML_TAG_U);
    SXML_HTML_TAG_LIST_OF(object_tags, SXML_HTML_TAG_APPLET, SXML_HTML_TAG_MARQUEE, SXML_HTML_TAG_OBJECT);
    SXML_HTML_TAG_LIST_OF(void_tags, SXML_HTML_TAG_AREA, SXML_HTML_TAG_BR, SXML_HTML_TAG_EMBED, SXML_HTML_TAG_IMG, SXML_HTML_TAG_KEYGEN, SXML_HTML_TAG_WBR);
    SXML_HTML_TAG_LIST_OF(media_tags, SXML_HTML_TAG_PARAM, SXML_HTML_TAG_SOURCE, SXML_HTML_TAG_TRACK);
    SXML_HTML_TAG_LIST_OF(ignored_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_FRAME, SXML_HTML_TAG_HEAD, SXML_HTML_TAG_TBODY,
             SXML_HTML_TAG_TD, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    const sxml_html_token *token = input->token;
    const uint32_t tag = input->tag;
    if (tag == SXML_HTML_TAG_HTML) {
        report_parse_error(parser);
        return has_template_on_stack(parser)
                   ? SXML_STATUS_OK
                   : add_missing_attributes(parser, open_element_at(parser, 0), token);
    }
    if (tag_is_in(tag, &head_tags)) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (tag == SXML_HTML_TAG_BODY) {
        report_parse_error(parser);
        if (open_count(parser) == 1 || !is_html_element(parser, open_element_at(parser, 1), SXML_HTML_TAG_BODY) ||
            has_template_on_stack(parser)) {
            return SXML_STATUS_OK;
        }
        parser->is_frameset_ok = false;
        return add_missing_attributes(parser, open_element_at(parser, 1), token);
    }
    if (tag == SXML_HTML_TAG_FRAMESET) {
        report_parse_error(parser);
        if (open_count(parser) == 1 || !is_html_element(parser, open_element_at(parser, 1), SXML_HTML_TAG_BODY) ||
            !parser->is_frameset_ok) {
            return SXML_STATUS_OK;
        }
        detach_node(parser->tree, open_element_at(parser, 1));
        while (open_count(parser) > 1) {
            pop_open_element(parser);
        }
        parser->mode = SXML_HTML_MODE_IN_FRAMESET;
        return insert_html_element(parser, token);
    }
    if (tag_is_in(tag, &block_tags)) {
        close_p_in_button_scope(parser);
        return insert_html_element(parser, token);
    }
    if (tag_is_in(tag, &heading_tags)) {
        close_p_in_button_scope(parser);
        if (is_html_element_in(parser, current_node(parser), &heading_tags)) {
            report_parse_error(parser);
            pop_open_element(parser);
        }
        return insert_html_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_PRE || tag == SXML_HTML_TAG_LISTING) {
        close_p_in_button_scope(parser);
        parser->ignores_next_line_feed = true;
        parser->is_frameset_ok = false;
        return insert_html_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_FORM) {
        const bool is_in_template = has_template_on_stack(parser);
        if (parser->form_element != SXML_HTML_NO_NODE && !is_in_template) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        close_p_in_button_scope(parser);
        uint32_t form = 0;
        const sxml_status status = insert_element_for_token(
            parser,
            &(sxml_html_element_request){.token = token,
                                         .element_namespace = SXML_HTML_NAMESPACE_HTML},
            &form);
        if (!is_in_template) {
            parser->form_element = form;
        }
        return status;
    }
    if (tag == SXML_HTML_TAG_LI || tag == SXML_HTML_TAG_DD || tag == SXML_HTML_TAG_DT) {
        return start_list_item(parser, input);
    }
    if (tag == SXML_HTML_TAG_PLAINTEXT) {
        close_p_in_button_scope(parser);
        sxml_html_tokenizer_switch_state(&parser->tokenizer, SXML_HTML_CONTENT_STATE_PLAINTEXT);
        return insert_html_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_BUTTON) {
        if (has_tag_in_scope(parser, SXML_HTML_TAG_BUTTON)) {
            report_parse_error(parser);
            generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
            pop_until_html_tag(parser, SXML_HTML_TAG_BUTTON);
        }
        parser->is_frameset_ok = false;
        return insert_after_reconstructing(parser, token);
    }
    if (tag == SXML_HTML_TAG_A) {
        const size_t position = find_formatting_after_marker(parser, SXML_HTML_TAG_A);
        if (position != SIZE_MAX) {
            const uint32_t element = formatting_at(parser, position);
            report_parse_error(parser);
            const sxml_status status = run_adoption_agency(parser, input);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            remove_formatting_element(parser, element);
            remove_open_element(parser, element);
        }
    }
    if (tag == SXML_HTML_TAG_NOBR) {
        sxml_status status = reconstruct_formatting_elements(parser);
        if (status == SXML_STATUS_OK && has_tag_in_scope(parser, SXML_HTML_TAG_NOBR)) {
            report_parse_error(parser);
            status = run_adoption_agency(parser, input);
            if (status == SXML_STATUS_OK) {
                status = reconstruct_formatting_elements(parser);
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (tag == SXML_HTML_TAG_A || tag == SXML_HTML_TAG_NOBR || tag_is_in(tag, &plain_formatting_tags)) {
        uint32_t element = 0;
        sxml_status status = reconstruct_formatting_elements(parser);
        if (status == SXML_STATUS_OK) {
            status = insert_element_for_token(
                parser,
                &(sxml_html_element_request){.token = token,
                                             .element_namespace = SXML_HTML_NAMESPACE_HTML},
                &element);
        }
        return status == SXML_STATUS_OK ? push_formatting_element(parser, element) : status;
    }
    if (tag_is_in(tag, &object_tags)) {
        parser->is_frameset_ok = false;
        const sxml_status status = insert_after_reconstructing(parser, token);
        return status == SXML_STATUS_OK
                   ? append_formatting_entry(parser, SXML_HTML_FORMATTING_MARKER)
                   : status;
    }
    if (tag == SXML_HTML_TAG_TABLE) {
        if (parser->quirks_mode != SXML_HTML_QUIRKS_MODE_QUIRKS) {
            close_p_in_button_scope(parser);
        }
        parser->is_frameset_ok = false;
        parser->mode = SXML_HTML_MODE_IN_TABLE;
        return insert_html_element(parser, token);
    }
    if (tag_is_in(tag, &void_tags)) {
        return insert_body_void_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_INPUT) {
        if (has_tag_in_scope(parser, SXML_HTML_TAG_SELECT)) {
            report_parse_error(parser);
            pop_until_html_tag(parser, SXML_HTML_TAG_SELECT);
        }
        if (!has_type_hidden(token)) {
            parser->is_frameset_ok = false;
        }
        const sxml_status status = reconstruct_formatting_elements(parser);
        return status == SXML_STATUS_OK ? insert_void_element(parser, token) : status;
    }
    if (tag_is_in(tag, &media_tags)) {
        return insert_void_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_HR) {
        close_p_in_button_scope(parser);
        if (has_tag_in_scope(parser, SXML_HTML_TAG_SELECT)) {
            generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
            if (has_tag_in_scope(parser, SXML_HTML_TAG_OPTION) || has_tag_in_scope(parser, SXML_HTML_TAG_OPTGROUP)) {
                report_parse_error(parser);
            }
        }
        parser->is_frameset_ok = false;
        return insert_void_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_IMAGE) {
        /* "Change the token's tag name to img and reprocess it." */
        report_parse_error(parser);
        sxml_html_token renamed = *token;
        renamed.name = SXML_BYTE_VIEW_FROM_LITERAL("img");
        return insert_body_void_element(parser, &renamed);
    }
    if (tag == SXML_HTML_TAG_TEXTAREA) {
        parser->ignores_next_line_feed = true;
        parser->is_frameset_ok = false;
        return parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_RCDATA);
    }
    if (tag == SXML_HTML_TAG_XMP) {
        close_p_in_button_scope(parser);
        const sxml_status status = reconstruct_formatting_elements(parser);
        parser->is_frameset_ok = false;
        return status == SXML_STATUS_OK
                   ? parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_RAWTEXT)
                   : status;
    }
    if (tag == SXML_HTML_TAG_IFRAME) {
        parser->is_frameset_ok = false;
        return parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_RAWTEXT);
    }
    if (tag == SXML_HTML_TAG_NOEMBED) {
        return parse_text_element(parser, input, SXML_HTML_CONTENT_STATE_RAWTEXT);
    }
    if (tag == SXML_HTML_TAG_SELECT) {
        if (has_tag_in_scope(parser, SXML_HTML_TAG_SELECT)) {
            report_parse_error(parser);
            pop_until_html_tag(parser, SXML_HTML_TAG_SELECT);
            return SXML_STATUS_OK;
        }
        parser->is_frameset_ok = false;
        return insert_after_reconstructing(parser, token);
    }
    if (tag == SXML_HTML_TAG_OPTION || tag == SXML_HTML_TAG_OPTGROUP) {
        if (has_tag_in_scope(parser, SXML_HTML_TAG_SELECT)) {
            generate_implied_end_tags(parser, tag == SXML_HTML_TAG_OPTION ? SXML_HTML_TAG_OPTGROUP : SXML_HTML_TAG_OTHER);
            if (has_tag_in_scope(parser, SXML_HTML_TAG_OPTION) ||
                (tag == SXML_HTML_TAG_OPTGROUP && has_tag_in_scope(parser, SXML_HTML_TAG_OPTGROUP))) {
                report_parse_error(parser);
            }
        } else if (is_html_element(parser, current_node(parser), SXML_HTML_TAG_OPTION)) {
            pop_open_element(parser);
        }
        return insert_after_reconstructing(parser, token);
    }
    if (tag == SXML_HTML_TAG_RB || tag == SXML_HTML_TAG_RTC) {
        if (has_tag_in_scope(parser, SXML_HTML_TAG_RUBY)) {
            generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
            if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_RUBY)) {
                report_parse_error(parser);
            }
        }
        return insert_html_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_RP || tag == SXML_HTML_TAG_RT) {
        if (has_tag_in_scope(parser, SXML_HTML_TAG_RUBY)) {
            generate_implied_end_tags(parser, SXML_HTML_TAG_RTC);
            if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_RTC) &&
                !is_html_element(parser, current_node(parser), SXML_HTML_TAG_RUBY)) {
                report_parse_error(parser);
            }
        }
        return insert_html_element(parser, token);
    }
    if (tag == SXML_HTML_TAG_MATH) {
        return start_foreign_root(parser, input, SXML_HTML_NAMESPACE_MATHML);
    }
    if (tag == SXML_HTML_TAG_SVG) {
        return start_foreign_root(parser, input, SXML_HTML_NAMESPACE_SVG);
    }
    if (tag_is_in(tag, &ignored_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    return insert_after_reconstructing(parser, token);
}

/* An end tag that closes an HTML element of its tag in scope, after
   generating implied end tags; false when none is in scope (a parse
   error, the token ignored). */
static bool close_element_in_scope(sxml_html_parser *parser, uint32_t tag) {
    if (!has_tag_in_scope(parser, tag)) {
        report_parse_error(parser);
        return false;
    }
    generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
    if (!is_html_element(parser, current_node(parser), tag)) {
        report_parse_error(parser);
    }
    pop_until_html_tag(parser, tag);
    return true;
}

/* "In body": end tags. */
static sxml_status end_tag_in_body(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                   sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(block_tags, SXML_HTML_TAG_ADDRESS, SXML_HTML_TAG_ARTICLE, SXML_HTML_TAG_ASIDE, SXML_HTML_TAG_BLOCKQUOTE, SXML_HTML_TAG_BUTTON,
             SXML_HTML_TAG_CENTER, SXML_HTML_TAG_DETAILS, SXML_HTML_TAG_DIALOG, SXML_HTML_TAG_DIR, SXML_HTML_TAG_DIV, SXML_HTML_TAG_DL, SXML_HTML_TAG_FIELDSET,
             SXML_HTML_TAG_FIGCAPTION, SXML_HTML_TAG_FIGURE, SXML_HTML_TAG_FOOTER, SXML_HTML_TAG_HEADER, SXML_HTML_TAG_HGROUP, SXML_HTML_TAG_LISTING, SXML_HTML_TAG_MAIN,
             SXML_HTML_TAG_MENU, SXML_HTML_TAG_NAV, SXML_HTML_TAG_OL, SXML_HTML_TAG_PRE, SXML_HTML_TAG_SEARCH, SXML_HTML_TAG_SECTION, SXML_HTML_TAG_SELECT, SXML_HTML_TAG_SUMMARY,
             SXML_HTML_TAG_UL);
    SXML_HTML_TAG_LIST_OF(object_tags, SXML_HTML_TAG_APPLET, SXML_HTML_TAG_MARQUEE, SXML_HTML_TAG_OBJECT);
    const uint32_t tag = input->tag;
    if (tag == SXML_HTML_TAG_TEMPLATE) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (tag == SXML_HTML_TAG_BODY || tag == SXML_HTML_TAG_HTML) {
        if (!has_tag_in_scope(parser, SXML_HTML_TAG_BODY)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        check_open_elements_at_end(parser);
        parser->mode = SXML_HTML_MODE_AFTER_BODY;
        if (tag == SXML_HTML_TAG_HTML) {
            reprocess(step);
        }
        return SXML_STATUS_OK;
    }
    if (tag_is_in(tag, &block_tags)) {
        (void)close_element_in_scope(parser, tag);
        return SXML_STATUS_OK;
    }
    if (tag == SXML_HTML_TAG_FORM) {
        if (!has_template_on_stack(parser)) {
            const uint32_t node = parser->form_element;
            parser->form_element = SXML_HTML_NO_NODE;
            if (node == SXML_HTML_NO_NODE || !has_node_in_scope(parser, node)) {
                report_parse_error(parser);
                return SXML_STATUS_OK;
            }
            generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
            if (current_node(parser) != node) {
                report_parse_error(parser);
            }
            remove_open_element(parser, node);
            return SXML_STATUS_OK;
        }
        (void)close_element_in_scope(parser, SXML_HTML_TAG_FORM);
        return SXML_STATUS_OK;
    }
    if (tag == SXML_HTML_TAG_P) {
        if (!has_tag_in_specific_scope(parser, SXML_HTML_TAG_P, SXML_HTML_SCOPE_BUTTON)) {
            report_parse_error(parser);
            const sxml_status status =
                insert_implied_element(parser, SXML_BYTE_VIEW_FROM_LITERAL("p"), NULL);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        close_p_element(parser);
        return SXML_STATUS_OK;
    }
    if (tag == SXML_HTML_TAG_LI || tag == SXML_HTML_TAG_DD || tag == SXML_HTML_TAG_DT) {
        const sxml_html_scope scope = tag == SXML_HTML_TAG_LI ? SXML_HTML_SCOPE_LIST_ITEM : SXML_HTML_SCOPE_DEFAULT;
        if (!has_tag_in_specific_scope(parser, tag, scope)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        generate_implied_end_tags(parser, tag);
        if (!is_html_element(parser, current_node(parser), tag)) {
            report_parse_error(parser);
        }
        pop_until_html_tag(parser, tag);
        return SXML_STATUS_OK;
    }
    if (tag_is_in(tag, &heading_tags)) {
        if (!has_in_scope(parser, &(sxml_html_scope_query){
                                      .target = {.tag = SXML_HTML_TAG_OTHER,
                                                 .tags = &heading_tags,
                                                 .node = SXML_HTML_NO_NODE},
                                      .scope = SXML_HTML_SCOPE_DEFAULT})) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
        if (!is_html_element(parser, current_node(parser), tag)) {
            report_parse_error(parser);
        }
        while (open_count(parser) > 0) {
            const uint32_t node = current_node(parser);
            pop_open_element(parser);
            if (is_html_element_in(parser, node, &heading_tags)) {
                break;
            }
        }
        return SXML_STATUS_OK;
    }
    if (tag_is_in(tag, &formatting_tags)) {
        return run_adoption_agency(parser, input);
    }
    if (tag_is_in(tag, &object_tags)) {
        if (close_element_in_scope(parser, tag)) {
            clear_formatting_to_last_marker(parser);
        }
        return SXML_STATUS_OK;
    }
    if (tag == SXML_HTML_TAG_BR) {
        /* Acts as a br start tag token with no attributes. */
        report_parse_error(parser);
        const sxml_html_token start = start_tag_named(SXML_BYTE_VIEW_FROM_LITERAL("br"));
        return insert_body_void_element(parser, &start);
    }
    close_any_other_end_tag(parser, input);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.7 "in body". */
static sxml_status run_in_body(sxml_html_parser *parser, const sxml_html_tree_token *input,
                               sxml_html_step *step) {
    finish_step(step);
    switch (kind_of(input)) {
    case SXML_HTML_TOKEN_KIND_CHARACTERS:
        return insert_body_characters(parser, input);
    case SXML_HTML_TOKEN_KIND_COMMENT:
    case SXML_HTML_TOKEN_KIND_PROCESSING_INSTRUCTION:
        return insert_comment(parser, input->token);
    case SXML_HTML_TOKEN_KIND_DOCTYPE:
        report_parse_error(parser);
        return SXML_STATUS_OK;
    case SXML_HTML_TOKEN_KIND_START_TAG:
        return start_tag_in_body(parser, input, step);
    case SXML_HTML_TOKEN_KIND_END_TAG:
        return end_tag_in_body(parser, input, step);
    case SXML_HTML_TOKEN_KIND_END_OF_FILE:
        if (parser->template_modes.element_count > 0) {
            use_rules(step, SXML_HTML_MODE_IN_TEMPLATE);
            return SXML_STATUS_OK;
        }
        check_open_elements_at_end(parser);
        step->action = SXML_HTML_STEP_STOP;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* "Clear the stack back to a table context" and its body and row forms
   (13.2.6.4.9, .13, .14): pops while the current node is not in stop. */
static void clear_stack_back_to(sxml_html_parser *parser, const sxml_html_tag_list *stop) {
    while (open_count(parser) > 0 && !is_html_element_in(parser, current_node(parser), stop)) {
        pop_open_element(parser);
    }
}

SXML_HTML_TAG_LIST_OF(table_context_tags, SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_HTML);
SXML_HTML_TAG_LIST_OF(table_body_context_tags, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_HTML);
SXML_HTML_TAG_LIST_OF(table_row_context_tags, SXML_HTML_TAG_TR, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_HTML);
SXML_HTML_TAG_LIST_OF(table_section_tags, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_THEAD);

static bool has_tag_in_table_scope(const sxml_html_parser *parser, uint32_t tag) {
    return has_tag_in_specific_scope(parser, tag, SXML_HTML_SCOPE_TABLE);
}

/* 13.2.6.4.9 "in table". */
static sxml_status run_in_table(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(text_parents, SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    SXML_HTML_TAG_LIST_OF(ignored_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_HTML, SXML_HTML_TAG_TBODY,
             SXML_HTML_TAG_TD, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    SXML_HTML_TAG_LIST_OF(head_tags, SXML_HTML_TAG_STYLE, SXML_HTML_TAG_SCRIPT, SXML_HTML_TAG_TEMPLATE);
    SXML_HTML_TAG_LIST_OF(cell_tags, SXML_HTML_TAG_TD, SXML_HTML_TAG_TH, SXML_HTML_TAG_TR);
    finish_step(step);
    const uint32_t tag = input->tag;
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_CHARACTERS &&
        is_html_element_in(parser, current_node(parser), &text_parents)) {
        parser->pending_table_text.byte_count = 0;
        parser->has_pending_non_whitespace = false;
        parser->original_mode = parser->mode;
        parser->mode = SXML_HTML_MODE_IN_TABLE_TEXT;
        reprocess(step);
        return SXML_STATUS_OK;
    }
    if (is_comment_like(input)) {
        return insert_comment(parser, input->token);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_CAPTION)) {
        clear_stack_back_to(parser, &table_context_tags);
        const sxml_status status = append_formatting_entry(parser, SXML_HTML_FORMATTING_MARKER);
        parser->mode = SXML_HTML_MODE_IN_CAPTION;
        return status == SXML_STATUS_OK ? insert_html_element(parser, input->token) : status;
    }
    if (is_start_tag(input, SXML_HTML_TAG_COLGROUP) || is_start_tag(input, SXML_HTML_TAG_COL)) {
        clear_stack_back_to(parser, &table_context_tags);
        parser->mode = SXML_HTML_MODE_IN_COLUMN_GROUP;
        if (tag == SXML_HTML_TAG_COL) {
            reprocess(step);
            return insert_implied_element(parser, SXML_BYTE_VIEW_FROM_LITERAL("colgroup"), NULL);
        }
        return insert_html_element(parser, input->token);
    }
    if (is_start_tag_in(input, &table_section_tags) || is_start_tag_in(input, &cell_tags)) {
        clear_stack_back_to(parser, &table_context_tags);
        parser->mode = SXML_HTML_MODE_IN_TABLE_BODY;
        if (tag_is_in(tag, &cell_tags)) {
            reprocess(step);
            return insert_implied_element(parser, SXML_BYTE_VIEW_FROM_LITERAL("tbody"), NULL);
        }
        return insert_html_element(parser, input->token);
    }
    if (is_start_tag(input, SXML_HTML_TAG_TABLE) || is_end_tag(input, SXML_HTML_TAG_TABLE)) {
        if (kind_of(input) == SXML_HTML_TOKEN_KIND_START_TAG) {
            report_parse_error(parser);
        }
        if (!has_tag_in_table_scope(parser, SXML_HTML_TAG_TABLE)) {
            if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG) {
                report_parse_error(parser);
            }
            return SXML_STATUS_OK;
        }
        pop_until_html_tag(parser, SXML_HTML_TAG_TABLE);
        reset_insertion_mode(parser);
        if (kind_of(input) == SXML_HTML_TOKEN_KIND_START_TAG) {
            reprocess(step);
        }
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &ignored_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag_in(input, &head_tags) || is_end_tag(input, SXML_HTML_TAG_TEMPLATE)) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_INPUT) && has_type_hidden(input->token)) {
        report_parse_error(parser);
        return insert_void_element(parser, input->token);
    }
    if (is_start_tag(input, SXML_HTML_TAG_FORM)) {
        report_parse_error(parser);
        const bool is_in_template = has_template_on_stack(parser);
        if (parser->form_element != SXML_HTML_NO_NODE && !is_in_template) {
            return SXML_STATUS_OK;
        }
        uint32_t form = 0;
        const sxml_status status = insert_element_for_token(
            parser,
            &(sxml_html_element_request){.token = input->token,
                                         .element_namespace = SXML_HTML_NAMESPACE_HTML},
            &form);
        if (!is_in_template) {
            parser->form_element = form;
        }
        pop_open_element(parser);
        return status;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_OF_FILE) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    report_parse_error(parser);
    parser->is_foster_parenting = true;
    use_rules(step, SXML_HTML_MODE_IN_BODY);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.10 "in table text". */
static sxml_status run_in_table_text(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                     sxml_html_step *step) {
    finish_step(step);
    if (is_null_run(input)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_CHARACTERS) {
        if (!is_whitespace_run(input)) {
            parser->has_pending_non_whitespace = true;
        }
        return sxml_xml_buffer_append(&parser->pending_table_text, input->token->data);
    }
    const sxml_byte_view pending = {.bytes = parser->pending_table_text.bytes,
                                    .byte_count = parser->pending_table_text.byte_count};
    sxml_status status = SXML_STATUS_OK;
    if (parser->has_pending_non_whitespace) {
        /* Reprocessed with the "anything else" rules of "in table": foster
           parenting and the "in body" rules for characters. */
        report_parse_error(parser);
        const bool was_foster_parenting = parser->is_foster_parenting;
        parser->is_foster_parenting = true;
        status = reconstruct_formatting_elements(parser);
        if (status == SXML_STATUS_OK) {
            parser->is_frameset_ok = false;
            status = insert_characters(parser, pending);
        }
        parser->is_foster_parenting = was_foster_parenting;
    } else {
        status = insert_characters(parser, pending);
    }
    parser->pending_table_text.byte_count = 0;
    parser->has_pending_non_whitespace = false;
    parser->mode = parser->original_mode;
    reprocess(step);
    return status;
}

/* Ends the caption (13.2.6.4.11); false when none is in table scope. */
static bool close_caption(sxml_html_parser *parser) {
    if (!has_tag_in_table_scope(parser, SXML_HTML_TAG_CAPTION)) {
        report_parse_error(parser);
        return false;
    }
    generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
    if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_CAPTION)) {
        report_parse_error(parser);
    }
    pop_until_html_tag(parser, SXML_HTML_TAG_CAPTION);
    clear_formatting_to_last_marker(parser);
    parser->mode = SXML_HTML_MODE_IN_TABLE;
    return true;
}

/* 13.2.6.4.11 "in caption". */
static sxml_status run_in_caption(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                  sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(table_start_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TD, SXML_HTML_TAG_TFOOT,
             SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    SXML_HTML_TAG_LIST_OF(ignored_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_HTML, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TD,
             SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    finish_step(step);
    if (is_end_tag(input, SXML_HTML_TAG_CAPTION)) {
        (void)close_caption(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag_in(input, &table_start_tags) || is_end_tag(input, SXML_HTML_TAG_TABLE)) {
        if (close_caption(parser)) {
            reprocess(step);
        }
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &ignored_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    use_rules(step, SXML_HTML_MODE_IN_BODY);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.12 "in column group". */
static sxml_status run_in_column_group(sxml_html_parser *parser,
                                       const sxml_html_tree_token *input, sxml_html_step *step) {
    finish_step(step);
    if (is_whitespace_run(input)) {
        return insert_characters(parser, input->token->data);
    }
    if (is_comment_like(input)) {
        return insert_comment(parser, input->token);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_HTML)) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_COL)) {
        return insert_void_element(parser, input->token);
    }
    if (is_end_tag(input, SXML_HTML_TAG_COL)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag(input, SXML_HTML_TAG_TEMPLATE) || is_end_tag(input, SXML_HTML_TAG_TEMPLATE)) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_OF_FILE) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_COLGROUP)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    pop_open_element(parser);
    parser->mode = SXML_HTML_MODE_IN_TABLE;
    if (!is_end_tag(input, SXML_HTML_TAG_COLGROUP)) {
        reprocess(step);
    }
    return SXML_STATUS_OK;
}

/* 13.2.6.4.13 "in table body". */
static sxml_status run_in_table_body(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                     sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(leaving_start_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT,
             SXML_HTML_TAG_THEAD);
    SXML_HTML_TAG_LIST_OF(ignored_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_HTML, SXML_HTML_TAG_TD,
             SXML_HTML_TAG_TH, SXML_HTML_TAG_TR);
    finish_step(step);
    if (is_start_tag(input, SXML_HTML_TAG_TR)) {
        clear_stack_back_to(parser, &table_body_context_tags);
        parser->mode = SXML_HTML_MODE_IN_ROW;
        return insert_html_element(parser, input->token);
    }
    if (is_start_tag(input, SXML_HTML_TAG_TH) || is_start_tag(input, SXML_HTML_TAG_TD)) {
        report_parse_error(parser);
        clear_stack_back_to(parser, &table_body_context_tags);
        parser->mode = SXML_HTML_MODE_IN_ROW;
        reprocess(step);
        return insert_implied_element(parser, SXML_BYTE_VIEW_FROM_LITERAL("tr"), NULL);
    }
    if (is_end_tag_in(input, &table_section_tags)) {
        if (!has_tag_in_table_scope(parser, input->tag)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        clear_stack_back_to(parser, &table_body_context_tags);
        pop_open_element(parser);
        parser->mode = SXML_HTML_MODE_IN_TABLE;
        return SXML_STATUS_OK;
    }
    if (is_start_tag_in(input, &leaving_start_tags) || is_end_tag(input, SXML_HTML_TAG_TABLE)) {
        if (!has_tag_in_table_scope(parser, SXML_HTML_TAG_TBODY) && !has_tag_in_table_scope(parser, SXML_HTML_TAG_THEAD) &&
            !has_tag_in_table_scope(parser, SXML_HTML_TAG_TFOOT)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        clear_stack_back_to(parser, &table_body_context_tags);
        pop_open_element(parser);
        parser->mode = SXML_HTML_MODE_IN_TABLE;
        reprocess(step);
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &ignored_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    use_rules(step, SXML_HTML_MODE_IN_TABLE);
    return SXML_STATUS_OK;
}

/* Ends the row (13.2.6.4.14); false when no tr is in table scope. */
static bool close_row(sxml_html_parser *parser) {
    if (!has_tag_in_table_scope(parser, SXML_HTML_TAG_TR)) {
        report_parse_error(parser);
        return false;
    }
    clear_stack_back_to(parser, &table_row_context_tags);
    pop_open_element(parser);
    parser->mode = SXML_HTML_MODE_IN_TABLE_BODY;
    return true;
}

/* 13.2.6.4.14 "in row". */
static sxml_status run_in_row(sxml_html_parser *parser, const sxml_html_tree_token *input,
                              sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(leaving_start_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT,
             SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    SXML_HTML_TAG_LIST_OF(ignored_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_HTML, SXML_HTML_TAG_TD,
             SXML_HTML_TAG_TH);
    finish_step(step);
    if (is_start_tag(input, SXML_HTML_TAG_TH) || is_start_tag(input, SXML_HTML_TAG_TD)) {
        clear_stack_back_to(parser, &table_row_context_tags);
        parser->mode = SXML_HTML_MODE_IN_CELL;
        const sxml_status status = insert_html_element(parser, input->token);
        return status == SXML_STATUS_OK
                   ? append_formatting_entry(parser, SXML_HTML_FORMATTING_MARKER)
                   : status;
    }
    if (is_end_tag(input, SXML_HTML_TAG_TR)) {
        (void)close_row(parser);
        return SXML_STATUS_OK;
    }
    if (is_start_tag_in(input, &leaving_start_tags) || is_end_tag(input, SXML_HTML_TAG_TABLE)) {
        if (close_row(parser)) {
            reprocess(step);
        }
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &table_section_tags)) {
        if (!has_tag_in_table_scope(parser, input->tag)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        if (has_tag_in_table_scope(parser, SXML_HTML_TAG_TR) && close_row(parser)) {
            reprocess(step);
        }
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &ignored_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    use_rules(step, SXML_HTML_MODE_IN_TABLE);
    return SXML_STATUS_OK;
}

/* "Close the cell" (13.2.6.4.15). */
static void close_cell(sxml_html_parser *parser) {
    generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
    if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_TD) &&
        !is_html_element(parser, current_node(parser), SXML_HTML_TAG_TH)) {
        report_parse_error(parser);
    }
    while (open_count(parser) > 0) {
        const uint32_t node = current_node(parser);
        pop_open_element(parser);
        if (is_html_element(parser, node, SXML_HTML_TAG_TD) || is_html_element(parser, node, SXML_HTML_TAG_TH)) {
            break;
        }
    }
    clear_formatting_to_last_marker(parser);
    parser->mode = SXML_HTML_MODE_IN_ROW;
}

/* 13.2.6.4.15 "in cell". */
static sxml_status run_in_cell(sxml_html_parser *parser, const sxml_html_tree_token *input,
                               sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(table_start_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TD, SXML_HTML_TAG_TFOOT,
             SXML_HTML_TAG_TH, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    SXML_HTML_TAG_LIST_OF(ignored_end_tags, SXML_HTML_TAG_BODY, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COL, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_HTML);
    SXML_HTML_TAG_LIST_OF(table_end_tags, SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_THEAD, SXML_HTML_TAG_TR);
    finish_step(step);
    if (is_end_tag(input, SXML_HTML_TAG_TD) || is_end_tag(input, SXML_HTML_TAG_TH)) {
        if (!has_tag_in_table_scope(parser, input->tag)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        generate_implied_end_tags(parser, SXML_HTML_TAG_OTHER);
        if (!is_html_element(parser, current_node(parser), input->tag)) {
            report_parse_error(parser);
        }
        pop_until_html_tag(parser, input->tag);
        clear_formatting_to_last_marker(parser);
        parser->mode = SXML_HTML_MODE_IN_ROW;
        return SXML_STATUS_OK;
    }
    if (is_start_tag_in(input, &table_start_tags)) {
        if (!has_tag_in_table_scope(parser, SXML_HTML_TAG_TD) && !has_tag_in_table_scope(parser, SXML_HTML_TAG_TH)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        close_cell(parser);
        reprocess(step);
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &ignored_end_tags)) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (is_end_tag_in(input, &table_end_tags)) {
        if (!has_tag_in_table_scope(parser, input->tag)) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        close_cell(parser);
        reprocess(step);
        return SXML_STATUS_OK;
    }
    use_rules(step, SXML_HTML_MODE_IN_BODY);
    return SXML_STATUS_OK;
}

/* Replaces the current template insertion mode and switches to mode, to
   reprocess the token there (13.2.6.4.16). */
static sxml_status switch_template_mode(sxml_html_parser *parser, sxml_html_insertion_mode mode,
                                        sxml_html_step *step) {
    pop_template_mode(parser);
    parser->mode = mode;
    reprocess(step);
    return push_template_mode(parser, mode);
}

/* 13.2.6.4.16 "in template". */
static sxml_status run_in_template(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                   sxml_html_step *step) {
    SXML_HTML_TAG_LIST_OF(head_tags, SXML_HTML_TAG_BASE, SXML_HTML_TAG_BASEFONT, SXML_HTML_TAG_BGSOUND, SXML_HTML_TAG_LINK, SXML_HTML_TAG_META, SXML_HTML_TAG_NOFRAMES,
             SXML_HTML_TAG_SCRIPT, SXML_HTML_TAG_STYLE, SXML_HTML_TAG_TEMPLATE, SXML_HTML_TAG_TITLE);
    SXML_HTML_TAG_LIST_OF(table_tags, SXML_HTML_TAG_CAPTION, SXML_HTML_TAG_COLGROUP, SXML_HTML_TAG_TBODY, SXML_HTML_TAG_TFOOT, SXML_HTML_TAG_THEAD);
    finish_step(step);
    switch (kind_of(input)) {
    case SXML_HTML_TOKEN_KIND_CHARACTERS:
    case SXML_HTML_TOKEN_KIND_COMMENT:
    case SXML_HTML_TOKEN_KIND_PROCESSING_INSTRUCTION:
    case SXML_HTML_TOKEN_KIND_DOCTYPE:
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    case SXML_HTML_TOKEN_KIND_START_TAG:
        if (tag_is_in(input->tag, &head_tags)) {
            use_rules(step, SXML_HTML_MODE_IN_HEAD);
            return SXML_STATUS_OK;
        }
        if (tag_is_in(input->tag, &table_tags)) {
            return switch_template_mode(parser, SXML_HTML_MODE_IN_TABLE, step);
        }
        if (input->tag == SXML_HTML_TAG_COL) {
            return switch_template_mode(parser, SXML_HTML_MODE_IN_COLUMN_GROUP, step);
        }
        if (input->tag == SXML_HTML_TAG_TR) {
            return switch_template_mode(parser, SXML_HTML_MODE_IN_TABLE_BODY, step);
        }
        if (input->tag == SXML_HTML_TAG_TD || input->tag == SXML_HTML_TAG_TH) {
            return switch_template_mode(parser, SXML_HTML_MODE_IN_ROW, step);
        }
        return switch_template_mode(parser, SXML_HTML_MODE_IN_BODY, step);
    case SXML_HTML_TOKEN_KIND_END_TAG:
        if (input->tag == SXML_HTML_TAG_TEMPLATE) {
            use_rules(step, SXML_HTML_MODE_IN_HEAD);
            return SXML_STATUS_OK;
        }
        report_parse_error(parser);
        return SXML_STATUS_OK;
    case SXML_HTML_TOKEN_KIND_END_OF_FILE:
        if (!has_template_on_stack(parser)) {
            step->action = SXML_HTML_STEP_STOP;
            return SXML_STATUS_OK;
        }
        report_parse_error(parser);
        pop_until_html_tag(parser, SXML_HTML_TAG_TEMPLATE);
        clear_formatting_to_last_marker(parser);
        pop_template_mode(parser);
        reset_insertion_mode(parser);
        reprocess(step);
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* 13.2.6.4.17 "after body" and 13.2.6.4.20 "after after body". */
static sxml_status run_after_body(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                  sxml_html_step *step) {
    const bool is_after_after = parser->mode == SXML_HTML_MODE_AFTER_AFTER_BODY;
    finish_step(step);
    if (is_whitespace_run(input) || is_start_tag(input, SXML_HTML_TAG_HTML) ||
        (is_after_after && kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE)) {
        use_rules(step, SXML_HTML_MODE_IN_BODY);
        return SXML_STATUS_OK;
    }
    if (is_comment_like(input)) {
        return insert_comment_in(parser, input->token,
                                 is_after_after ? 0 : open_element_at(parser, 0));
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (!is_after_after && is_end_tag(input, SXML_HTML_TAG_HTML)) {
        parser->mode = SXML_HTML_MODE_AFTER_AFTER_BODY;
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_OF_FILE) {
        step->action = SXML_HTML_STEP_STOP;
        return SXML_STATUS_OK;
    }
    report_parse_error(parser);
    parser->mode = SXML_HTML_MODE_IN_BODY;
    reprocess(step);
    return SXML_STATUS_OK;
}

/* 13.2.6.4.18 "in frameset", 13.2.6.4.19 "after frameset" and
   13.2.6.4.21 "after after frameset". */
static sxml_status run_frameset_modes(sxml_html_parser *parser, const sxml_html_tree_token *input,
                                      sxml_html_step *step) {
    const sxml_html_insertion_mode mode = parser->mode;
    finish_step(step);
    if (mode == SXML_HTML_MODE_AFTER_AFTER_FRAMESET) {
        if (is_comment_like(input)) {
            return insert_comment_in(parser, input->token, 0);
        }
        if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE || is_whitespace_run(input) ||
            is_start_tag(input, SXML_HTML_TAG_HTML)) {
            use_rules(step, SXML_HTML_MODE_IN_BODY);
            return SXML_STATUS_OK;
        }
    } else {
        if (is_whitespace_run(input)) {
            return insert_characters(parser, input->token->data);
        }
        if (is_comment_like(input)) {
            return insert_comment(parser, input->token);
        }
        if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
            report_parse_error(parser);
            return SXML_STATUS_OK;
        }
        if (is_start_tag(input, SXML_HTML_TAG_HTML)) {
            use_rules(step, SXML_HTML_MODE_IN_BODY);
            return SXML_STATUS_OK;
        }
    }
    if (is_start_tag(input, SXML_HTML_TAG_NOFRAMES)) {
        use_rules(step, SXML_HTML_MODE_IN_HEAD);
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_OF_FILE) {
        if (mode == SXML_HTML_MODE_IN_FRAMESET && current_node(parser) != open_element_at(parser, 0)) {
            report_parse_error(parser);
        }
        step->action = SXML_HTML_STEP_STOP;
        return SXML_STATUS_OK;
    }
    if (mode == SXML_HTML_MODE_IN_FRAMESET) {
        if (is_start_tag(input, SXML_HTML_TAG_FRAMESET)) {
            return insert_html_element(parser, input->token);
        }
        if (is_end_tag(input, SXML_HTML_TAG_FRAMESET)) {
            if (current_node(parser) == open_element_at(parser, 0)) {
                report_parse_error(parser);
                return SXML_STATUS_OK;
            }
            pop_open_element(parser);
            if (!is_html_element(parser, current_node(parser), SXML_HTML_TAG_FRAMESET)) {
                parser->mode = SXML_HTML_MODE_AFTER_FRAMESET;
            }
            return SXML_STATUS_OK;
        }
        if (is_start_tag(input, SXML_HTML_TAG_FRAME)) {
            return insert_void_element(parser, input->token);
        }
    }
    if (mode == SXML_HTML_MODE_AFTER_FRAMESET && is_end_tag(input, SXML_HTML_TAG_HTML)) {
        parser->mode = SXML_HTML_MODE_AFTER_AFTER_FRAMESET;
        return SXML_STATUS_OK;
    }
    report_parse_error(parser);
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------- Foreign content */

/* The name of node in ASCII lower case equals name. */
static bool lower_name_equals(const sxml_html_parser *parser, uint32_t node_number,
                              sxml_byte_view name) {
    return is_ascii_case_insensitive_match(tree_text(parser, node_at(parser, node_number)->name),
                                           name);
}

/* 13.2.6.5: start tags that break out of foreign content. */
static bool breaks_out_of_foreign_content(const sxml_html_tree_token *input) {
    SXML_HTML_TAG_LIST_OF(breakout_tags, SXML_HTML_TAG_B, SXML_HTML_TAG_BIG, SXML_HTML_TAG_BLOCKQUOTE, SXML_HTML_TAG_BODY, SXML_HTML_TAG_BR, SXML_HTML_TAG_CENTER, SXML_HTML_TAG_CODE,
             SXML_HTML_TAG_DD, SXML_HTML_TAG_DIV, SXML_HTML_TAG_DL, SXML_HTML_TAG_DT, SXML_HTML_TAG_EM, SXML_HTML_TAG_EMBED, SXML_HTML_TAG_H1, SXML_HTML_TAG_H2, SXML_HTML_TAG_H3, SXML_HTML_TAG_H4,
             SXML_HTML_TAG_H5, SXML_HTML_TAG_H6, SXML_HTML_TAG_HEAD, SXML_HTML_TAG_HR, SXML_HTML_TAG_I, SXML_HTML_TAG_IMG, SXML_HTML_TAG_LI, SXML_HTML_TAG_LISTING, SXML_HTML_TAG_MENU,
             SXML_HTML_TAG_META, SXML_HTML_TAG_NOBR, SXML_HTML_TAG_OL, SXML_HTML_TAG_P, SXML_HTML_TAG_PRE, SXML_HTML_TAG_RUBY, SXML_HTML_TAG_S, SXML_HTML_TAG_SMALL, SXML_HTML_TAG_SPAN,
             SXML_HTML_TAG_STRONG, SXML_HTML_TAG_STRIKE, SXML_HTML_TAG_SUB, SXML_HTML_TAG_SUP, SXML_HTML_TAG_TABLE, SXML_HTML_TAG_TT, SXML_HTML_TAG_U, SXML_HTML_TAG_UL, SXML_HTML_TAG_VAR);
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_END_TAG) {
        return input->tag == SXML_HTML_TAG_BR || input->tag == SXML_HTML_TAG_P;
    }
    if (kind_of(input) != SXML_HTML_TOKEN_KIND_START_TAG) {
        return false;
    }
    if (tag_is_in(input->tag, &breakout_tags)) {
        return true;
    }
    return input->tag == SXML_HTML_TAG_FONT &&
           (find_token_attribute(input->token, SXML_BYTE_VIEW_FROM_LITERAL("color")).is_found ||
            find_token_attribute(input->token, SXML_BYTE_VIEW_FROM_LITERAL("face")).is_found ||
            find_token_attribute(input->token, SXML_BYTE_VIEW_FROM_LITERAL("size")).is_found);
}

/* 13.2.6.5 "the rules for parsing tokens in foreign content". */
static sxml_status run_foreign_content(sxml_html_parser *parser,
                                       const sxml_html_tree_token *input, sxml_html_step *step) {
    finish_step(step);
    if (is_null_run(input)) {
        report_parse_error(parser);
        sxml_status status = SXML_STATUS_OK;
        for (size_t null_index = 0; status == SXML_STATUS_OK && null_index < input->token->data.byte_count;
             null_index += 1) {
            status = insert_characters(parser, SXML_BYTE_VIEW_FROM_LITERAL("\xEF\xBF\xBD"));
        }
        return status;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_CHARACTERS) {
        if (!is_whitespace_run(input)) {
            parser->is_frameset_ok = false;
        }
        return insert_characters(parser, input->token->data);
    }
    if (is_comment_like(input)) {
        return insert_comment(parser, input->token);
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_DOCTYPE) {
        report_parse_error(parser);
        return SXML_STATUS_OK;
    }
    if (breaks_out_of_foreign_content(input)) {
        report_parse_error(parser);
        while (open_count(parser) > 0) {
            const uint32_t node = current_node(parser);
            if (is_mathml_text_integration_point(parser, node) ||
                is_html_integration_point(parser, node) ||
                node_at(parser, node)->element_namespace == SXML_HTML_NAMESPACE_HTML) {
                break;
            }
            pop_open_element(parser);
        }
        use_rules(step, parser->mode);
        return SXML_STATUS_OK;
    }
    if (kind_of(input) == SXML_HTML_TOKEN_KIND_START_TAG) {
        const sxml_html_namespace element_namespace =
            node_at(parser, current_node(parser))->element_namespace;
        const sxml_status status = insert_element_for_token(
            parser,
            &(sxml_html_element_request){.token = input->token,
                                         .element_namespace = element_namespace},
            NULL);
        if (input->token->is_self_closing) {
            /* An SVG script element is popped as its end tag would pop it. */
            acknowledge_self_closing(parser);
            pop_open_element(parser);
        }
        return status;
    }
    /* End tags ("script" in SVG pops the current node as any other does). */
    uint32_t node = current_node(parser);
    if (!lower_name_equals(parser, node, input->token->name)) {
        report_parse_error(parser);
    }
    for (size_t position = open_count(parser); position > 0; position -= 1) {
        node = open_element_at(parser, position - 1);
        if (position == 1) {
            return SXML_STATUS_OK;
        }
        if (lower_name_equals(parser, node, input->token->name)) {
            pop_until_node(parser, node);
            return SXML_STATUS_OK;
        }
        const uint32_t previous = open_element_at(parser, position - 2);
        if (node_at(parser, previous)->element_namespace == SXML_HTML_NAMESPACE_HTML) {
            use_rules(step, parser->mode);
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- Dispatch */

/* 13.2.6, "the tree construction dispatcher": HTML content rules (the
   insertion mode) or the rules for foreign content. */
static bool uses_html_content_rules(const sxml_html_parser *parser,
                                    const sxml_html_tree_token *input) {
    if (open_count(parser) == 0) {
        return true;
    }
    const uint32_t node = current_node(parser);
    const sxml_html_node *adjusted = node_at(parser, node);
    const sxml_html_token_kind kind = kind_of(input);
    const bool is_start = kind == SXML_HTML_TOKEN_KIND_START_TAG;
    const bool is_characters = kind == SXML_HTML_TOKEN_KIND_CHARACTERS;
    if (adjusted->element_namespace == SXML_HTML_NAMESPACE_HTML) {
        return true;
    }
    if (is_mathml_text_integration_point(parser, node) &&
        ((is_start && input->tag != SXML_HTML_TAG_MGLYPH && input->tag != SXML_HTML_TAG_MALIGNMARK) || is_characters)) {
        return true;
    }
    if (adjusted->element_namespace == SXML_HTML_NAMESPACE_MATHML &&
        adjusted->tag == SXML_HTML_TAG_ANNOTATION_XML && is_start && input->tag == SXML_HTML_TAG_SVG) {
        return true;
    }
    if (is_html_integration_point(parser, node) && (is_start || is_characters)) {
        return true;
    }
    return kind == SXML_HTML_TOKEN_KIND_END_OF_FILE;
}

/* Runs the rules step->rules names; the step's outcome replaces it. */
static sxml_status run_rules(sxml_html_parser *parser, const sxml_html_tree_token *input,
                             sxml_html_step *step) {
    switch (step->rules) {
    case SXML_HTML_MODE_INITIAL:
        return run_initial(parser, input, step);
    case SXML_HTML_MODE_BEFORE_HTML:
        return run_before_html(parser, input, step);
    case SXML_HTML_MODE_BEFORE_HEAD:
        return run_before_head(parser, input, step);
    case SXML_HTML_MODE_IN_HEAD:
        return run_in_head(parser, input, step);
    case SXML_HTML_MODE_IN_HEAD_NOSCRIPT:
        return run_in_head_noscript(parser, input, step);
    case SXML_HTML_MODE_AFTER_HEAD:
        return run_after_head(parser, input, step);
    case SXML_HTML_MODE_IN_BODY:
        return run_in_body(parser, input, step);
    case SXML_HTML_MODE_TEXT:
        return run_text(parser, input, step);
    case SXML_HTML_MODE_IN_TABLE:
        return run_in_table(parser, input, step);
    case SXML_HTML_MODE_IN_TABLE_TEXT:
        return run_in_table_text(parser, input, step);
    case SXML_HTML_MODE_IN_CAPTION:
        return run_in_caption(parser, input, step);
    case SXML_HTML_MODE_IN_COLUMN_GROUP:
        return run_in_column_group(parser, input, step);
    case SXML_HTML_MODE_IN_TABLE_BODY:
        return run_in_table_body(parser, input, step);
    case SXML_HTML_MODE_IN_ROW:
        return run_in_row(parser, input, step);
    case SXML_HTML_MODE_IN_CELL:
        return run_in_cell(parser, input, step);
    case SXML_HTML_MODE_IN_TEMPLATE:
        return run_in_template(parser, input, step);
    case SXML_HTML_MODE_AFTER_BODY:
    case SXML_HTML_MODE_AFTER_AFTER_BODY:
        return run_after_body(parser, input, step);
    case SXML_HTML_MODE_IN_FRAMESET:
    case SXML_HTML_MODE_AFTER_FRAMESET:
    case SXML_HTML_MODE_AFTER_AFTER_FRAMESET:
        return run_frameset_modes(parser, input, step);
    case SXML_HTML_MODE_FOREIGN_CONTENT:
        return run_foreign_content(parser, input, step);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The most steps one token takes: every mode switch and reprocessing is
   bounded, so more means an error in sxml itself. */
#define SXML_HTML_MAXIMUM_STEPS_PER_TOKEN 64U

/* Processes one token (a character run of one class or another token)
   through the dispatcher and the steps it asks for. */
static sxml_status process_tree_token(sxml_html_parser *parser,
                                      const sxml_html_tree_token *input) {
    sxml_html_step step = {.action = SXML_HTML_STEP_REPROCESS, .rules = SXML_HTML_MODE_INITIAL};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t step_count = 0; step_count < SXML_HTML_MAXIMUM_STEPS_PER_TOKEN; step_count += 1) {
        sxml_html_insertion_mode rules = step.rules;
        if (step.action == SXML_HTML_STEP_REPROCESS) {
            rules = uses_html_content_rules(parser, input) ? parser->mode : SXML_HTML_MODE_FOREIGN_CONTENT;
        }
        step.rules = rules;
        status = run_rules(parser, input, &step);
        if (status != SXML_STATUS_OK || step.action == SXML_HTML_STEP_DONE || step.action == SXML_HTML_STEP_STOP) {
            break;
        }
    }
    parser->is_foster_parenting = false;
    if (parser->removes_head_after_token) {
        parser->removes_head_after_token = false;
        remove_open_element(parser, parser->head_element);
    }
    if (step.action == SXML_HTML_STEP_STOP) {
        parser->has_stopped = true;
    } else if (step.action != SXML_HTML_STEP_DONE && status == SXML_STATUS_OK) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

static uint8_t character_class(uint8_t byte) {
    if (byte == 0) {
        return 0;
    }
    return is_whitespace_byte(byte) ? 1U : 2U;
}

/* Splits a run of characters into runs of one class and processes each. */
static sxml_status process_characters(sxml_html_parser *parser, sxml_byte_view characters) {
    if (parser->ignores_next_line_feed) {
        parser->ignores_next_line_feed = false;
        if (characters.byte_count > 0 && characters.bytes[0] == '\n') {
            characters.bytes += 1;
            characters.byte_count -= 1;
        }
    }
    size_t start = 0;
    while (start < characters.byte_count) {
        const uint8_t run_class = character_class(characters.bytes[start]);
        size_t end = start + 1;
        while (end < characters.byte_count && character_class(characters.bytes[end]) == run_class) {
            end += 1;
        }
        sxml_html_token run_token;
        memset(&run_token, 0, sizeof(run_token));
        run_token.token_kind = SXML_HTML_TOKEN_KIND_CHARACTERS;
        run_token.data = (sxml_byte_view){.bytes = characters.bytes + start, .byte_count = end - start};
        const sxml_html_tree_token input = {.token = &run_token, .tag = SXML_HTML_TAG_OTHER};
        const sxml_status status = process_tree_token(parser, &input);
        if (status != SXML_STATUS_OK || parser->has_stopped) {
            return status;
        }
        start = end;
    }
    return SXML_STATUS_OK;
}

static sxml_status process_token(sxml_html_parser *parser, const sxml_html_token *token) {
    if (token->token_kind == SXML_HTML_TOKEN_KIND_CHARACTERS) {
        return process_characters(parser, token->data);
    }
    parser->ignores_next_line_feed = false;
    const bool is_tag = token->token_kind == SXML_HTML_TOKEN_KIND_START_TAG ||
                        token->token_kind == SXML_HTML_TOKEN_KIND_END_TAG;
    const sxml_html_tree_token input = {.token = token,
                                        .tag = is_tag ? find_tag(token->name) : SXML_HTML_TAG_OTHER};
    parser->has_acknowledged_self_closing = false;
    const sxml_status status = process_tree_token(parser, &input);
    if (token->token_kind == SXML_HTML_TOKEN_KIND_START_TAG && token->is_self_closing &&
        !parser->has_acknowledged_self_closing) {
        /* non-void-html-element-start-tag-with-trailing-solidus. */
        report_parse_error(parser);
    }
    return status;
}

/* ------------------------------------------------------------- Entry */

sxml_status sxml_html_construct_tree(const sxml_html_parse_request *request,
                                     sxml_html_working_tree *tree,
                                     sxml_html_parse_outcome *outcome) {
    *outcome = (sxml_html_parse_outcome){
        .has_parse_error = false, .changes_encoding = false, .new_encoding_number = 0};
    sxml_html_parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.encoding_number = request->encoding_number;
    parser.is_encoding_tentative = request->is_encoding_tentative;
    const sxml_allocator allocator = tree->nodes.allocator;
    sxml_html_tokenizer_initialize(&parser.tokenizer,
                                   &(sxml_html_tokenizer_configuration){.allocator = allocator,
                                                                        .input = request->input});
    parser.tree = tree;
    parser.allocator = allocator;
    parser.open_elements = sxml_xml_array_empty(
        (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(uint32_t)});
    parser.formatting_elements = sxml_xml_array_empty(
        (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(uint32_t)});
    parser.template_modes = sxml_xml_array_empty(
        (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(uint8_t)});
    parser.pending_table_text = sxml_xml_buffer_empty(allocator);
    parser.mode = SXML_HTML_MODE_INITIAL;
    parser.original_mode = SXML_HTML_MODE_INITIAL;
    parser.head_element = SXML_HTML_NO_NODE;
    parser.form_element = SXML_HTML_NO_NODE;
    parser.is_frameset_ok = true;
    parser.quirks_mode = SXML_HTML_QUIRKS_MODE_NO_QUIRKS;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && !parser.has_stopped) {
        sxml_html_token token;
        status = sxml_html_tokenizer_next(&parser.tokenizer, &token);
        if (status == SXML_STATUS_OK) {
            status = process_token(&parser, &token);
        }
        if (token.token_kind == SXML_HTML_TOKEN_KIND_END_OF_FILE) {
            break;
        }
        if (request->stops_at_parse_error &&
            (parser.has_parse_error || parser.tokenizer.has_parse_error)) {
            break;
        }
        const uint32_t adjusted = current_node(&parser);
        parser.tokenizer.allows_cdata_sections =
            adjusted != SXML_HTML_NO_NODE &&
            node_at(&parser, adjusted)->element_namespace != SXML_HTML_NAMESPACE_HTML;
    }
    outcome->has_parse_error = parser.has_parse_error || parser.tokenizer.has_parse_error;
    outcome->changes_encoding = parser.changes_encoding;
    outcome->new_encoding_number = parser.new_encoding_number;
    sxml_html_tokenizer_finalize(&parser.tokenizer);
    sxml_xml_array_release(&parser.open_elements);
    sxml_xml_array_release(&parser.formatting_elements);
    sxml_xml_array_release(&parser.template_modes);
    sxml_xml_buffer_release(&parser.pending_table_text);
    return status;
}
