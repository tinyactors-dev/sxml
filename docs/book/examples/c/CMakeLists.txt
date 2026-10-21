cmake_minimum_required(VERSION 3.24)
project(sxml_book_examples C)

# Where the sxml source is: a checkout, a git submodule, or a directory
# you unpacked. Only the library is built, without sxml's own tests and
# command.
set(SXML_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/sxml" CACHE PATH "The sxml source directory")
set(SXML_BUILD_TESTS OFF)
set(SXML_BUILD_APPS OFF)
add_subdirectory("${SXML_SOURCE}" sxml EXCLUDE_FROM_ALL)

foreach(example requirements compile_once resolver errors memory)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE sxml)
endforeach()
