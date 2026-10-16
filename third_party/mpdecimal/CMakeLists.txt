# libmpdec from mpdecimal, pinned in third_party/mpdecimal.version and
# patched as D42 (allocation through a scoped sxml_allocator) and D49 (one
# header for every target). Vendored code: exempt from docs/C_GUIDELINES.md
# and the lint gates (D12), built with sxml_apply_third_party_flags.
#
# Patched files, each change marked "sxml patch":
#   libmpdec/mpdecimal.h  configuration chosen from the width of size_t;
#                         the allocation hooks are functions, not pointers.
#   libmpdec/mpalloc.c    the hooks allocate through the decimal scope.
#   libmpdec/context.c    WASI has no signals (D57).
#   libmpdec/transpose.c  transform buffers on the heap, not the stack (D74).

add_library(sxml_mpdecimal STATIC
    libmpdec/basearith.c
    libmpdec/constants.c
    libmpdec/context.c
    libmpdec/convolute.c
    libmpdec/crt.c
    libmpdec/difradix2.c
    libmpdec/fnt.c
    libmpdec/fourstep.c
    libmpdec/io.c
    libmpdec/mpalloc.c
    libmpdec/mpdecimal.c
    libmpdec/mpsignal.c
    libmpdec/numbertheory.c
    libmpdec/sixstep.c
    libmpdec/transpose.c)
# mpalloc.c reads the sxml_allocator type from include/sxml/memory.h; it
# calls no sxml function, so libmpdec does not depend on the sxml library.
target_include_directories(sxml_mpdecimal
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libmpdec"
    PRIVATE "${PROJECT_SOURCE_DIR}/include")
# LIBMPDEC_SXML_BUILD makes mpdecimal.h define the build configuration
# (CONFIG_64 or CONFIG_32, ANSI); NDEBUG keeps libmpdec's assert calls out
# of the library in every build type, as upstream release builds do.
target_compile_definitions(sxml_mpdecimal PRIVATE LIBMPDEC_SXML_BUILD NDEBUG)
# A shared sxml library must not export libmpdec's symbols.
set_target_properties(sxml_mpdecimal PROPERTIES C_VISIBILITY_PRESET hidden)
sxml_apply_third_party_flags(sxml_mpdecimal)
# libmpdec calls log10 (mpdecimal.c, precision estimates of the logarithm
# functions), which lives in a separate math library on Linux and Android.
if(NOT MSVC AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    find_library(SXML_MATH_LIBRARY m)
    if(SXML_MATH_LIBRARY)
        target_link_libraries(sxml_mpdecimal PUBLIC "${SXML_MATH_LIBRARY}")
    endif()
endif()
