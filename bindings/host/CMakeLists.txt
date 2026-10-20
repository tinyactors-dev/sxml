# The host shim (docs/GLOSSARY.md, "host_shim"; D238): the C layer under
# the JavaScript, Kotlin and Swift bindings, written against the public
# API only (D138).
#
#   sxml_host                    the shim with every language and every
#                                optional feature sxml_all_optional_features()
#                                names: the native library the Kotlin JNI
#                                glue and the Swift package link, and what
#                                tests/host_shim_test.c drives.
#
# The JavaScript module links the sources itself, from the lists below
# (bindings/javascript/CMakeLists.txt): all of them, since every build has
# every language and feature (D272).

# XPath, values, serialization, the processor.
set(SXML_HOST_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/sxml_host.c"
    PARENT_SCOPE)
# XQuery, XSLT and XSD.
set(sxml_host_full_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/sxml_host_full.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sxml_host_xquery.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sxml_host_xslt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sxml_host_schema.c")
set(SXML_HOST_FULL_SOURCES ${sxml_host_full_sources} PARENT_SCOPE)
set(SXML_HOST_INCLUDE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)

add_library(sxml_host STATIC sxml_host.c ${sxml_host_full_sources} sxml_host_full_features.c)
target_include_directories(sxml_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sxml_host PUBLIC sxml)
sxml_apply_project_flags(sxml_host)

# tests/host_shim_test.c, registered by tests/CMakeLists.txt's glob (D45),
# links the shim as a host binding does.
if(TARGET host_shim_test)
    target_link_libraries(host_shim_test PRIVATE sxml_host)
endif()
