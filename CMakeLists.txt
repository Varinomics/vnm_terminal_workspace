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
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network Qml Quick)
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
        src/terminal_worker_composition.cpp
        src/terminal_worker_envelope.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/vnm_terminal_workspace/terminal_launch_request.h
            include/vnm_terminal_workspace/terminal_worker_envelope.h)
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

add_library(vnm_terminal_workspace_owner_core OBJECT
    src/terminal_hosted_owner.cpp
    src/terminal_owner_core.cpp
    src/terminal_owner_proxy_gate.cpp
    src/terminal_settlement_receipt_inbox.cpp)
target_compile_features(vnm_terminal_workspace_owner_core PUBLIC cxx_std_20)
target_include_directories(vnm_terminal_workspace_owner_core
    PRIVATE
        include
        src)
target_link_libraries(vnm_terminal_workspace_owner_core
    PUBLIC
        vnm_terminal_workspace::vnm_terminal_workspace_request
        vnm_framework::vnm_remote_ui_runtime)

add_library(vnm_terminal_workspace_owner STATIC
    src/terminal_owner_host.cpp
    $<TARGET_OBJECTS:vnm_terminal_workspace_owner_core>)
add_library(
    vnm_terminal_workspace::vnm_terminal_workspace_owner
    ALIAS vnm_terminal_workspace_owner)
target_compile_features(vnm_terminal_workspace_owner PUBLIC cxx_std_20)
target_sources(vnm_terminal_workspace_owner
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/vnm_terminal_workspace/terminal_owner_host.h)
target_include_directories(vnm_terminal_workspace_owner
    PRIVATE
        src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vnm_terminal_workspace_owner
    PUBLIC
        vnm_terminal_workspace::vnm_terminal_workspace_request
    PRIVATE
        vnm_framework::vnm_remote_ui_runtime
        Qt6::Core)

add_library(vnm_terminal_workspace_owner_client STATIC
    src/terminal_owner_client.cpp
    src/terminal_owner_process_identity.cpp
    src/terminal_owner_wire.cpp)
add_library(
    vnm_terminal_workspace::vnm_terminal_workspace_owner_client
    ALIAS vnm_terminal_workspace_owner_client)
target_compile_features(
    vnm_terminal_workspace_owner_client
    PUBLIC cxx_std_20)
target_sources(vnm_terminal_workspace_owner_client
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/vnm_terminal_workspace/terminal_owner_client.h)
target_sources(vnm_terminal_workspace_owner_client
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/vnm_terminal_workspace/terminal_worker_composition.h)
target_include_directories(vnm_terminal_workspace_owner_client
    PRIVATE
        src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vnm_terminal_workspace_owner_client
    PUBLIC
        vnm_terminal_workspace::vnm_terminal_workspace_owner
    PRIVATE
        Qt6::Network)

add_library(vnm_terminal_workspace_owner_service STATIC
    src/terminal_owner_process_identity.cpp
    src/terminal_owner_service_main.cpp
    src/terminal_owner_wire.cpp)
add_library(
    vnm_terminal_workspace::vnm_terminal_workspace_owner_service
    ALIAS vnm_terminal_workspace_owner_service)
target_compile_features(vnm_terminal_workspace_owner_service PUBLIC cxx_std_20)
target_include_directories(vnm_terminal_workspace_owner_service
    PRIVATE src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vnm_terminal_workspace_owner_service
    PUBLIC
        vnm_terminal_workspace::vnm_terminal_workspace_owner
    PRIVATE
        vnm_framework::vnm_remote_ui_runtime
        Qt6::Core
        Qt6::Network)

add_executable(vnm_terminal_workspace_owner_host
    src/terminal_owner_service_executable_main.cpp)
target_compile_features(vnm_terminal_workspace_owner_host PRIVATE cxx_std_20)
target_link_libraries(vnm_terminal_workspace_owner_host
    PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_owner_service)

set(_vnm_terminal_workspace_worker_output_dir
    "${CMAKE_CURRENT_BINARY_DIR}/bin/terminal_worker")
add_library(vnm_terminal_workspace_terminal_worker SHARED
    src/terminal_worker_plugin.cpp)
target_compile_features(
    vnm_terminal_workspace_terminal_worker
    PRIVATE cxx_std_20)
target_compile_definitions(
    vnm_terminal_workspace_terminal_worker
    PRIVATE VNM_PLUGIN_EXPORTS)
target_link_libraries(vnm_terminal_workspace_terminal_worker
    PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_surface_provider
        vnm_framework::vnm_remote_ui_runtime
        Qt6::Core)
set_target_properties(vnm_terminal_workspace_terminal_worker
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${_vnm_terminal_workspace_worker_output_dir}"
        LIBRARY_OUTPUT_DIRECTORY
            "${_vnm_terminal_workspace_worker_output_dir}")
file(MAKE_DIRECTORY "${_vnm_terminal_workspace_worker_output_dir}")
configure_file(
    src/vnm_terminal_workspace_worker_package.json
    "${_vnm_terminal_workspace_worker_output_dir}/vnm_package.json"
    COPYONLY)

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
            vnm_terminal_workspace_owner_core)
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
        vnm_terminal_deploy_conpty(vnm_terminal_workspace_surface_provider_tests)
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

    add_executable(vnm_terminal_workspace_remote_input_tests
        tests/terminal_remote_input_tests.cpp)
    target_include_directories(vnm_terminal_workspace_remote_input_tests PRIVATE
        include src)
    target_link_libraries(vnm_terminal_workspace_remote_input_tests PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_request
        vnm_framework::vnm_remote_ui_common)
    add_test(NAME vnm_terminal_workspace_remote_input_tests
        COMMAND vnm_terminal_workspace_remote_input_tests)
    set_tests_properties(vnm_terminal_workspace_remote_input_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(vnm_terminal_workspace_owner_input_admission_tests
        tests/terminal_owner_input_admission_tests.cpp)
    target_link_libraries(vnm_terminal_workspace_owner_input_admission_tests PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_owner
        vnm_framework::vnm_remote_ui_runtime Qt6::Core)
    add_test(NAME vnm_terminal_workspace_owner_input_admission_tests
        COMMAND vnm_terminal_workspace_owner_input_admission_tests)
    set_tests_properties(vnm_terminal_workspace_owner_input_admission_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(vnm_terminal_workspace_owner_core_tests
        tests/terminal_owner_core_tests.cpp)
    target_include_directories(vnm_terminal_workspace_owner_core_tests
        PRIVATE
            include
            src)
    target_link_libraries(vnm_terminal_workspace_owner_core_tests
        PRIVATE
            vnm_terminal_workspace_owner_core)
    add_test(
        NAME vnm_terminal_workspace_owner_core_tests
        COMMAND vnm_terminal_workspace_owner_core_tests)
    set_tests_properties(
        vnm_terminal_workspace_owner_core_tests
        PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(WIN32)
        # The terminal worker DLL hosts its terminal inside the framework's
        # vnm_hosted_worker_host process, so the packaged ConPTY runtime must
        # sit beside that executable. The framework target is created in the
        # FetchContent sub-build directory, which POST_BUILD staging cannot
        # reach; DESTINATION exists for exactly this cross-directory case.
        get_property(_vnm_tw_conpty_destination_supported
            GLOBAL PROPERTY vnm_terminal_conpty_deploy_destination_supported)
        if(NOT _vnm_tw_conpty_destination_supported)
            message(FATAL_ERROR
                "vnm_terminal_workspace hosted tests require the ConPTY "
                "runtime beside vnm_hosted_worker_host, but the resolved "
                "vnm_terminal_surface does not support DESTINATION staging")
        endif()
        vnm_terminal_deploy_conpty(vnm_hosted_worker_host
            DESTINATION
                "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_workspace_framework-build")

        add_executable(vnm_terminal_workspace_hosted_owner_tests
            tests/terminal_hosted_owner_tests.cpp)
        target_include_directories(
            vnm_terminal_workspace_hosted_owner_tests
            PRIVATE
                include
                src)
        target_compile_definitions(
            vnm_terminal_workspace_hosted_owner_tests
            PRIVATE
                VNM_TW_TEST_HOST_PATH="$<TARGET_FILE:vnm_hosted_worker_host>"
                VNM_TW_TEST_WORKER_PATH="$<TARGET_FILE:vnm_terminal_workspace_terminal_worker>")
        target_link_libraries(
            vnm_terminal_workspace_hosted_owner_tests
            PRIVATE
                vnm_terminal_workspace::vnm_terminal_workspace_owner
                vnm_framework::vnm_remote_ui_runtime
                Qt6::Core)
        add_dependencies(
            vnm_terminal_workspace_hosted_owner_tests
            vnm_hosted_worker_host
            vnm_terminal_workspace_terminal_worker)
        add_test(
            NAME vnm_terminal_workspace_hosted_owner_tests
            COMMAND vnm_terminal_workspace_hosted_owner_tests)
        set_tests_properties(
            vnm_terminal_workspace_hosted_owner_tests
            PROPERTIES
                TIMEOUT 60
                ENVIRONMENT
                    "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins/platforms"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

        add_executable(vnm_terminal_workspace_owner_client_tests
            tests/terminal_owner_client_tests.cpp)
        target_include_directories(
            vnm_terminal_workspace_owner_client_tests
            PRIVATE src)
        target_compile_definitions(
            vnm_terminal_workspace_owner_client_tests
            PRIVATE
                VNM_TW_TEST_OWNER_PATH="$<TARGET_FILE:vnm_terminal_workspace_owner_host>"
                VNM_TW_TEST_HOST_PATH="$<TARGET_FILE:vnm_hosted_worker_host>"
                VNM_TW_TEST_WORKER_PATH="$<TARGET_FILE:vnm_terminal_workspace_terminal_worker>")
        target_link_libraries(
            vnm_terminal_workspace_owner_client_tests
            PRIVATE
                vnm_terminal_workspace::vnm_terminal_workspace_owner_client
                vnm_framework::vnm_surface
                Qt6::Core
                Qt6::Network)
        add_dependencies(
            vnm_terminal_workspace_owner_client_tests
            vnm_terminal_workspace_owner_host
            vnm_hosted_worker_host
            vnm_terminal_workspace_terminal_worker)
        add_test(
            NAME vnm_terminal_workspace_owner_client_tests
            COMMAND vnm_terminal_workspace_owner_client_tests)
        set_tests_properties(
            vnm_terminal_workspace_owner_client_tests
            PROPERTIES
                TIMEOUT 90
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
        vnm_terminal_workspace_owner
        vnm_terminal_workspace_owner_client
        vnm_terminal_workspace_owner_service
    EXPORT vnm_terminal_workspace_targets
    FILE_SET HEADERS
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
    TARGETS vnm_terminal_workspace_owner_host
    RUNTIME DESTINATION
        "${CMAKE_INSTALL_BINDIR}/vnm_terminal_workspace")
install(
    TARGETS vnm_terminal_workspace_terminal_worker
    RUNTIME DESTINATION
        "${CMAKE_INSTALL_BINDIR}/vnm_terminal_workspace/terminal_worker"
    LIBRARY DESTINATION
        "${CMAKE_INSTALL_BINDIR}/vnm_terminal_workspace/terminal_worker")
install(
    FILES src/vnm_terminal_workspace_worker_package.json
    DESTINATION
        "${CMAKE_INSTALL_BINDIR}/vnm_terminal_workspace/terminal_worker"
    RENAME vnm_package.json)
install(
    EXPORT vnm_terminal_workspace_targets
    NAMESPACE vnm_terminal_workspace::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_workspace")
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_workspace-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_workspace-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_workspace")
