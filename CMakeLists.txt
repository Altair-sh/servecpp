cmake_minimum_required(VERSION 3.21)

set(PROJ_NAME servecpp)
set(PROJ_SOURCES
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
set(PROJ_TEMPLATES
    templates/SharesPage.htcpp
    templates/ListingPage.htcpp
)
# Only what most files use: every file loads the whole precompiled header,
# adding CLI11 and toml++ (one file each) made the other files slower
set(PROJ_PRECOMPILED_HEADERS
    <stdio.h>
    <boost/asio.hpp>
    <boost/beast.hpp>
)
set(PROJ_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
)
# Release: link-time optimization, every function and variable in its own section,
# so the linker can remove the unused ones
set(PROJ_RELEASE_COMPILE_OPTIONS
    -flto=auto
    -ffunction-sections
    -fdata-sections
)
set(PROJ_RELEASE_LINK_OPTIONS
    -flto=auto
    -fuse-linker-plugin
    -Wl,--gc-sections
    -s
)

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

add_executable(${PROJ_NAME} ${PROJ_SOURCES})
# generate C++ headers from .htcpp files, source code includes them
hypertextcpp_TargetTemplates(${PROJ_NAME} TEMPLATE_FILES ${PROJ_TEMPLATES})
# to include "Path.hpp" in generated headers
target_include_directories(${PROJ_NAME} PRIVATE src)
target_precompile_headers(${PROJ_NAME} PRIVATE ${PROJ_PRECOMPILED_HEADERS})
target_compile_options(${PROJ_NAME} PRIVATE "$<$<CONFIG:Debug>:${PROJ_WARNINGS}>")
target_compile_options(${PROJ_NAME} PRIVATE "$<$<CONFIG:Release>:${PROJ_RELEASE_COMPILE_OPTIONS}>")
target_link_options(${PROJ_NAME} PRIVATE "$<$<CONFIG:Release>:${PROJ_RELEASE_LINK_OPTIONS}>")
target_link_libraries(${PROJ_NAME} PRIVATE 
    Boost::asio
    Boost::beast
    CLI11::CLI11
    tomlplusplus::tomlplusplus)
# disable old code parts kept for compatibility because they produce warnings
target_compile_definitions(${PROJ_NAME} PRIVATE BOOST_ASIO_NO_DEPRECATED)
# non-template code of Asio and Beast is compiled once, in src/BoostSrc.cpp
target_compile_definitions(${PROJ_NAME} PRIVATE BOOST_ASIO_SEPARATE_COMPILATION BOOST_BEAST_SEPARATE_COMPILATION)

if(WIN32)
    target_compile_definitions(${PROJ_NAME} PRIVATE _WIN32_WINNT=0x0A00)
    # bcrypt: random pipe names in Asio's separately compiled code
    target_link_libraries(${PROJ_NAME} PRIVATE ws2_32 mswsock bcrypt)
endif()
