# 通常の Engine 開発と、Editor が生成するゲーム用 Solution の名前を分ける
set(CUE_GAME_SOLUTION_NAME "" CACHE STRING "Application identifier for a game Visual Studio solution")
set(CUE_SOLUTION_NAME CueEngine)
if(NOT CUE_GAME_SOLUTION_NAME STREQUAL "")
    string(LENGTH "${CUE_GAME_SOLUTION_NAME}" taskSolutionLength)
    string(TOLOWER "${CUE_GAME_SOLUTION_NAME}" taskSolutionLower)
    if(CUE_GAME_PROJECT STREQUAL "" OR NOT DEFINED CUE_GAME_PROJECT
       OR taskSolutionLength GREATER 128 OR NOT CUE_GAME_SOLUTION_NAME MATCHES "^[A-Za-z0-9_-]+$"
       OR taskSolutionLower MATCHES "^(con|prn|aux|nul|com[1-9]|lpt[1-9])$")
        message(FATAL_ERROR "Cue game solution name requires a project and a valid application identifier")
    endif()
    set(CUE_SOLUTION_NAME "${CUE_GAME_SOLUTION_NAME}")
endif()

# Host の既定 Target が定義された後で、ゲーム用 Solution の起点を確定する
function(cue_set_game_solution_startup)
    if(CUE_GAME_SOLUTION_NAME STREQUAL "")
        return()
    endif()
    get_property(taskGameTarget GLOBAL PROPERTY CUE_CONFIGURED_GAME_TARGET)
    if("${taskGameTarget}" STREQUAL "" OR NOT TARGET "${taskGameTarget}")
        message(FATAL_ERROR "Cue game solution has no configured game target")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT "${taskGameTarget}")
endfunction()
