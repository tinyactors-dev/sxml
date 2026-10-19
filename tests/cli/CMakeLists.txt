# Tests of the CLI framework and the sxml program (docs/CLI.md, D44), added
# by apps/CMakeLists.txt.

# Unit tests: tests/cli/<module>_test.c, one executable each, registered as
# ctest test <module> (as sxml_add_test does for the library, D16).
foreach(cli_module cli_output cli_suggestion cli_arguments cli_run)
    add_executable(${cli_module}_test "${cli_module}_test.c")
    target_link_libraries(${cli_module}_test PRIVATE sxml_program sxml_test_runner)
    sxml_apply_project_flags(${cli_module}_test)
    add_test(NAME ${cli_module} COMMAND ${cli_module}_test)
endforeach()

# End-to-end tests of the sxml executable. Each test runs
# `sxml ARGUMENT...` and compares standard output, standard error and the exit
# code with tests/cli/expected/<golden>.stdout and .stderr (a missing file
# means "nothing written"). Regenerate goldens after an intended change with
#   SXML_UPDATE_GOLDEN=1 ctest --preset debug -R '^cli/'
# and review the diff.

# sxml_add_cli_test(<name> EXIT_CODE <code> [GOLDEN <golden>]
#                   [WORKSPACE [FILES <input>...] [EXISTING <file>...]]
#                   [ARGUMENTS <argument>...])
# Arguments are passed verbatim; empty arguments are not supported here (the
# unit tests in tests/cli_arguments_test.c cover them). The program runs in
# tests/cli/input/, so file arguments are relative names there: the same
# under WASI, whose only visible directory is the working directory.
#
# WORKSPACE runs the program in an empty directory of its own instead, for
# commands that write files (--output, --result-directory, --allow-put):
# the FILES of tests/cli/input/ are copied into it, each EXISTING file is
# made with the text "old" first, and after the run every file in it but
# the copied inputs that are unchanged, with its contents, is compared with
# expected/<golden>.files (so a temporary file left behind fails the test).
function(sxml_add_cli_test name)
    cmake_parse_arguments(PARSE_ARGV 1 cli "WORKSPACE" "EXIT_CODE;GOLDEN" "ARGUMENTS;FILES;EXISTING")
    if(NOT DEFINED cli_EXIT_CODE)
        message(FATAL_ERROR "sxml_add_cli_test(${name}): EXIT_CODE is required")
    endif()
    if(NOT cli_GOLDEN)
        set(cli_GOLDEN "${name}")
    endif()
    set(argument_text "")
    foreach(argument IN LISTS cli_ARGUMENTS)
        string(APPEND argument_text " [==[${argument}]==]")
    endforeach()
    set(emulator_text "")
    foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        string(APPEND emulator_text " [==[${emulator_part}]==]")
    endforeach()
    set(golden_base "${CMAKE_CURRENT_SOURCE_DIR}/expected/${cli_GOLDEN}")
    set(workspace_text "")
    if(cli_WORKSPACE)
        set(input_text "")
        foreach(input IN LISTS cli_FILES)
            string(APPEND input_text " [==[${input}]==]")
        endforeach()
        set(existing_text "")
        foreach(existing IN LISTS cli_EXISTING)
            string(APPEND existing_text " [==[${existing}]==]")
        endforeach()
        set(workspace_text
"set(WORKSPACE_DIRECTORY [==[${CMAKE_CURRENT_BINARY_DIR}/workspace/${name}]==])
set(WORKSPACE_INPUTS${input_text})
set(WORKSPACE_EXISTING${existing_text})
set(EXPECTED_FILES_FILE [==[${golden_base}.files]==])
")
    endif()
    set(script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${name}.cmake")
    file(GENERATE OUTPUT "${script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_cli>]==])
set(EMULATOR${emulator_text})
set(ARGUMENTS${argument_text})
set(EXPECTED_EXIT_CODE ${cli_EXIT_CODE})
set(EXPECTED_STDOUT_FILE [==[${golden_base}.stdout]==])
set(EXPECTED_STDERR_FILE [==[${golden_base}.stderr]==])
set(WORKING_DIRECTORY [==[${CMAKE_CURRENT_SOURCE_DIR}/input]==])
${workspace_text}include([==[${CMAKE_CURRENT_SOURCE_DIR}/run-cli-test.cmake]==])
")
    add_test(NAME cli/${name} COMMAND "${CMAKE_COMMAND}" -P "${script}")
endfunction()

# Help on every level (docs/CLI.md, "Syntax").
sxml_add_cli_test(help_long_option EXIT_CODE 0 GOLDEN program_help ARGUMENTS --help)
sxml_add_cli_test(help_short_option EXIT_CODE 0 GOLDEN program_help ARGUMENTS -h)
sxml_add_cli_test(help_command EXIT_CODE 0 GOLDEN program_help ARGUMENTS help)
sxml_add_cli_test(help_wins_over_rest EXIT_CODE 0 GOLDEN program_help ARGUMENTS --help --bogus)
sxml_add_cli_test(help_for_version EXIT_CODE 0 GOLDEN version_help ARGUMENTS help version)
sxml_add_cli_test(version_help_option EXIT_CODE 0 GOLDEN version_help ARGUMENTS version --help)
sxml_add_cli_test(version_help_after_error EXIT_CODE 0 GOLDEN version_help ARGUMENTS version --bogus -h)
sxml_add_cli_test(help_for_help EXIT_CODE 0 ARGUMENTS help help)

# The version, three ways.
sxml_add_cli_test(version_option EXIT_CODE 0 GOLDEN version_text ARGUMENTS --version)
sxml_add_cli_test(version_command EXIT_CODE 0 GOLDEN version_text ARGUMENTS version)
sxml_add_cli_test(version_format_text EXIT_CODE 0 GOLDEN version_text ARGUMENTS version --format=text)
sxml_add_cli_test(version_format_json EXIT_CODE 0 GOLDEN version_json ARGUMENTS version --format=json)
sxml_add_cli_test(version_format_separate_value EXIT_CODE 0 GOLDEN version_json ARGUMENTS version --format json)
sxml_add_cli_test(version_after_double_dash EXIT_CODE 0 GOLDEN version_text ARGUMENTS -- version)

# Usage errors: exit code 2, diagnostics on standard error only.
sxml_add_cli_test(no_arguments EXIT_CODE 2)
sxml_add_cli_test(unknown_command EXIT_CODE 2 ARGUMENTS verison)
sxml_add_cli_test(unknown_command_without_suggestion EXIT_CODE 2 ARGUMENTS transmogrify)
sxml_add_cli_test(unknown_program_option EXIT_CODE 2 ARGUMENTS --verison)
sxml_add_cli_test(version_option_with_value EXIT_CODE 2 ARGUMENTS --version=1)
sxml_add_cli_test(version_option_with_command EXIT_CODE 2 ARGUMENTS --version version)
sxml_add_cli_test(unknown_option EXIT_CODE 2 ARGUMENTS version --fromat=json)
sxml_add_cli_test(unknown_short_option EXIT_CODE 2 ARGUMENTS version -x)
sxml_add_cli_test(abbreviated_option EXIT_CODE 2 ARGUMENTS version --form=json)
sxml_add_cli_test(invalid_choice EXIT_CODE 2 ARGUMENTS version --format=JSON)
sxml_add_cli_test(missing_value EXIT_CODE 2 ARGUMENTS version --format)
sxml_add_cli_test(repeated_option EXIT_CODE 2 ARGUMENTS version --format=json --format=text)
sxml_add_cli_test(unexpected_argument EXIT_CODE 2 ARGUMENTS version extra)
sxml_add_cli_test(standard_input_is_an_argument EXIT_CODE 2 ARGUMENTS version -)
sxml_add_cli_test(help_unknown_command EXIT_CODE 2 ARGUMENTS help verison)
sxml_add_cli_test(help_extra_argument EXIT_CODE 2 ARGUMENTS help version extra)

# sxml check (apps/sxml/README.md): files in tests/cli/input/.
sxml_add_cli_test(check_well_formed EXIT_CODE 0 ARGUMENTS check well-formed.xml)
sxml_add_cli_test(check_encodings EXIT_CODE 0 ARGUMENTS check utf-16.xml latin-1.xml)
sxml_add_cli_test(check_not_well_formed EXIT_CODE 1 ARGUMENTS check well-formed.xml mismatched-tag.xml unbound-prefix.xml)
sxml_add_cli_test(check_json EXIT_CODE 1 ARGUMENTS check --format=json well-formed.xml mismatched-tag.xml)
sxml_add_cli_test(check_missing_file EXIT_CODE 3 ARGUMENTS check missing.xml well-formed.xml)
sxml_add_cli_test(check_missing_file_json EXIT_CODE 3 ARGUMENTS check --format json missing.xml)
sxml_add_cli_test(check_entity_limit EXIT_CODE 4 ARGUMENTS check billion-laughs.xml)
sxml_add_cli_test(check_no_file EXIT_CODE 2 ARGUMENTS check)
sxml_add_cli_test(check_help EXIT_CODE 0 ARGUMENTS check --help)
# sxml check with the DTD (D199, D44): files in tests/cli/input/dtd/.
sxml_add_cli_test(check_external_not_read EXIT_CODE 0 ARGUMENTS check dtd/article.xml dtd/broken-entity.xml)
sxml_add_cli_test(check_external_entities EXIT_CODE 1 ARGUMENTS check --external-entities dtd/article.xml dtd/broken-entity.xml)
sxml_add_cli_test(check_validate_dtd EXIT_CODE 1 ARGUMENTS check --validate-dtd --external-entities dtd/article.xml dtd/invalid.xml)
sxml_add_cli_test(check_validate_dtd_json EXIT_CODE 1 ARGUMENTS check --validate-dtd --external-entities --format=json dtd/article.xml dtd/invalid.xml well-formed.xml)
sxml_add_cli_test(check_validate_dtd_internal_only EXIT_CODE 1 ARGUMENTS check --validate-dtd dtd/article.xml)

# sxml xpath (apps/sxml/README.md): files in tests/cli/input/.
sxml_add_cli_test(xpath_sequence EXIT_CODE 0 ARGUMENTS xpath "1 to 3, 'four', 5.0, 6e0, [7], {'k': 8}")
sxml_add_cli_test(xpath_empty EXIT_CODE 0 ARGUMENTS xpath "()")
sxml_add_cli_test(xpath_context_item EXIT_CODE 0 ARGUMENTS xpath --context-item=books.xml "//title/string(), count(//book)")
sxml_add_cli_test(xpath_nodes EXIT_CODE 0 ARGUMENTS xpath --context-item books.xml "/catalog/book[1]")
sxml_add_cli_test(xpath_param EXIT_CODE 0 ARGUMENTS xpath --param=n=5 --param=name=sxml "xs:integer($n) * 2, $name, $n instance of xs:untypedAtomic")
sxml_add_cli_test(xpath_doc EXIT_CODE 0 ARGUMENTS xpath "doc('books.xml')//book[2]/title/string(), doc('books.xml') is doc('books.xml')")
sxml_add_cli_test(xpath_doc_missing EXIT_CODE 1 ARGUMENTS xpath "doc('missing.xml')")
sxml_add_cli_test(xpath_static_error EXIT_CODE 1 ARGUMENTS xpath "1 + $undeclared")
sxml_add_cli_test(xpath_syntax_error EXIT_CODE 1 ARGUMENTS xpath "1 +")
# fn:parse-html: the HTML parser feature the sxml command names (packet 7.10a).
sxml_add_cli_test(xpath_parse_html EXIT_CODE 0 ARGUMENTS xpath "parse-html('<title>T</title><ul><li>one<li>two</ul><svg viewbox=\"0 0 1 1\"/>')")
sxml_add_cli_test(xpath_parse_html_error EXIT_CODE 1 ARGUMENTS xpath "parse-html('<p>x</q>', {'fail-on-error': true()})")
# fn:invisible-xml: the Invisible XML feature the sxml command names (packet 7.18, D215).
sxml_add_cli_test(xpath_invisible_xml EXIT_CODE 0 ARGUMENTS xpath "invisible-xml(\"date: year, -'-', month. year: ['0'-'9']+. @month: ['0'-'9']+.\")('2026-10')")
sxml_add_cli_test(xquery_invisible_xml_failed EXIT_CODE 0 ARGUMENTS xquery "--expression=invisible-xml(\"s: 'a', 'b'.\")('ac')")
sxml_add_cli_test(xpath_invisible_xml_error EXIT_CODE 1 ARGUMENTS xpath "invisible-xml(\"s: 'a'.\", {'fail-on-error': true()})('b')")
# Relative names resolve against the working directory, the static base URI
# of the query commands (packet 7.10b): fn:html-doc and fn:unparsed-text.
sxml_add_cli_test(xpath_relative_html_doc EXIT_CODE 0 ARGUMENTS xpath "string-join((html-doc('page.html')//*:p ! normalize-space(), unparsed-text('note.txt') => normalize-space()), ' ')")
sxml_add_cli_test(xquery_relative_html_doc EXIT_CODE 0 ARGUMENTS xquery --method=text "--expression=html-doc('page.html')//*:title ! string(), ends-with(static-base-uri(), '/'), unparsed-text-lines('note.txt')")
sxml_add_cli_test(xpath_dynamic_error EXIT_CODE 1 ARGUMENTS xpath "error(QName('http://www.w3.org/2005/xqt-errors', 'err:FOER0000'), 'stop here')")
sxml_add_cli_test(xpath_type_error EXIT_CODE 1 ARGUMENTS xpath "(1, 2) eq 1")
sxml_add_cli_test(xpath_no_focus EXIT_CODE 1 ARGUMENTS xpath "count(//x)")
sxml_add_cli_test(xpath_context_not_well_formed EXIT_CODE 1 ARGUMENTS xpath --context-item=mismatched-tag.xml ".")
sxml_add_cli_test(xpath_context_missing EXIT_CODE 3 ARGUMENTS xpath --context-item=missing.xml ".")
sxml_add_cli_test(xpath_bad_param EXIT_CODE 2 ARGUMENTS xpath --param=novalue "1")
sxml_add_cli_test(xpath_bad_method EXIT_CODE 2 ARGUMENTS xpath --method=pdf "1")
sxml_add_cli_test(xpath_method_xml EXIT_CODE 0 ARGUMENTS xpath --method=xml --context-item=books.xml "//book[1]/title, 1, 2")
sxml_add_cli_test(xpath_method_json EXIT_CODE 0 ARGUMENTS xpath --method=json "map { 'a': [1, true(), 'x'] }")
sxml_add_cli_test(xpath_method_text EXIT_CODE 0 ARGUMENTS xpath --method=text --context-item=books.xml "//title")
sxml_add_cli_test(xpath_method_html EXIT_CODE 0 ARGUMENTS xpath --method=html "parse-xml('<p>a<br/></p>')")
sxml_add_cli_test(xpath_serialization_error EXIT_CODE 1 ARGUMENTS xpath --method=json "(1, 2)")
sxml_add_cli_test(xpath_no_expression EXIT_CODE 2 ARGUMENTS xpath)
sxml_add_cli_test(xpath_negative_after_double_dash EXIT_CODE 0 ARGUMENTS xpath -- "-1 - 1")
sxml_add_cli_test(xpath_help EXIT_CODE 0 ARGUMENTS xpath --help)
# EXPath Binary, always there, and EXPath File with --allow-file-access
# only (packets 7.8, 7.9; D184, D198, D200).
sxml_add_cli_test(xpath_binary EXIT_CODE 0 ARGUMENTS xpath "bin:hex('CAFE') => bin:to-octets(), string(bin:pack-integer(1000, 2))")
# The timezone database over the host's /usr/share/zoneinfo (packet 7.16,
# D209): a zone's offset, its abbreviation, a country's zone.
sxml_add_cli_test(xpath_timezone EXIT_CODE 0 ARGUMENTS xpath "civil-timezone(xs:dateTime('2024-11-05T12:00:00Z'), 'Europe/Paris'), format-dateTime(xs:dateTime('2015-08-15T12:00:00Z'), '[H01]:[m01] [ZN]', (), (), 'America/New_York'), format-dateTime(xs:dateTime('2018-07-01T12:00:00-05:00'), '[ZN]', (), (), 'us')")
# UCA collations over the DUCET the program embeds (packet 7.15a, D213):
# numeric order, caseFirst, and fallback=no, which only the table serves
# (";" is CMake's list separator, so the URIs build it).
sxml_add_cli_test(xpath_uca_collation EXIT_CODE 0 ARGUMENTS xpath "let $uca := 'http://www.w3.org/2013/collation/UCA?fallback=no' || codepoints-to-string(59) return (compare('Chap2', 'Chap10', $uca || 'numeric=yes'), sort(('b', 'B', 'a', 'A'), $uca || 'caseFirst=upper'))")
# Tailorings and reordering over the CLDR files the command embeds (D220):
# Spanish n-tilde, German phonebook, Canadian French accents, Greek first.
sxml_add_cli_test(xpath_uca_tailoring EXIT_CODE 0 ARGUMENTS xpath "let $uca := 'http://www.w3.org/2013/collation/UCA?fallback=no' || codepoints-to-string(59) return (compare(codepoints-to-string(241) || 'a', 'nz', $uca || 'lang=es'), compare(codepoints-to-string(241) || 'a', 'nz', $uca), compare(codepoints-to-string(228), 'ae', $uca || 'lang=de-u-co-phonebk' || codepoints-to-string(59) || 'strength=primary'), compare('p' || codepoints-to-string(234) || 'che', 'p' || codepoints-to-string(233) || 'ch' || codepoints-to-string(233), $uca || 'lang=fr-CA' || codepoints-to-string(59) || 'strength=secondary'), compare(codepoints-to-string(945), 'a', $uca || 'reorder=Grek'), collation-available($uca || 'lang=se'))")
sxml_add_cli_test(transform_sort_lang EXIT_CODE 0 ARGUMENTS transform xslt/sort-lang.xsl books.xml)
# fn:transform, one of the optional features the query commands name (D203).
sxml_add_cli_test(xquery_transform EXIT_CODE 0 ARGUMENTS xquery "--expression=transform({'stylesheet-location': 'xslt/modes.xsl', 'source-node': doc('books.xml'), 'initial-mode': QName('', 'titles'), 'delivery-format': 'serialized'})?output")
sxml_add_cli_test(xpath_file_access EXIT_CODE 0 ARGUMENTS xpath --allow-file-access "file:exists('books.xml'), file:is-dir('xslt'), file:is-file('xslt'), file:read-text-lines('books.xml')[1]")
sxml_add_cli_test(xpath_file_access_denied EXIT_CODE 1 ARGUMENTS xpath "file:exists('books.xml')")
sxml_add_cli_test(xquery_file_write EXIT_CODE 0 ARGUMENTS xquery --allow-file-access --method=adaptive "--expression=let $file := file:create-temp-file('sxml-cli-', '.txt') return (file:write-text($file, codepoints-to-string((99, 97, 102, 233))), file:read-text($file), file:size($file), file:delete($file), file:exists($file))")

# sxml xquery (apps/sxml/README.md): query files in tests/cli/input/.
sxml_add_cli_test(xquery_file_html EXIT_CODE 0 ARGUMENTS xquery --context-item=books.xml titles.xq)
sxml_add_cli_test(xquery_param EXIT_CODE 0 ARGUMENTS xquery --context-item=books.xml --param=heading=Catalog titles.xq)
sxml_add_cli_test(xquery_method_overrides EXIT_CODE 0 ARGUMENTS xquery --context-item=books.xml --method=xml titles.xq)
sxml_add_cli_test(xquery_file_json EXIT_CODE 0 ARGUMENTS xquery summary.xq)
sxml_add_cli_test(xquery_expression_xml EXIT_CODE 0 ARGUMENTS xquery "--expression=for $i in 1 to 3 return <i n='{$i}'/>")
sxml_add_cli_test(xquery_expression_text EXIT_CODE 0 ARGUMENTS xquery --method=text "--expression=string-join(('a', 'b'), '-')")
sxml_add_cli_test(xquery_external_param EXIT_CODE 0 ARGUMENTS xquery --param=n=5 double.xq)
sxml_add_cli_test(xquery_external_missing EXIT_CODE 1 ARGUMENTS xquery double.xq)
sxml_add_cli_test(xquery_static_error EXIT_CODE 1 ARGUMENTS xquery broken.xq)
sxml_add_cli_test(xquery_output_declaration_error EXIT_CODE 1 ARGUMENTS xquery unsupported-option.xq)
sxml_add_cli_test(xquery_syntax_error EXIT_CODE 1 ARGUMENTS xquery "--expression=for $x in")
sxml_add_cli_test(xquery_dynamic_error EXIT_CODE 1 ARGUMENTS xquery "--expression=1 div 0")
sxml_add_cli_test(xquery_missing_file EXIT_CODE 3 ARGUMENTS xquery missing.xq)
sxml_add_cli_test(xquery_module_import EXIT_CODE 0 ARGUMENTS xquery --method=text area.xq)
sxml_add_cli_test(xquery_module_error EXIT_CODE 1 ARGUMENTS xquery import-broken.xq)
sxml_add_cli_test(xquery_module_missing EXIT_CODE 1 ARGUMENTS xquery import-missing.xq)
sxml_add_cli_test(xquery_both_texts EXIT_CODE 2 ARGUMENTS xquery --expression=1 titles.xq)
sxml_add_cli_test(xquery_no_text EXIT_CODE 2 ARGUMENTS xquery)
sxml_add_cli_test(xquery_help EXIT_CODE 0 ARGUMENTS xquery --help)
# An updating query writes the context document as it changed (D187).
sxml_add_cli_test(xquery_update EXIT_CODE 0 ARGUMENTS xquery --context-item=books.xml "--expression=delete node //book[2], insert node <book id='b3'/> into /catalog, rename node /catalog as 'books'")
sxml_add_cli_test(xquery_update_error EXIT_CODE 1 ARGUMENTS xquery --context-item=books.xml "--expression=rename node /catalog as 'a', rename node /catalog as 'b'")

# sxml transform (apps/sxml/README.md): stylesheets in tests/cli/input/xslt/.
sxml_add_cli_test(transform_apply_templates EXIT_CODE 0 ARGUMENTS transform xslt/books.xsl books.xml)
sxml_add_cli_test(transform_param EXIT_CODE 0 ARGUMENTS transform --param=heading=Catalog xslt/books.xsl books.xml)
sxml_add_cli_test(transform_method EXIT_CODE 0 ARGUMENTS transform --method=text xslt/books.xsl books.xml)
sxml_add_cli_test(transform_initial_template EXIT_CODE 0 ARGUMENTS transform xslt/hello.xsl)
sxml_add_cli_test(transform_named_template EXIT_CODE 0 ARGUMENTS transform --initial-template=numbers xslt/hello.xsl)
sxml_add_cli_test(transform_template_param EXIT_CODE 0 ARGUMENTS transform --param=who=sxml xslt/hello.xsl)
sxml_add_cli_test(transform_result_document EXIT_CODE 0 ARGUMENTS transform xslt/modes.xsl books.xml)
sxml_add_cli_test(transform_initial_mode EXIT_CODE 0 ARGUMENTS transform --initial-mode=titles xslt/modes.xsl books.xml)
sxml_add_cli_test(transform_required_param EXIT_CODE 0 ARGUMENTS transform --param=count=21 xslt/required.xsl)
# A version="1.0" stylesheet: XSLT 1.0 behaviour (packet 7.4, D197).
sxml_add_cli_test(transform_version_1 EXIT_CODE 0 ARGUMENTS transform xslt/version1.xsl books.xml)
# xsl:evaluate of an expression given on the command line (packet 7.3, D196).
sxml_add_cli_test(transform_evaluate EXIT_CODE 0 ARGUMENTS transform "--param=expression=sum(//*:price) * (1 + $tax)" xslt/evaluate.xsl books.xml)
sxml_add_cli_test(transform_missing_param EXIT_CODE 1 ARGUMENTS transform xslt/required.xsl)
sxml_add_cli_test(transform_static_error EXIT_CODE 1 ARGUMENTS transform xslt/broken.xsl books.xml)
sxml_add_cli_test(transform_terminate EXIT_CODE 1 ARGUMENTS transform xslt/terminate.xsl)
sxml_add_cli_test(transform_parent_document EXIT_CODE 0 ARGUMENTS transform xslt/parent-document.xsl)
sxml_add_cli_test(transform_dynamic_error EXIT_CODE 1 ARGUMENTS transform xslt/dynamic-error.xsl)
sxml_add_cli_test(transform_no_initial_template EXIT_CODE 1 ARGUMENTS transform xslt/books.xsl)
sxml_add_cli_test(transform_unknown_mode EXIT_CODE 1 ARGUMENTS transform --initial-mode=nothing xslt/books.xsl books.xml)
sxml_add_cli_test(transform_not_well_formed EXIT_CODE 1 ARGUMENTS transform xslt/books.xsl mismatched-tag.xml)
sxml_add_cli_test(transform_stylesheet_not_well_formed EXIT_CODE 1 ARGUMENTS transform xslt/not-well-formed.xsl books.xml)
sxml_add_cli_test(transform_missing_stylesheet EXIT_CODE 3 ARGUMENTS transform missing.xsl books.xml)
sxml_add_cli_test(transform_mode_without_input EXIT_CODE 2 ARGUMENTS transform --initial-mode=titles xslt/modes.xsl)
sxml_add_cli_test(transform_bad_name EXIT_CODE 2 ARGUMENTS transform --initial-template=a:b xslt/hello.xsl)
sxml_add_cli_test(transform_no_stylesheet EXIT_CODE 2 ARGUMENTS transform)
sxml_add_cli_test(transform_help EXIT_CODE 0 ARGUMENTS transform --help)

# sxml validate (apps/sxml/README.md): schemas and documents in tests/cli/input/xsd/.
sxml_add_cli_test(validate_valid EXIT_CODE 0 ARGUMENTS validate --schema=xsd/books.xsd xsd/books.xml)
sxml_add_cli_test(validate_invalid EXIT_CODE 1 ARGUMENTS validate --schema=xsd/books.xsd xsd/books.xml xsd/bad-books.xml)
sxml_add_cli_test(validate_json EXIT_CODE 1 ARGUMENTS validate --format=json --schema=xsd/books.xsd xsd/books.xml xsd/bad-books.xml)
sxml_add_cli_test(validate_include EXIT_CODE 1 ARGUMENTS validate --schema=xsd/library.xsd xsd/books.xml xsd/negative-price.xml)
sxml_add_cli_test(validate_import EXIT_CODE 1 ARGUMENTS validate --schema=xsd/offers.xsd xsd/offers.xml)
sxml_add_cli_test(validate_strict_undeclared EXIT_CODE 1 ARGUMENTS validate --schema=xsd/books.xsd xsd/shelf.xml)
sxml_add_cli_test(validate_lax EXIT_CODE 1 ARGUMENTS validate --mode=lax --schema=xsd/books.xsd xsd/shelf.xml xsd/bad-shelf.xml)
sxml_add_cli_test(validate_lax_not_known EXIT_CODE 0 ARGUMENTS validate --mode=lax --schema=xsd/books.xsd xsd/shelf.xml)
sxml_add_cli_test(validate_lax_json EXIT_CODE 0 ARGUMENTS validate --mode=lax --format=json --schema=xsd/books.xsd xsd/shelf.xml)
sxml_add_cli_test(validate_schema_error EXIT_CODE 1 ARGUMENTS validate --schema=xsd/broken.xsd xsd/books.xml)
sxml_add_cli_test(validate_schema_error_json EXIT_CODE 1 ARGUMENTS validate --format=json --schema=xsd/broken.xsd xsd/books.xml)
sxml_add_cli_test(validate_schema_error_in_include EXIT_CODE 1 ARGUMENTS validate --schema=xsd/broken-include.xsd xsd/books.xml)
sxml_add_cli_test(validate_missing_include EXIT_CODE 1 ARGUMENTS validate --schema=xsd/missing-include.xsd xsd/books.xml)
sxml_add_cli_test(validate_two_schemas EXIT_CODE 1 ARGUMENTS validate --schema=xsd/books.xsd --schema=xsd/library.xsd xsd/books.xml)
sxml_add_cli_test(validate_schema_not_well_formed EXIT_CODE 1 ARGUMENTS validate --schema=xsd/unclosed.xml xsd/books.xml)
sxml_add_cli_test(validate_not_well_formed EXIT_CODE 1 ARGUMENTS validate --schema=xsd/books.xsd xsd/unclosed.xml xsd/books.xml)
sxml_add_cli_test(validate_missing_file EXIT_CODE 3 ARGUMENTS validate --schema=xsd/books.xsd missing.xml xsd/books.xml)
sxml_add_cli_test(validate_missing_file_json EXIT_CODE 3 ARGUMENTS validate --format=json --schema=xsd/books.xsd missing.xml)
sxml_add_cli_test(validate_missing_schema EXIT_CODE 3 ARGUMENTS validate --schema=missing.xsd xsd/books.xml)
sxml_add_cli_test(validate_missing_schema_json EXIT_CODE 3 ARGUMENTS validate --format=json --schema=missing.xsd xsd/books.xml)
sxml_add_cli_test(validate_entity_limit EXIT_CODE 4 ARGUMENTS validate --schema=xsd/books.xsd billion-laughs.xml)
sxml_add_cli_test(validate_no_schema EXIT_CODE 2 ARGUMENTS validate xsd/books.xml)
sxml_add_cli_test(validate_no_file EXIT_CODE 2 ARGUMENTS validate --schema=xsd/books.xsd)
sxml_add_cli_test(validate_standard_input_twice EXIT_CODE 2 ARGUMENTS validate --schema=- -)
sxml_add_cli_test(validate_bad_mode EXIT_CODE 2 ARGUMENTS validate --mode=loose --schema=xsd/books.xsd xsd/books.xml)
sxml_add_cli_test(validate_help EXIT_CODE 0 ARGUMENTS validate --help)

# sxml serialize (apps/sxml/README.md).
sxml_add_cli_test(serialize_xml EXIT_CODE 0 ARGUMENTS serialize books.xml)
sxml_add_cli_test(serialize_indent EXIT_CODE 0 ARGUMENTS serialize --indent books.xml)
sxml_add_cli_test(serialize_text EXIT_CODE 0 ARGUMENTS serialize --method=text books.xml)
sxml_add_cli_test(serialize_not_well_formed EXIT_CODE 1 ARGUMENTS serialize mismatched-tag.xml)
sxml_add_cli_test(serialize_missing EXIT_CODE 3 ARGUMENTS serialize missing.xml)
sxml_add_cli_test(serialize_help EXIT_CODE 0 ARGUMENTS serialize --help)

# --output and --force (docs/CLI.md, "No hidden side effects"; packet
# cli-2): the result goes to FILE only when the command succeeds, through a
# temporary file that leaves nothing behind, and never over an existing
# file without --force.
sxml_add_cli_test(xpath_output EXIT_CODE 0 WORKSPACE ARGUMENTS xpath --output=numbers.txt "1 to 3")
sxml_add_cli_test(xpath_output_short EXIT_CODE 0 WORKSPACE FILES books.xml ARGUMENTS xpath -o titles.txt --context-item=books.xml "//title/string()")
sxml_add_cli_test(xpath_output_exists EXIT_CODE 3 WORKSPACE EXISTING numbers.txt ARGUMENTS xpath --output=numbers.txt "1 to 3")
sxml_add_cli_test(xpath_output_force EXIT_CODE 0 WORKSPACE EXISTING numbers.txt ARGUMENTS xpath --force --output=numbers.txt "1 to 3")
sxml_add_cli_test(xpath_output_error EXIT_CODE 1 WORKSPACE ARGUMENTS xpath --output=numbers.txt "1, 1 div 0")
sxml_add_cli_test(xpath_output_error_keeps_file EXIT_CODE 1 WORKSPACE EXISTING numbers.txt ARGUMENTS xpath --force --output=numbers.txt "1 div 0")
sxml_add_cli_test(xpath_output_no_directory EXIT_CODE 3 WORKSPACE ARGUMENTS xpath --output=missing/numbers.txt "1")
sxml_add_cli_test(xpath_output_directory EXIT_CODE 3 WORKSPACE FILES xslt/books.xsl ARGUMENTS xpath --force --output=xslt "1")
sxml_add_cli_test(xpath_output_standard_output EXIT_CODE 2 ARGUMENTS xpath --output=- "1")
sxml_add_cli_test(xpath_force_without_output EXIT_CODE 2 ARGUMENTS xpath --force "1")
sxml_add_cli_test(xquery_output EXIT_CODE 0 WORKSPACE FILES books.xml titles.xq ARGUMENTS xquery --context-item=books.xml --output=titles.html titles.xq)
sxml_add_cli_test(xquery_output_update EXIT_CODE 0 WORKSPACE FILES books.xml ARGUMENTS xquery --context-item=books.xml --output=books-2.xml "--expression=delete node //book[2]")
sxml_add_cli_test(serialize_output EXIT_CODE 0 WORKSPACE FILES books.xml ARGUMENTS serialize --indent --output=books-indented.xml books.xml)
sxml_add_cli_test(serialize_output_same_file EXIT_CODE 0 WORKSPACE FILES books.xml ARGUMENTS serialize --method=text --force --output=books.xml books.xml)
sxml_add_cli_test(transform_output EXIT_CODE 0 WORKSPACE FILES books.xml xslt/books.xsl xslt/common.xsl ARGUMENTS transform --output=books.html xslt/books.xsl books.xml)
sxml_add_cli_test(transform_force_without_output EXIT_CODE 2 ARGUMENTS transform --force xslt/books.xsl books.xml)

# Result documents with an href: written only below --result-directory,
# which is the base output URI (XSLT 4.0, 2.4.6, 26.2; packet cli-2).
sxml_add_cli_test(transform_result_documents_dropped EXIT_CODE 0 WORKSPACE FILES books.xml xslt/chapters.xsl ARGUMENTS transform xslt/chapters.xsl books.xml)
sxml_add_cli_test(transform_result_directory EXIT_CODE 0 WORKSPACE FILES books.xml xslt/chapters.xsl EXISTING site/keep.txt ARGUMENTS transform --result-directory=site xslt/chapters.xsl books.xml)
sxml_add_cli_test(transform_result_directory_output EXIT_CODE 0 WORKSPACE FILES books.xml xslt/chapters.xsl EXISTING site/keep.txt ARGUMENTS transform --result-directory=./site/ -o site/index.txt xslt/chapters.xsl books.xml)
sxml_add_cli_test(transform_result_directory_exists EXIT_CODE 3 WORKSPACE FILES books.xml xslt/chapters.xsl EXISTING site/books/b2.xml ARGUMENTS transform --result-directory=site --output=index.txt xslt/chapters.xsl books.xml)
sxml_add_cli_test(transform_result_directory_force EXIT_CODE 0 WORKSPACE FILES books.xml xslt/chapters.xsl EXISTING site/books/b2.xml ARGUMENTS transform --result-directory=site --force xslt/chapters.xsl books.xml)
sxml_add_cli_test(transform_result_directory_parent EXIT_CODE 3 WORKSPACE FILES xslt/escape.xsl EXISTING site/keep.txt ARGUMENTS transform --result-directory=site xslt/escape.xsl)
sxml_add_cli_test(transform_result_directory_other_scheme EXIT_CODE 3 WORKSPACE FILES xslt/escape.xsl EXISTING site/keep.txt ARGUMENTS transform --result-directory=site --param=href=http://example.com/a.xml xslt/escape.xsl)
sxml_add_cli_test(transform_result_directory_escaped EXIT_CODE 3 WORKSPACE FILES xslt/escape.xsl EXISTING site/keep.txt ARGUMENTS transform --result-directory=site --param=href=a%2F..%2F..%2Fb.xml xslt/escape.xsl)
sxml_add_cli_test(transform_result_directory_missing EXIT_CODE 3 WORKSPACE FILES books.xml xslt/chapters.xsl ARGUMENTS transform --result-directory=site xslt/chapters.xsl books.xml)
sxml_add_cli_test(transform_result_directory_force_alone EXIT_CODE 2 ARGUMENTS transform --force xslt/hello.xsl)

# fn:put writes files only with --allow-put (XQUF 3.0, 6.1; D205; packet
# cli-2), relative URIs in the working directory, the static base URI.
sxml_add_cli_test(xquery_put_denied EXIT_CODE 1 WORKSPACE FILES books.xml ARGUMENTS xquery --context-item=books.xml "--expression=put(., 'copy.xml')")
sxml_add_cli_test(xquery_put EXIT_CODE 0 WORKSPACE FILES books.xml ARGUMENTS xquery --allow-put --context-item=books.xml "--expression=delete node //book[2], put(., 'copy.xml'), put(<note>{count(//book)}</note>, 'notes/count%201.xml', {'omit-xml-declaration': false()})" EXISTING notes/keep.txt)
sxml_add_cli_test(xquery_put_exists EXIT_CODE 3 WORKSPACE EXISTING copy.xml ARGUMENTS xquery --allow-put "--expression=put(<a/>, 'copy.xml')")
sxml_add_cli_test(xquery_put_force EXIT_CODE 0 WORKSPACE EXISTING copy.xml ARGUMENTS xquery --allow-put --force "--expression=put(<a/>, 'copy.xml')")
sxml_add_cli_test(xquery_put_not_file EXIT_CODE 3 WORKSPACE ARGUMENTS xquery --allow-put "--expression=put(<a/>, 'http://example.com/a.xml')")
sxml_add_cli_test(xquery_put_error EXIT_CODE 1 WORKSPACE ARGUMENTS xquery --allow-put "--expression=put(<a/>, 'a.xml'), put(<b/>, 'b.xml'), error()")
sxml_add_cli_test(xquery_put_bad_parameters EXIT_CODE 1 WORKSPACE ARGUMENTS xquery --allow-put "--expression=put(<a/>, 'a.xml', {'indent': 'x'})")
sxml_add_cli_test(xquery_force_without_files EXIT_CODE 2 ARGUMENTS xquery --force --expression=1)

# --method=csv (Serialization 4.0, CSV output method; packet cli-2).
sxml_add_cli_test(xpath_method_csv EXIT_CODE 0 ARGUMENTS xpath --method=csv "(['name', 'year'], ['XPath', 4], ['say \"hi\", twice', 3.5])")
sxml_add_cli_test(xquery_method_csv EXIT_CODE 0 ARGUMENTS xquery --method=csv --context-item=books.xml "--expression=//book ! [string(title), number(*:price)]")
sxml_add_cli_test(xpath_method_csv_error EXIT_CODE 1 ARGUMENTS xpath --method=csv "1")

# An unknown option of a command: "did you mean" its close option, or the
# other commands that have it (docs/CLI.md; packet cli-2).
sxml_add_cli_test(xpath_misspelled_option EXIT_CODE 2 ARGUMENTS xpath --contxt-item=books.xml "1")
sxml_add_cli_test(xpath_misspelled_output EXIT_CODE 2 ARGUMENTS xpath --ouptut=a.txt "1")
sxml_add_cli_test(xpath_option_of_xquery EXIT_CODE 2 ARGUMENTS xpath --expression=1)
sxml_add_cli_test(xpath_option_of_others EXIT_CODE 2 ARGUMENTS xpath --format=json "1")
sxml_add_cli_test(serialize_option_of_others EXIT_CODE 2 ARGUMENTS serialize --context-item=books.xml books.xml)
