cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_workspace_install_consumer LANGUAGES CXX)

find_package(Qt6 6.11 REQUIRED COMPONENTS Gui)
find_package(vnm_terminal_workspace CONFIG REQUIRED)

add_executable(
    vnm_terminal_workspace_install_consumer
    ../runtime_consumer_main.cpp)
target_link_libraries(vnm_terminal_workspace_install_consumer
    PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_surface_provider
        vnm_terminal_workspace::vnm_terminal_workspace_owner_client
        Qt6::Gui)
