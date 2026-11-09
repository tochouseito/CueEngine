cmake_minimum_required(VERSION 4.2.0)

# ゲームを Build の起点とし、Engine の実行 Target はこの Project に取り込まない
project(CueGame LANGUAGES CXX)

if(NOT DEFINED CUE_ENGINE_ROOT OR CUE_ENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR "Select the Engine SDK in CueEditor, or set CUE_ENGINE_ROOT in a local CMake preset")
endif()

# Editor からの指定を優先し、単独利用では曖昧でない一つの Project だけを選ぶ
if(NOT DEFINED CUE_GAME_PROJECT OR CUE_GAME_PROJECT STREQUAL "")
    file(GLOB taskProjectFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cueproj")
    list(LENGTH taskProjectFiles taskProjectCount)
    if(NOT taskProjectCount EQUAL 1)
        message(FATAL_ERROR "Set CUE_GAME_PROJECT when the game folder does not contain exactly one .cueproj")
    endif()
    list(GET taskProjectFiles 0 CUE_GAME_PROJECT)
endif()

include("${CUE_ENGINE_ROOT}/cmake/CueGameSdk.cmake")
cue_add_standalone_game_project("${CUE_GAME_PROJECT}")
