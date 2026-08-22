cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_workspace_install_consumer LANGUAGES CXX)

find_package(vnm_terminal_workspace CONFIG REQUIRED)

add_executable(vnm_terminal_workspace_install_consumer main.cpp)
target_link_libraries(vnm_terminal_workspace_install_consumer
    PRIVATE
        vnm_terminal_workspace::vnm_terminal_workspace_request)
