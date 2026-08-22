cmake_minimum_required(VERSION 3.28)
project(vnm_terminal_workspace VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(VNM_TERMINAL_WORKSPACE_BUILD_TESTS
    "Build vnm_terminal_workspace focused tests"
    ${PROJECT_IS_TOP_LEVEL})

include(cmake/vnm_environment_policy_dependency.cmake)
vnm_terminal_workspace_make_environment_policy_available()

add_library(vnm_terminal_workspace_request STATIC)
add_library(
    vnm_terminal_workspace::vnm_terminal_workspace_request
    ALIAS vnm_terminal_workspace_request)

target_compile_features(vnm_terminal_workspace_request PUBLIC cxx_std_20)
target_sources(vnm_terminal_workspace_request
    PRIVATE
        src/terminal_launch_request.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/vnm_terminal_workspace/terminal_launch_request.h)
target_link_libraries(vnm_terminal_workspace_request
    PUBLIC
        vnm_framework::vnm_environment_policy)
target_include_directories(vnm_terminal_workspace_request
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(VNM_TERMINAL_WORKSPACE_BUILD_TESTS)
    add_executable(vnm_terminal_workspace_request_tests
        tests/terminal_launch_request_tests.cpp)
    target_include_directories(vnm_terminal_workspace_request_tests
        PRIVATE
            src)
    target_link_libraries(vnm_terminal_workspace_request_tests
        PRIVATE
            vnm_terminal_workspace::vnm_terminal_workspace_request)
    add_test(
        NAME vnm_terminal_workspace_request_tests
        COMMAND vnm_terminal_workspace_request_tests)
endif()

configure_package_config_file(
    cmake/vnm_terminal_workspace-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_workspace-config.cmake"
    INSTALL_DESTINATION
        "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_workspace")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_workspace-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(
    TARGETS vnm_terminal_workspace_request
    EXPORT vnm_terminal_workspace_targets
    FILE_SET HEADERS
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
    EXPORT vnm_terminal_workspace_targets
    NAMESPACE vnm_terminal_workspace::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_workspace")
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_workspace-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_workspace-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_workspace")
