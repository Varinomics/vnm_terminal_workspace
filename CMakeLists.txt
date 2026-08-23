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
include(cmake/vnm_terminal_dependency.cmake)
vnm_terminal_workspace_make_terminal_available()
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick)
add_library(
    vnm_terminal_workspace::_terminal_app_support_dependency
    INTERFACE IMPORTED GLOBAL)
set_property(
    TARGET vnm_terminal_workspace::_terminal_app_support_dependency
    PROPERTY INTERFACE_LINK_LIBRARIES
        vnm_terminal::vnm_terminal_app_support)

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

add_library(vnm_terminal_workspace_surface_provider STATIC)
add_library(
    vnm_terminal_workspace::vnm_terminal_workspace_surface_provider
    ALIAS vnm_terminal_workspace_surface_provider)
target_compile_features(
    vnm_terminal_workspace_surface_provider
    PUBLIC cxx_std_20)
qt_add_resources(
    vnm_terminal_workspace_worker_qml_sources
    qml/terminal_worker.qrc)
target_sources(vnm_terminal_workspace_surface_provider
    PRIVATE
        ${vnm_terminal_workspace_worker_qml_sources}
        src/terminal_settlement_receipt_inbox.cpp
        src/terminal_surface_runtime_provider.cpp
        src/terminal_worker_runtime.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/vnm_terminal_workspace/terminal_worker_runtime.h)
target_include_directories(vnm_terminal_workspace_surface_provider
    PRIVATE
        src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vnm_terminal_workspace_surface_provider
    PUBLIC
        vnm_terminal_workspace::vnm_terminal_workspace_request
    PRIVATE
        vnm_framework::vnm_remote_ui_runtime
        vnm_terminal_workspace::_terminal_app_support_dependency)

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

    add_executable(vnm_terminal_workspace_settlement_receipt_tests
        tests/terminal_settlement_receipt_tests.cpp)
    target_include_directories(vnm_terminal_workspace_settlement_receipt_tests
        PRIVATE
            src)
    target_link_libraries(vnm_terminal_workspace_settlement_receipt_tests
        PRIVATE
            vnm_terminal_workspace_surface_provider)
    add_test(
        NAME vnm_terminal_workspace_settlement_receipt_tests
        COMMAND vnm_terminal_workspace_settlement_receipt_tests)
    set_tests_properties(
        vnm_terminal_workspace_settlement_receipt_tests
        PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(vnm_terminal_workspace_worker_runtime_tests
        tests/terminal_worker_runtime_tests.cpp)
    target_include_directories(vnm_terminal_workspace_worker_runtime_tests
        PRIVATE
            src)
    target_link_libraries(vnm_terminal_workspace_worker_runtime_tests
        PRIVATE
            vnm_terminal_workspace::vnm_terminal_workspace_surface_provider)
    add_test(
        NAME vnm_terminal_workspace_worker_runtime_tests
        COMMAND vnm_terminal_workspace_worker_runtime_tests)
    set_tests_properties(
        vnm_terminal_workspace_worker_runtime_tests
        PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(WIN32)
        add_executable(vnm_terminal_workspace_surface_provider_tests
            tests/terminal_surface_runtime_provider_tests.cpp)
        target_include_directories(
            vnm_terminal_workspace_surface_provider_tests
            PRIVATE
                src)
        target_link_libraries(vnm_terminal_workspace_surface_provider_tests
            PRIVATE
                vnm_terminal_workspace::vnm_terminal_workspace_surface_provider
                vnm_terminal_surface::vnm_terminal_surface)
        add_test(
            NAME vnm_terminal_workspace_surface_provider_tests
            COMMAND vnm_terminal_workspace_surface_provider_tests)
        set_tests_properties(
            vnm_terminal_workspace_surface_provider_tests
            PROPERTIES
                TIMEOUT 60
                ENVIRONMENT
                    "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins/platforms"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
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
    TARGETS
        vnm_terminal_workspace_request
        vnm_terminal_workspace_surface_provider
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
