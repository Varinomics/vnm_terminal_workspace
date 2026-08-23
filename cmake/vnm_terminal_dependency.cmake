include(FetchContent)

set(VNM_TERMINAL_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_terminal.")
set(VNM_TERMINAL_SURFACE_SOURCE_DIR "" CACHE PATH
    "Path to the vnm_terminal_surface source used by vnm_terminal.")
set(VNM_QML_CHROME_SOURCE_DIR "" CACHE PATH
    "Path to the vnm_qml_chrome source used by vnm_terminal.")

function(vnm_terminal_workspace_adopt_terminal_target_source)
    get_target_property(
        vnm_terminal_workspace_terminal_is_imported
        vnm_terminal::vnm_terminal_app_support
        IMPORTED)
    if(vnm_terminal_workspace_terminal_is_imported)
        set(VNM_TERMINAL_SOURCE_DIR "" CACHE PATH
            "Path to a source checkout of vnm_terminal." FORCE)
        return()
    endif()

    get_target_property(
        vnm_terminal_workspace_terminal_source_dir
        vnm_terminal::vnm_terminal_app_support
        SOURCE_DIR)
    if(vnm_terminal_workspace_terminal_source_dir)
        set(VNM_TERMINAL_SOURCE_DIR
            "${vnm_terminal_workspace_terminal_source_dir}"
            CACHE PATH
            "Path to a source checkout of vnm_terminal."
            FORCE)
    endif()
endfunction()

function(vnm_terminal_workspace_make_terminal_available)
    if(TARGET vnm_terminal::vnm_terminal_app_support)
        vnm_terminal_workspace_adopt_terminal_target_source()
        return()
    endif()

    if(NOT VNM_TERMINAL_SOURCE_DIR)
        find_package(vnm_terminal CONFIG QUIET)
        if(TARGET vnm_terminal::vnm_terminal_app_support)
            vnm_terminal_workspace_adopt_terminal_target_source()
            return()
        endif()
    endif()

    if(VNM_TERMINAL_SOURCE_DIR)
        if(NOT EXISTS "${VNM_TERMINAL_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "VNM_TERMINAL_SOURCE_DIR does not contain the vnm_terminal "
                "CMakeLists.txt: ${VNM_TERMINAL_SOURCE_DIR}")
        endif()
        set(VNM_TERMINAL_BUILD_STANDALONE_APP OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            "${VNM_TERMINAL_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/_deps/vnm_terminal-build"
            EXCLUDE_FROM_ALL)
    else()
        set(VNM_TERMINAL_BUILD_STANDALONE_APP OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(vnm_terminal_workspace_terminal
            GIT_REPOSITORY https://github.com/Varinomics/vnm_terminal.git
            GIT_TAG master
            GIT_SHALLOW FALSE)
        FetchContent_MakeAvailable(vnm_terminal_workspace_terminal)
        set(VNM_TERMINAL_SOURCE_DIR
            "${vnm_terminal_workspace_terminal_SOURCE_DIR}"
            CACHE PATH
            "Path to a source checkout of vnm_terminal."
            FORCE)
    endif()

    if(NOT TARGET vnm_terminal::vnm_terminal_app_support OR
       NOT TARGET vnm_terminal_surface::vnm_terminal_surface)
        message(FATAL_ERROR
            "vnm_terminal_workspace requires the neutral app-support and "
            "semantic surface targets from vnm_terminal.")
    endif()
endfunction()
