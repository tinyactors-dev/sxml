# The Schematron test driver, sxml-schematron-test (packet 7.28; D315,
# D323): a developer tool, not shipped, on the public API only. Joins the
# build from the root CMakeLists.txt once this file exists.
#
#   sxml_schematron_driver   the executable "sxml-schematron-test"
#                            (main.c; its queries cases.xq, compare.xq
#                            and vefa.xq are read at run time).
#   schematron/suite         the conformance suite of tests/schematron,
#                            with the deviation list deviations.txt.
#   schematron/en16931-ubl   EN 16931's VEFA unit tests of the UBL binding
#                            against the rules compiled from its .sch
#                            sources (third_party/en16931).
#   schematron/en16931-cii   the same for the CII binding (D331).
#   schematron/xrechnung-*   XRechnung's UBL and CII rule sets compiled from
#                            their sources (third_party/xrechnung) and run
#                            over its instances (D331).
#
# differential.py, the differential test against SchXslt
# (third_party/schxslt) on the unpatched sources of EN 16931 and
# XRechnung, joins ctest as schematron/differential-* when the option
# SXML_SCHEMATRON_DIFFERENTIAL is on: SchXslt takes minutes to compile a
# rule set (D323, D331); the four take some ten minutes or more.

add_executable(sxml_schematron_driver main.c)
target_link_libraries(sxml_schematron_driver PRIVATE sxml)
set_target_properties(sxml_schematron_driver PROPERTIES OUTPUT_NAME sxml-schematron-test)
sxml_apply_project_flags(sxml_schematron_driver)

add_test(NAME schematron/suite
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:sxml_schematron_driver>
            run --suite=tests/schematron --deviations=tests/schematron/driver/deviations.txt
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

set(en16931 "${PROJECT_SOURCE_DIR}/third_party/en16931")
if(EXISTS "${en16931}/ubl/schematron/EN16931-UBL-validation.sch")
    file(GLOB en16931_ubl_tests "${en16931}/test/Invoice-unit-UBL/*.xml"
                                "${en16931}/test/CreditNote-unit-UBL/*.xml")
    list(SORT en16931_ubl_tests)
    add_test(NAME schematron/en16931-ubl
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:sxml_schematron_driver>
                vefa --quiet --schematron=${en16931}/ubl/schematron/EN16931-UBL-validation.sch
                ${en16931_ubl_tests}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    # EN16931-CII-syntax.sch names the parameter CII-SR-282 twice in one
    # pattern instance, which 5.4.11 and Annex B make an error: it is read
    # with a compatibility warning, the first value used (D331).
    file(GLOB en16931_cii_tests "${en16931}/test/cii/*.xml")
    list(SORT en16931_cii_tests)
    add_test(NAME schematron/en16931-cii
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:sxml_schematron_driver>
                vefa --quiet --schematron=${en16931}/cii/schematron/EN16931-CII-validation.sch
                ${en16931_cii_tests}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

set(xrechnung "${PROJECT_SOURCE_DIR}/third_party/xrechnung")
if(EXISTS "${xrechnung}/schematron/ubl/XRechnung-UBL-validation.sch")
    # XRechnung reads pattern variables from other patterns and writes lets
    # after assertions: compiled with compatibility warnings (D331).
    file(GLOB xrechnung_ubl_instances "${xrechnung}/instances/ubl-inv/*.xml"
                                      "${xrechnung}/instances/ubl-cn/*.xml")
    file(GLOB xrechnung_cii_instances "${xrechnung}/instances/cii/*.xml")
    list(SORT xrechnung_ubl_instances)
    list(SORT xrechnung_cii_instances)
    add_test(NAME schematron/xrechnung-ubl
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:sxml_schematron_driver>
                report --schematron=${xrechnung}/schematron/ubl/XRechnung-UBL-validation.sch
                ${xrechnung_ubl_instances}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    add_test(NAME schematron/xrechnung-cii
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:sxml_schematron_driver>
                report --schematron=${xrechnung}/schematron/cii/XRechnung-CII-validation.sch
                ${xrechnung_cii_instances}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

    option(SXML_SCHEMATRON_DIFFERENTIAL
           "Run the Schematron differential test against SchXslt in ctest (slow)" OFF)
    find_program(sxml_python3 python3)
    if(SXML_SCHEMATRON_DIFFERENTIAL AND sxml_python3 AND TARGET sxml_cli
       AND EXISTS "${en16931}/ubl/schematron/EN16931-UBL-validation.sch")
        file(GLOB en16931_ubl_instances "${en16931}/test/testfiles/*.xml")
        list(SORT en16931_ubl_instances)
        function(sxml_add_schematron_differential name schema)
            add_test(NAME schematron/differential-${name}
                COMMAND ${sxml_python3} -I ${CMAKE_CURRENT_SOURCE_DIR}/differential.py
                        --sxml $<TARGET_FILE:sxml_cli>
                        --driver $<TARGET_FILE:sxml_schematron_driver>
                        --schxslt ${PROJECT_SOURCE_DIR}/third_party/schxslt/2.0/pipeline-for-svrl.xsl
                        --schema ${schema}
                        --work ${CMAKE_CURRENT_BINARY_DIR}/differential
                        ${ARGN}
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        endfunction()
        sxml_add_schematron_differential(en16931-ubl
            ${en16931}/ubl/schematron/EN16931-UBL-validation.sch
            ${en16931_ubl_instances} ${xrechnung_ubl_instances})
        sxml_add_schematron_differential(en16931-cii
            ${en16931}/cii/schematron/EN16931-CII-validation.sch ${xrechnung_cii_instances})
        sxml_add_schematron_differential(xrechnung-ubl
            ${xrechnung}/schematron/ubl/XRechnung-UBL-validation.sch ${xrechnung_ubl_instances})
        sxml_add_schematron_differential(xrechnung-cii
            ${xrechnung}/schematron/cii/XRechnung-CII-validation.sch ${xrechnung_cii_instances})
    endif()
endif()
