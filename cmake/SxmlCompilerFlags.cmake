# Compiler settings for project-owned code (docs/C_GUIDELINES.md,
# "Enforcement"). Third-party targets do not call this function.

function(sxml_apply_project_flags target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wvla
            -Wswitch-enum
            -Wimplicit-fallthrough)
    endif()
endfunction()

# Compiler settings for vendored and generated code: its own C standard, no
# project warnings and never -Werror (D12: third-party code is exempt from the
# guidelines).
function(sxml_apply_third_party_flags target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
endfunction()
