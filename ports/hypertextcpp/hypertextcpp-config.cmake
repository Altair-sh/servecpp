# Package config for find_package(hypertextcpp CONFIG), added by the overlay port:
# hypertextcpp itself installs only the executable.
# Provides hypertextcpp_TargetTemplates() below and the functions from hypertextcpp.cmake (hypertextcpp_GenerateHeader...).
# They run the command "hypertextcpp", and CMake replaces a target name in a command with its location,
# so the imported target below makes them use the tool from vcpkg, which isn't on PATH.

find_program(HYPERTEXTCPP_EXECUTABLE hypertextcpp
    PATHS "${CMAKE_CURRENT_LIST_DIR}/../../tools/hypertextcpp"
    NO_DEFAULT_PATH
    REQUIRED)

if(NOT TARGET hypertextcpp)
    add_executable(hypertextcpp IMPORTED)
    set_target_properties(hypertextcpp PROPERTIES IMPORTED_LOCATION "${HYPERTEXTCPP_EXECUTABLE}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/hypertextcpp.cmake")

# hypertextcpp_TargetTemplates(<target> TEMPLATE_FILES <file>... [OUTPUT_DIR <dir>])
# Transpiles each template <name>.htcpp to a single header <name>.h during the build.
# The headers are added to the target's sources and their directory to its include directories,
# so the target's code includes them as "<name>.h".
# TEMPLATE_FILES: relative paths are relative to CMAKE_CURRENT_SOURCE_DIR.
# OUTPUT_DIR: default hypertextcpp/<target>, relative paths are relative to CMAKE_CURRENT_BINARY_DIR.
function(hypertextcpp_TargetTemplates TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "OUTPUT_DIR" "TEMPLATE_FILES")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "[hypertextcpp_TargetTemplates] Unsupported argument: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "[hypertextcpp_TargetTemplates] '${TARGET}' is not a target")
    endif()
    if(NOT ARG_TEMPLATE_FILES)
        message(FATAL_ERROR "[hypertextcpp_TargetTemplates] Argument 'TEMPLATE_FILES' is missing")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR hypertextcpp/${TARGET})
    endif()
    cmake_path(ABSOLUTE_PATH ARG_OUTPUT_DIR BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    file(MAKE_DIRECTORY ${ARG_OUTPUT_DIR})

    foreach(TEMPLATE_FILE IN LISTS ARG_TEMPLATE_FILES)
        hypertextcpp_GenerateHeader(TEMPLATE_FILE ${TEMPLATE_FILE} OUTPUT_DIR ${ARG_OUTPUT_DIR})
        # the header is named like the template without its last extension, as hypertextcpp_GenerateHeader() does
        cmake_path(GET TEMPLATE_FILE STEM LAST_ONLY TEMPLATE_NAME)
        target_sources(${TARGET} PRIVATE ${ARG_OUTPUT_DIR}/${TEMPLATE_NAME}.h)
    endforeach()
    target_include_directories(${TARGET} PRIVATE ${ARG_OUTPUT_DIR})
endfunction()
