#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "environment.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "xslt_reading.h"

/*
 * Environments of the XSLT 4.0 test suite, resolved (glossary:
 * xslt_test_environment; third_party/xslt40-test/admin/catalog-schema.xsd,
 * "environmentType" and the elements it holds).
 *
 * An XSLT environment holds what a QT4 environment holds (sources, params,
 * resources, collections, collations, schemas, a context item), which the
 * QT4 environment reader reads into `shared`, and besides: inline source
 * documents and their other attributes, static parameters, stylesheets
 * and packages, and an output element. Those are read here. The same
 * stylesheet, package, param and output elements also occur in <test>,
 * and the readers below serve both. Every file attribute is a suite path
 * resolved against the file that holds the element (suite_path.h).
 * Nothing is read from other files or evaluated: running a test does that
 * (xslt_test_run.h).
 */

/* What a stylesheet or package element of an environment or test is. */
typedef enum sxml_xslt_test_module_kind {
    /* <stylesheet>: a stylesheet module. */
    SXML_XSLT_TEST_MODULE_KIND_STYLESHEET,
    /* <package>: a package, used by name. */
    SXML_XSLT_TEST_MODULE_KIND_PACKAGE,
} sxml_xslt_test_module_kind;

/* The role attribute (catalog-schema.xsd, "stylesheetType" and
   "packageType"). */
typedef enum sxml_xslt_test_module_role {
    /* No role: the first such module is the principal one. */
    SXML_XSLT_TEST_MODULE_ROLE_UNSPECIFIED,
    SXML_XSLT_TEST_MODULE_ROLE_PRINCIPAL,
    /* Included, imported or used by the principal module. */
    SXML_XSLT_TEST_MODULE_ROLE_SECONDARY,
} sxml_xslt_test_module_role;

/* A <stylesheet> or <package>. */
typedef struct sxml_xslt_test_module {
    /* The suite path of the module; empty when the element has none. */
    sxml_byte_view file_path;
    /* uri=: for a package its name (an absolute URI, XSLT 4.0, 3.4), for
       a stylesheet the URI it is known by; empty for none. */
    sxml_byte_view uri;
    /* name= of a package (rare; the suite names packages by uri); empty
       for none. */
    sxml_byte_view package_name;
    /* package-version=; empty for none. */
    sxml_byte_view package_version;
    /* xml-version=; empty for none (XML 1.0). */
    sxml_byte_view xml_version;
    sxml_xslt_test_module_kind module_kind;
    sxml_xslt_test_module_role module_role;
} sxml_xslt_test_module;

typedef struct sxml_xslt_test_module_view {
    const sxml_xslt_test_module *modules;
    size_t module_count;
} sxml_xslt_test_module_view;

/* A <param>: a stylesheet parameter, a static parameter, or a parameter
   of an initial template, mode or function. */
typedef struct sxml_xslt_test_parameter {
    sxml_xslt_test_name name;
    /* An XPath expression giving the value; empty for none. */
    sxml_byte_view select;
    /* as=: the SequenceType the value is declared with; empty for none. */
    sxml_byte_view declared_type;
    /* static="yes": a static parameter (XSLT 4.0, 9.6). */
    bool is_static;
    /* tunnel="yes": a tunnel parameter of the initial template or mode. */
    bool is_tunnel;
} sxml_xslt_test_parameter;

typedef struct sxml_xslt_test_parameter_view {
    const sxml_xslt_test_parameter *parameters;
    size_t parameter_count;
} sxml_xslt_test_parameter_view;

/* What a <source> has beyond its QT4 attributes (role, file, uri,
   validation). */
typedef struct sxml_xslt_test_source_detail {
    /* <content>: the document given inline, its text joined. */
    bool has_content;
    sxml_byte_view content;
    /* select=: with role ".", an expression selecting the global context
       item within the document; empty for none. */
    sxml_byte_view select;
    /* xml-version=; empty for none. */
    sxml_byte_view xml_version;
    /* streaming="true": the document is read by a streamed construct;
       run unstreamed (D97). */
    bool is_streamed;
    /* dtd="true": the document's DTD gives attribute types. */
    bool needs_dtd;
    /* defines-stylesheet="true": the document holds or names the
       stylesheet through an xml-stylesheet processing instruction. */
    bool defines_stylesheet;
    /* xinclude="true": XInclude processing is expected. */
    bool needs_xinclude;
} sxml_xslt_test_source_detail;

typedef struct sxml_xslt_test_source_detail_view {
    const sxml_xslt_test_source_detail *source_details;
    size_t source_detail_count;
} sxml_xslt_test_source_detail_view;

/* An <output> (catalog-schema.xsd, "output"). */
typedef struct sxml_xslt_test_output {
    bool is_present;
    /* serialize="yes": the principal result is serialized (default no). */
    bool is_serialized;
    /* tree= given: the raw result or a tree, when not serialized;
       otherwise the build-tree default of the output definition. */
    bool has_tree_choice;
    bool builds_tree;
    /* well-formed= (default yes). */
    bool is_well_formed;
    /* file=: the suite path the base output URI names; empty for none. */
    sxml_byte_view file_path;
    /* result-var=: with tree="no", the variable the raw result is bound
       to in assertions; empty for none. */
    sxml_byte_view result_variable;
} sxml_xslt_test_output;

/* The absent output element. */
sxml_xslt_test_output sxml_xslt_test_output_absent(void);

typedef struct sxml_xslt_test_environment {
    /* Read by the QT4 environment reader. Its parameters are not used:
       `parameters` holds them with their XSLT attributes. */
    sxml_qt4_environment shared;
    /* One per shared.sources entry, in the same order. */
    sxml_xslt_test_source_detail_view source_details;
    sxml_xslt_test_parameter_view parameters;
    /* Stylesheets and packages, in document order. */
    sxml_xslt_test_module_view modules;
    sxml_xslt_test_output output;
} sxml_xslt_test_environment;

/* The environment of a test case with no environment element. */
sxml_xslt_test_environment sxml_xslt_test_environment_absent(void);

/*
 * Reads the environment element request names into *environment, with
 * the QT4 reader for the shared part. Returns the failures of
 * sxml_qt4_environment_read and of the readers below; *environment is
 * then untouched.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_environment_read(const sxml_qt4_environment_request *request,
                                sxml_qt4_problem *problem,
                                sxml_xslt_test_environment *environment);

/*
 * Reads the <stylesheet> and <package> children of parent, in document
 * order. Returns the tree's and the arena's failures.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_read_modules(const sxml_xslt_test_element_reader *reader,
                            sxml_node_index parent,
                            sxml_xslt_test_module_view *modules);

/*
 * Reads the <param> children of parent. Returns the failures of
 * sxml_xslt_test_read_name, the tree's and the arena's.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_read_parameters(const sxml_xslt_test_element_reader *reader,
                               sxml_node_index parent,
                               sxml_xslt_test_parameter_view *parameters);

/*
 * Reads the first <output> child of parent into *output, or the absent
 * output when there is none. Returns the tree's and the arena's failures.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_read_output(const sxml_xslt_test_element_reader *reader,
                           sxml_node_index parent,
                           sxml_xslt_test_output *output);
