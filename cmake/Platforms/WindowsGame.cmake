# 出力形式と起動入口は Windows 実装の登録情報に閉じる
set_property(GLOBAL APPEND PROPERTY CUE_GAME_SUPPORTED_PLATFORMS Windows)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_ARCHITECTURES x64)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_PROVIDER cue_validate_windows_game_toolchain)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_MODULE_SUFFIX .dll)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_STATIC_SUFFIX .lib)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_APPLICATION_SUFFIX .exe)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_HOST_TARGET Cue.GameHost.Windows)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_MAIN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../Engine/Source/GameHost/Windows/Main.cpp")
set_property(GLOBAL PROPERTY CUE_GAME_Windows_APPLICATION_FACTORY cue_add_windows_game_application)
set_property(GLOBAL PROPERTY CUE_GAME_Windows_RUNTIME_FILES
    dxcompiler.dll dxil.dll "$<$<NOT:$<CONFIG:Release>>:WinPixEventRuntime.dll>")
set_property(GLOBAL PROPERTY CUE_GAME_Windows_LICENSE_FILES
    "$<$<NOT:$<CONFIG:Release>>:WinPixEventRuntime-MIT.txt>"
    "$<$<NOT:$<CONFIG:Release>>:WinPixEventRuntime-ThirdPartyNotices.txt>"
    DirectXShaderCompiler-MS.txt DirectXShaderCompiler-LLVM.txt
    DirectXShaderCompiler-MIT.txt DirectXShaderCompiler-HLSL.txt)

# Editor を実行する OS と別に、選択した出力対象とコンパイラの一致を検証する
function(cue_validate_windows_game_toolchain)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
       OR (DEFINED CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL ""
           AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
       OR (DEFINED CMAKE_CXX_COMPILER_ARCHITECTURE_ID AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL ""
           AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"))
        message(FATAL_ERROR "The selected Windows/x64 game target requires a compatible Windows x64 toolchain")
    endif()
endfunction()

# 静的なゲーム登録を持つ Windows アプリを生成し、Editor / Play DLL に依存させない
function(cue_add_windows_game_application a_target a_gameStatic a_runtimeDirectory)
    get_property(taskMain GLOBAL PROPERTY CUE_GAME_Windows_MAIN_SOURCE)
    if(NOT TARGET Cue.GameHost.Windows OR NOT EXISTS "${taskMain}")
        message(FATAL_ERROR "The selected Windows game host is not available")
    endif()
    add_executable(${a_target} WIN32 "${taskMain}")
    target_link_libraries(${a_target} PRIVATE Cue.GameHost.Windows ${a_gameStatic})
    target_compile_features(${a_target} PRIVATE cxx_std_20)
    target_compile_definitions(${a_target} PRIVATE
        CUE_GAME_PLATFORM="${_CUE_GAME_PLATFORM}"
        CUE_GAME_ARCHITECTURE="${_CUE_GAME_ARCHITECTURE}"
        CUE_GAME_CONFIGURATION="$<CONFIG>"
        $<$<CONFIG:Release>:CUE_SHIPPING=1>)
    set_target_properties(${a_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${a_runtimeDirectory}"
        PREFIX "" SUFFIX ".exe"
        DEBUG_POSTFIX "" DEVELOPMENT_POSTFIX "" RELEASE_POSTFIX "")
    if(MSVC)
        target_compile_options(${a_target} PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
        target_link_options(${a_target} PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()
    cue_copy_shader_runtime(${a_target})
endfunction()
