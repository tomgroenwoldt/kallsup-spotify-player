# cspot_overlay(<source> <srcs_var> <find1> <replace1> [<find2> <replace2> ...])
#
# Copies <source> into the build directory with each <find> replaced by its
# <replace>, and swaps the copy in for the original in list <srcs_var>.
# Configuration fails if a <find> text is missing, so an overlay can't silently
# stop applying after a submodule update.
function(cspot_overlay source srcs_var)
    file(READ "${source}" content)
    # ARGV<n> keeps each argument whole; ARGN would split C++ code at ';'.
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 2 ${last} 2)
        math(EXPR j "${i} + 1")
        set(find "${ARGV${i}}")
        set(replace "${ARGV${j}}")
        string(FIND "${content}" "${find}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "cspot overlay: text not found in ${source}:\n${find}")
        endif()
        string(REPLACE "${find}" "${replace}" content "${content}")
    endforeach()

    get_filename_component(name "${source}" NAME)
    set(out "${CMAKE_BINARY_DIR}/cspot_overlay/${name}")
    # Only rewrite on change, so unchanged overlays don't trigger rebuilds.
    if(EXISTS "${out}")
        file(READ "${out}" old)
    endif()
    if(NOT old STREQUAL content)
        file(WRITE "${out}" "${content}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")

    set(srcs ${${srcs_var}})
    list(REMOVE_ITEM srcs "${source}")
    list(APPEND srcs "${out}")
    set(${srcs_var} ${srcs} PARENT_SCOPE)
endfunction()
