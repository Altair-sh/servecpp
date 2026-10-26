cmake_minimum_required(VERSION 3.21)

set(PROJ_NAME servecpp)
set(SOURCES
    src/main.cpp
    src/Logger.cpp
    src/Config.cpp
    src/Auth.cpp
    src/Path.cpp
    src/HttpSession.cpp
    src/Exchange.cpp
    src/Download.cpp
    src/Listing.cpp
    src/Upload.cpp
    src/Mime.cpp
    src/TempFile.cpp
    src/BoostSrc.cpp
)
# HTML templates, transpiled to C++ headers by hypertextcpp
set(TEMPLATES
    templates/SharesPage.htcpp
    templates/ListingPage.htcpp
)

# Debug: warnings and a precompiled header.
# The precompiled header has only what most files use: every file loads the whole of it,
# adding CLI11 and toml++ (one file each) made the other files slower
set(DEBUG_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
)
set(DEBUG_PRECOMPILED_HEADERS
    <boost/asio.hpp>
    <boost/beast.hpp>
)

# Release: link-time optimization, every function and variable in its own section,
# so the linker can remove the unused ones.
# No precompiled header: release is a clean build on the server, where it adds time and a large file
set(RELEASE_COMPILE_OPTIONS
    -flto=auto
    -ffunction-sections
    -fdata-sections
)
set(RELEASE_LINK_OPTIONS
    -flto=auto
    -fuse-linker-plugin
    -Wl,--gc-sections
    -s
)

# vcpkg's default triplet on Windows uses MSVC, this project is built with MinGW GCC
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET x64-mingw-static)
    set(VCPKG_HOST_TRIPLET x64-mingw-static)
endif()

project(${PROJ_NAME} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# replacement of CMake's default -O3, which makes larger code with little gain
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(hypertextcpp CONFIG REQUIRED)

add_executable(${PROJ_NAME} ${SOURCES})
# generate C++ headers from .htcpp files, source code includes them
hypertextcpp_TargetTemplates(${PROJ_NAME} TEMPLATE_FILES ${TEMPLATES})
# to include "Path.hpp" in generated headers
target_include_directories(${PROJ_NAME} PRIVATE src)
target_link_libraries(${PROJ_NAME} PRIVATE
    Boost::asio
    Boost::beast
    CLI11::CLI11
    tomlplusplus::tomlplusplus
)
# disable old code parts kept for compatibility because they produce warnings
target_compile_definitions(${PROJ_NAME} PRIVATE BOOST_ASIO_NO_DEPRECATED)
# Boost.DateTime isn't installed (vcpkg.json), Asio would include it otherwise
target_compile_definitions(${PROJ_NAME} PRIVATE BOOST_ASIO_DISABLE_BOOST_DATE_TIME)
# boost libraries are header-only by default, it slows down compilation.
# Non-template code of Asio and Beast is compiled once, in src/BoostSrc.cpp
target_compile_definitions(${PROJ_NAME} PRIVATE BOOST_ASIO_SEPARATE_COMPILATION BOOST_BEAST_SEPARATE_COMPILATION)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${PROJ_NAME} PRIVATE ${DEBUG_WARNINGS})
    target_precompile_headers(${PROJ_NAME} PRIVATE ${DEBUG_PRECOMPILED_HEADERS})
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJ_NAME} PRIVATE ${RELEASE_COMPILE_OPTIONS})
    target_link_options(${PROJ_NAME} PRIVATE ${RELEASE_LINK_OPTIONS})
endif()

if(WIN32)
    target_compile_definitions(${PROJ_NAME} PRIVATE _WIN32_WINNT=0x0A00)
    # bcrypt: random pipe names in Asio's separately compiled code
    target_link_libraries(${PROJ_NAME} PRIVATE ws2_32 mswsock bcrypt)
endif()
