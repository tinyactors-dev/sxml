# The Swift package (D238, packet 6.7): module Sxml over the host shim,
# with Package.swift at the repository root. On Linux the package links
# the host shim from this build tree; on Apple platforms it links the
# XCFramework of packet 6.9 instead.
#
#   sxml_host_swift_library   lib/libsxml_host.a: the static libraries of
#                             sxml_host (the shim, sxml, the grammars,
#                             tree-sitter, libmpdec) merged into one archive,
#                             the library the module map CSxmlHost links.
#   ctest swift/linux         swift test against that archive; skipped
#                             (code 77) when no Swift toolchain is found.
#
# SXML_SWIFT names the swift driver; empty looks for swift on PATH.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    return()
endif()

set(SXML_SWIFT "" CACHE FILEPATH "The swift driver for the swift/linux test (empty: swift on PATH)")
if(SXML_SWIFT STREQUAL "")
    find_program(sxml_swift_found NAMES swift)
    set(sxml_swift_driver "${sxml_swift_found}")
else()
    set(sxml_swift_driver "${SXML_SWIFT}")
endif()

# Every static library sxml_host links, walked breadth-first over
# LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES (a static library keeps its
# private dependencies there as $<LINK_ONLY:...>), and the grammars, whose
# link to sxml grammars/CMakeLists.txt defers to the end of the root.
get_property(sxml_swift_grammars GLOBAL PROPERTY SXML_GRAMMAR_LIBRARIES)
set(sxml_swift_pending sxml_host ${sxml_swift_grammars})
set(sxml_swift_archives "")
while(sxml_swift_pending)
    list(POP_FRONT sxml_swift_pending sxml_swift_target)
    if(NOT TARGET "${sxml_swift_target}" OR sxml_swift_target IN_LIST sxml_swift_archives)
        continue()
    endif()
    get_target_property(sxml_swift_type "${sxml_swift_target}" TYPE)
    if(NOT sxml_swift_type STREQUAL "STATIC_LIBRARY")
        continue()
    endif()
    list(APPEND sxml_swift_archives "${sxml_swift_target}")
    foreach(sxml_swift_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(sxml_swift_links "${sxml_swift_target}" ${sxml_swift_property})
        if(sxml_swift_links)
            foreach(sxml_swift_link IN LISTS sxml_swift_links)
                string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" sxml_swift_link "${sxml_swift_link}")
                list(APPEND sxml_swift_pending "${sxml_swift_link}")
            endforeach()
        endif()
    endforeach()
endwhile()

# One archive, so that the manifest names one library whatever sxml links
# (GNU ar and llvm-ar read the MRI script).
set(sxml_swift_library_directory "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(sxml_swift_library "${sxml_swift_library_directory}/libsxml_host.a")
set(sxml_swift_script_content "create ${sxml_swift_library}\n")
foreach(sxml_swift_archive IN LISTS sxml_swift_archives)
    string(APPEND sxml_swift_script_content "addlib $<TARGET_FILE:${sxml_swift_archive}>\n")
endforeach()
string(APPEND sxml_swift_script_content "save\nend\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/merge-archives.mri"
    CONTENT "${sxml_swift_script_content}")
set(sxml_swift_archive_files "")
foreach(sxml_swift_archive IN LISTS sxml_swift_archives)
    list(APPEND sxml_swift_archive_files "$<TARGET_FILE:${sxml_swift_archive}>")
endforeach()
add_custom_command(
    OUTPUT "${sxml_swift_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${sxml_swift_library_directory}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${sxml_swift_library}"
    COMMAND sh -c "\"${CMAKE_AR}\" -M < \"${CMAKE_CURRENT_BINARY_DIR}/merge-archives.mri\""
    DEPENDS ${sxml_swift_archives} ${sxml_swift_archive_files}
        "${CMAKE_CURRENT_BINARY_DIR}/merge-archives.mri"
    COMMENT "Merging the host shim's static libraries for the Swift package"
    VERBATIM)
add_custom_target(sxml_host_swift_library ALL DEPENDS "${sxml_swift_library}")

if(NOT SXML_BUILD_TESTS)
    return()
endif()

if(sxml_swift_driver STREQUAL "" OR NOT EXISTS "${sxml_swift_driver}")
    add_test(NAME swift/linux
        COMMAND sh -c "echo 'swift/linux: skipped, no Swift toolchain found (set SXML_SWIFT or put swift on PATH)'; exit 77")
    set_tests_properties(swift/linux PROPERTIES SKIP_RETURN_CODE 77 LABELS bindings)
    return()
endif()

# The sanitizers of the build (D23): the Swift test binary links the ASan
# runtime, which has UBSan's handlers too (swiftc has no
# -sanitize=undefined on Linux).
set(sxml_swift_arguments "")
if(SXML_ENABLE_SANITIZERS)
    list(APPEND sxml_swift_arguments --sanitize=address)
endif()
add_test(NAME swift/linux
    COMMAND "${sxml_swift_driver}" test
        --package-path "${PROJECT_SOURCE_DIR}"
        --scratch-path "${CMAKE_CURRENT_BINARY_DIR}/swiftpm"
        --disable-xctest
        -Xswiftc -warnings-as-errors
        ${sxml_swift_arguments})
set_tests_properties(swift/linux PROPERTIES
    LABELS bindings
    TIMEOUT 1800
    ENVIRONMENT "SXML_HOST_LIBRARY_PATH=${sxml_swift_library_directory};SXML_HOST_FEATURES=test,expath-binary,html,xslt-transform,timezone-database,dtd-validation,uca-collation,full-text,invisible-xml,localization,builtin-schemas")
