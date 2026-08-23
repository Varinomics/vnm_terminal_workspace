cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_workspace_source_consumer LANGUAGES CXX)

find_package(Qt6 6.11 REQUIRED COMPONENTS Gui)

if(NOT VNM_TERMINAL_WORKSPACE_SOURCE_DIR)
    message(FATAL_ERROR "VNM_TERMINAL_WORKSPACE_SOURCE_DIR is required")
endif()

set(VNM_TERMINAL_WORKSPACE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${VNM_TERMINAL_WORKSPACE_SOURCE_DIR}"
    vnm_terminal_workspace)

add_executable(
    vnm_terminal_workspace_source_consumer
    ../runtime_consumer_main.cpp)
target_link_libraries(vnm_terminal_workspace_source_consumer
    PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_surface_provider
        vnm_terminal_workspace::vnm_terminal_workspace_owner_client
        Qt6::Gui)
