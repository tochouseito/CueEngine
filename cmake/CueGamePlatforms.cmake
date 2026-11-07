include_guard(GLOBAL)

# 選択は Project の永続設定ではなく Configure / Build 要求に属する
set(CUE_GAME_PLATFORM "" CACHE STRING "Explicit game output platform")
set(CUE_GAME_ARCHITECTURE "" CACHE STRING "Explicit game output architecture")
set_property(CACHE CUE_GAME_PLATFORM PROPERTY STRINGS Windows)
set_property(CACHE CUE_GAME_ARCHITECTURE PROPERTY STRINGS x64)

# 現在提供する実装だけを登録し、未知の要求を Native Platform に置き換えない
include("${CMAKE_CURRENT_LIST_DIR}/Platforms/WindowsGame.cmake")

# 選択した Platform の実装が Toolchain・拡張子・Host を提供する
function(cue_select_game_build_target)
    if(CUE_GAME_PLATFORM STREQUAL "" OR CUE_GAME_ARCHITECTURE STREQUAL "")
        message(FATAL_ERROR "Cue game builds require explicit CUE_GAME_PLATFORM and CUE_GAME_ARCHITECTURE")
    endif()
    get_property(taskPlatforms GLOBAL PROPERTY CUE_GAME_SUPPORTED_PLATFORMS)
    if(NOT CUE_GAME_PLATFORM IN_LIST taskPlatforms)
        message(FATAL_ERROR "Unsupported Cue game platform: ${CUE_GAME_PLATFORM}")
    endif()
    get_property(taskArchitectures GLOBAL PROPERTY "CUE_GAME_${CUE_GAME_PLATFORM}_ARCHITECTURES")
    if(NOT CUE_GAME_ARCHITECTURE IN_LIST taskArchitectures)
        message(FATAL_ERROR "Unsupported Cue game architecture: ${CUE_GAME_PLATFORM}/${CUE_GAME_ARCHITECTURE}")
    endif()
    get_property(taskProvider GLOBAL PROPERTY "CUE_GAME_${CUE_GAME_PLATFORM}_PROVIDER")
    cmake_language(CALL "${taskProvider}")
    foreach(taskValue IN ITEMS MODULE_SUFFIX STATIC_SUFFIX APPLICATION_SUFFIX HOST_TARGET MAIN_SOURCE)
        get_property(taskSelected GLOBAL PROPERTY "CUE_GAME_${CUE_GAME_PLATFORM}_${taskValue}")
        set("_CUE_GAME_${taskValue}" "${taskSelected}" PARENT_SCOPE)
    endforeach()
    set(_CUE_GAME_PLATFORM "${CUE_GAME_PLATFORM}" PARENT_SCOPE)
    set(_CUE_GAME_ARCHITECTURE "${CUE_GAME_ARCHITECTURE}" PARENT_SCOPE)
endfunction()

# Platform 固有の Entry Point / Link / Runtime 配布設定へ委譲する
function(cue_add_selected_game_application a_target a_gameStatic a_runtimeDirectory)
    get_property(taskFactory GLOBAL PROPERTY "CUE_GAME_${_CUE_GAME_PLATFORM}_APPLICATION_FACTORY")
    cmake_language(CALL "${taskFactory}" "${a_target}" "${a_gameStatic}" "${a_runtimeDirectory}")
endfunction()
