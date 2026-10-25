# Overlay port: hypertextcpp has no port in the official vcpkg registry.
# Builds the hypertextcpp executable (a build tool, used as a host dependency)
# and installs it with hypertextcpp.cmake, which defines the CMake functions that call it,
# and hypertextcpp-config.cmake from this port for find_package(hypertextcpp CONFIG),
# which adds hypertextcpp_TargetTemplates().
# The build downloads its own dependencies (seal_lake, cmdlime, sfun, GSL, fmt, range-v3) with git.

set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kamchatka-volcano/hypertextcpp
    REF v2.0.0
    SHA512 de5894a4f635ee24f454b27a60a0a40111a9bf25232c935c8c41de2841959f72ddd96b377f446a0452b6adf46bcaeae9497a97a59d7a3d08cf518ae99f1876a9
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # vcpkg disables downloads in port builds, the dependencies are fetched by hypertextcpp's CMakeLists.txt
        -DFETCHCONTENT_FULLY_DISCONNECTED=OFF
)
vcpkg_cmake_install()
vcpkg_copy_tools(TOOL_NAMES hypertextcpp AUTO_CLEAN)

file(INSTALL
    "${SOURCE_PATH}/hypertextcpp.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/hypertextcpp-config.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")

# a tool only, no headers or libraries
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
