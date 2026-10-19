# Writes OUTPUT, a C file that embeds the CLDR data the sxml command
# serves to the UCA collation feature (D213, D220) and the localization
# feature (D221): each INPUTS item is PATH=FILE, the path under CLDR's
# common/ directory the features ask for ("urn:sxml:collation-data:" or
# "urn:sxml:locale-data:" + PATH) and the vendored file
# (third_party/cldr/common/PATH). What the features do not read is left
# out, so the files are subsets of CLDR's with the same paths and values:
# - a .txt file (the root's allkeys_CLDR.txt, UTS #10, section 9.1) loses
#   its comments and blank lines;
# - a main/ file keeps ldml/dates/calendars/calendar of the types
#   gregorian, buddhist, japanese, islamic and islamic-civil, and of each
#   only months, days, dayPeriods and eras (UTS #35, Part 4, section 2);
# - supplemental/supplementalData.xml keeps supplementalData/calendarData;
# - other .xml files (collation/, rbnf/, supplemental/ordinals.xml) are
#   kept as they are.
# The text is split into string literals of at most about 4,000 bytes,
# below the 4,095 that C17 (5.2.4.1) asks compilers to support, so
# -Wpedantic has nothing to say; the resolver of apps/sxml/query_command.c
# joins them. INPUTS separates its items with "|".
#
#   cmake "-DINPUTS=uca/allkeys_CLDR.txt=/x/allkeys_CLDR.txt|main/de.xml=/x/de.xml"
#         -DOUTPUT=sxml_cldr_data.c -P embed-cldr-data.cmake

# Appends to the variable named by result the element <name>...</name> of
# text (empty when text has none).
function(sxml_cldr_element text name result)
    string(FIND "${text}" "<${name}>" start)
    if(start EQUAL -1)
        set(${result} "${${result}}" PARENT_SCOPE)
        return()
    endif()
    string(SUBSTRING "${text}" ${start} -1 rest)
    string(FIND "${rest}" "</${name}>" end)
    if(end EQUAL -1)
        set(${result} "${${result}}" PARENT_SCOPE)
        return()
    endif()
    string(LENGTH "</${name}>" close_length)
    math(EXPR end "${end} + ${close_length}")
    string(SUBSTRING "${rest}" 0 ${end} element)
    set(${result} "${${result}}${element}\n" PARENT_SCOPE)
endfunction()

# The calendars a main/ file keeps, reduced to the names.
function(sxml_cldr_reduce_main text result)
    set(reduced "<ldml>\n<dates>\n<calendars>\n")
    foreach(calendar_type gregorian buddhist japanese islamic islamic-civil)
        string(FIND "${text}" "<calendar type=\"${calendar_type}\">" start)
        if(start EQUAL -1)
            continue()
        endif()
        string(SUBSTRING "${text}" ${start} -1 rest)
        string(FIND "${rest}" "</calendar>" end)
        string(SUBSTRING "${rest}" 0 ${end} calendar)
        set(parts "")
        foreach(part months days dayPeriods eras)
            sxml_cldr_element("${calendar}" ${part} parts)
        endforeach()
        string(APPEND reduced "<calendar type=\"${calendar_type}\">\n${parts}</calendar>\n")
    endforeach()
    string(APPEND reduced "</calendars>\n</dates>\n</ldml>\n")
    set(${result} "${reduced}" PARENT_SCOPE)
endfunction()

string(REPLACE "|" ";" inputs "${INPUTS}")
set(files_text "")
set(arrays_text "")
set(file_count 0)
foreach(input IN LISTS inputs)
    string(FIND "${input}" "=" separator)
    string(SUBSTRING "${input}" 0 ${separator} path)
    math(EXPR file_start "${separator} + 1")
    string(SUBSTRING "${input}" ${file_start} -1 file)
    file(READ "${file}" text)
    if(path MATCHES "\\.txt$")
        string(REGEX REPLACE "#[^\n]*" "" text "${text}")
        string(REGEX REPLACE "[ \t\r]+\n" "\n" text "${text}")
        string(REGEX REPLACE "\n\n+" "\n" text "${text}")
        string(REGEX REPLACE "^\n" "" text "${text}")
    elseif(path MATCHES "^main/")
        sxml_cldr_reduce_main("${text}" text)
    elseif(path STREQUAL "supplemental/supplementalData.xml")
        set(calendar_data "")
        sxml_cldr_element("${text}" calendarData calendar_data)
        set(text "<supplementalData>\n${calendar_data}</supplementalData>\n")
    endif()
    # C string literal escapes.
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE "\t" "\\t" text "${text}")
    string(REPLACE "\r" "" text "${text}")
    # Lines become list elements: ";" is CMake's separator and brackets
    # change how it splits, so they are carried as markers the data does not
    # have and put back below.
    string(REPLACE ";" "@sxml-semicolon@" text "${text}")
    string(REPLACE "[" "@sxml-open@" text "${text}")
    string(REPLACE "]" "@sxml-close@" text "${text}")
    string(REPLACE "\n" ";" text "${text}")
    set(chunks "")
    set(chunk "")
    set(chunk_count 0)
    foreach(line IN LISTS text)
        string(APPEND chunk "${line}\\n")
        string(LENGTH "${chunk}" chunk_length)
        if(chunk_length GREATER 3900)
            string(APPEND chunks "    \"${chunk}\",\n")
            set(chunk "")
            math(EXPR chunk_count "${chunk_count} + 1")
        endif()
    endforeach()
    if(NOT chunk STREQUAL "")
        string(APPEND chunks "    \"${chunk}\",\n")
        math(EXPR chunk_count "${chunk_count} + 1")
    endif()
    string(REPLACE "@sxml-semicolon@" ";" chunks "${chunks}")
    string(REPLACE "@sxml-open@" "[" chunks "${chunks}")
    string(REPLACE "@sxml-close@" "]" chunks "${chunks}")
    string(APPEND arrays_text "static const char *const sxml_cli_cldr_chunks_${file_count}[${chunk_count}] = {
${chunks}};

")
    string(APPEND files_text "    {.path = \"${path}\",
     .text = {.chunks = sxml_cli_cldr_chunks_${file_count}, .chunk_count = ${chunk_count}}},
")
    math(EXPR file_count "${file_count} + 1")
endforeach()
file(WRITE "${OUTPUT}" "/* Generated by apps/sxml/embed-cldr-data.cmake from
   third_party/cldr; do not edit. */
#include \"cldr_data.h\"

${arrays_text}static const sxml_cli_cldr_file sxml_cli_cldr_files[${file_count}] = {
${files_text}};

sxml_cli_cldr_file_view sxml_cli_cldr_data(void) {
    return (sxml_cli_cldr_file_view){.files = sxml_cli_cldr_files,
                                     .file_count = ${file_count}};
}
")
