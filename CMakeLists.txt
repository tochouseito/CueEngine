cmake_minimum_required(VERSION 4.2.0)

project(CueEngine LANGUAGES CXX)

# DebugはDebug Runtime、DevelopmentとReleaseはRelease Runtimeを使う
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # 各 C++ Target の Source を MSVC で並列 Compile する
    add_compile_options(/MP)
endif()

include(CTest)

add_subdirectory(Engine/Source)

if(BUILD_TESTING)
    add_subdirectory(Engine/Tests)
endif()
