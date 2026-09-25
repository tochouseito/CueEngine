cmake_minimum_required(VERSION 4.2.0)

project(CueEngine LANGUAGES CXX)

# Debug は Debug Runtime、Development と Release は Release Runtime を使う
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # 各 C++ Target の Source を MSVC で並列 Compile する
    add_compile_options(/MP)
endif()

# Test の有無を BUILD_TESTING で切り替え、Source の構成は共通に保つ
include(CTest)

add_subdirectory(Engine/Source)

if(BUILD_TESTING)
    add_subdirectory(Engine/Tests)
endif()
