include(FetchContent)

set(VNM_FRAMEWORK_SOURCE_DIR
    ""
    CACHE PATH "Optional path to a local vnm_framework checkout")

function(vnm_terminal_workspace_make_environment_policy_available)
    if(TARGET vnm_framework::vnm_environment_policy AND
       TARGET vnm_framework::vnm_remote_ui_runtime)
        return()
    endif()

    if(NOT VNM_FRAMEWORK_SOURCE_DIR)
        find_package(vnm_framework CONFIG QUIET)
        if(TARGET vnm_framework::vnm_environment_policy AND
           TARGET vnm_framework::vnm_remote_ui_runtime)
            return()
        endif()
    endif()

    set(VNM_FRAMEWORK_BUILD_AGGREGATE OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_TEXT OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_MANAGER OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_QML_SHELL OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_QML_RUNTIME OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_SURFACE ON CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_CONTROL ON CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_WORKER_RUNTIME ON CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_APP_INIT OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_ENABLE_REMOTE_UI_RUNTIME ON CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_REMOTE_UI_RUNTIME ON CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_ENABLE_REMOTE_CONTROL OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_ENABLE_QML_CHROME OFF CACHE BOOL "" FORCE)

    if(VNM_FRAMEWORK_SOURCE_DIR)
        if(NOT EXISTS "${VNM_FRAMEWORK_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "VNM_FRAMEWORK_SOURCE_DIR does not contain vnm_framework: "
                "${VNM_FRAMEWORK_SOURCE_DIR}")
        endif()
        FetchContent_Declare(vnm_terminal_workspace_framework
            SOURCE_DIR "${VNM_FRAMEWORK_SOURCE_DIR}"
            EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(vnm_terminal_workspace_framework
            GIT_REPOSITORY https://github.com/imakris/vnm_framework.git
            GIT_TAG master
            GIT_SHALLOW FALSE
            EXCLUDE_FROM_ALL)
    endif()
    FetchContent_MakeAvailable(vnm_terminal_workspace_framework)

    if(NOT TARGET vnm_framework::vnm_environment_policy)
        message(FATAL_ERROR
            "vnm_framework did not provide vnm_framework::vnm_environment_policy")
    endif()
    if(NOT TARGET vnm_framework::vnm_remote_ui_runtime)
        message(FATAL_ERROR
            "vnm_framework did not provide vnm_framework::vnm_remote_ui_runtime")
    endif()
endfunction()
