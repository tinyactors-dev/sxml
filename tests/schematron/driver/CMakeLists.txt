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
#
# differential.py, the differential test against SchXslt
# (third_party/schxslt), is run by hand: SchXslt takes minutes to compile
# a rule set (D323).

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
    # The CII unit tests wait for the owner: EN16931-CII-syntax.sch names
    # the parameter CII-SR-282 twice in one pattern instance, which 5.4.11
    # and Annex B make an incorrect schema (D323, "Proposed").
endif()
