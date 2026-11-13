# ゲーム Folder を Root にした実 SDK 利用を、通常の Engine Build Tree と分離して検証する
foreach(taskKey IN ITEMS CUE_SOURCE_ROOT CUE_FIXTURE_ROOT CUE_GENERATOR CUE_CONFIGURATION)
    if(NOT DEFINED ${taskKey} OR "${${taskKey}}" STREQUAL "")
        message(FATAL_ERROR "Game SDK test argument is missing: ${taskKey}")
    endif()
endforeach()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef taskNonce)
set(taskGame "${CUE_FIXTURE_ROOT}/Standalone-${taskNonce}/日本語 Game")
set(taskTree "${taskGame}/Intermediate/Standalone")
file(MAKE_DIRECTORY "${taskGame}/Source" "${taskGame}/Content/Scenes")
file(READ "${CUE_SOURCE_ROOT}/Engine/Version.json" taskVersionJson)
string(JSON taskVersion GET "${taskVersionJson}" version)
file(WRITE "${taskGame}/Standalone.cueproj"
    "formatVersion=2\nprojectId=8f533c49-d416-47ea-98c1-026822b1e6ed\nname=SDK Game\napplicationName=SdkGame\nsourceRoot=Source\ncontentRoot=Content\nstartupScene=Scenes/Start.cuescene\ngameTarget=SdkGameModule\nengineVersion=${taskVersion}\n")
file(COPY_FILE "${CUE_SOURCE_ROOT}/Templates/Empty3D/Source/EmptyBehaviour.cpp" "${taskGame}/Source/EmptyBehaviour.cpp")
file(COPY_FILE "${CUE_SOURCE_ROOT}/Templates/Empty3D/Source/ScriptSources.cueide" "${taskGame}/Source/ScriptSources.cueide")
file(APPEND "${taskGame}/Source/ScriptSources.cueide" "# Comment: ; $ < >\n")
file(WRITE "${taskGame}/Source/CMakeLists.txt" [=[
if(NOT PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR OR NOT PROJECT_BINARY_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "The game Source inherited the SDK root instead of its own project root")
endif()
cue_add_game_scripts(SdkGameModule)
]=])
file(WRITE "${taskGame}/Content/Scenes/Start.cuescene" "Fixture scene\n")
set(taskBootstrap [=[
cmake_minimum_required(VERSION 4.2)
project(SdkGame LANGUAGES CXX)
set(CMAKE_CONFIGURATION_TYPES Debug Development Release)
include("${CUE_ENGINE_ROOT}/cmake/CueGameSdk.cmake")
cue_add_standalone_game_project("${CMAKE_SOURCE_DIR}/Standalone.cueproj")
add_executable(ModuleMetadataProbe Probe.cpp)
target_link_libraries(ModuleMetadataProbe PRIVATE Cue.Script SdkGameModule.Static)
if(NOT PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "The SDK changed the game project root")
endif()
foreach(taskForbidden IN ITEMS CueEditorHost Cue.EditorHost Cue.EditorUi Cue.Editor.Ui Cue.EditorIde CuePlayHost CueProjectTool)
    if(TARGET "${taskForbidden}")
        message(FATAL_ERROR "An Editor or Tool target leaked into the independent game")
    endif()
endforeach()
if(NOT TARGET Cue.Script OR NOT TARGET SdkGameModule OR NOT TARGET SdkGameModule.Static)
    message(FATAL_ERROR "The game API or module is unavailable")
endif()
if(CUE_GAME_BUILD_APP)
    if(NOT TARGET SdkGame OR NOT TARGET Cue.GameHost.Windows)
        message(FATAL_ERROR "The independent App target is missing")
    endif()
elseif(TARGET SdkGame OR TARGET Cue.GameHost.Windows OR TARGET Cue.Renderer.DX12)
    message(FATAL_ERROR "Script editing unexpectedly builds the App or graphics runtime")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/GameRoot.txt" "${CMAKE_SOURCE_DIR}")
]=])
file(WRITE "${taskGame}/CMakeLists.txt" "${taskBootstrap}")
file(WRITE "${taskGame}/Probe.cpp" [=[
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <Script/Abi.h>
extern "C" CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(std::uint32_t, CueScriptModuleApi *) noexcept;
int wmain(int count, wchar_t **arguments)
{
    HMODULE library = count == 2 ? LoadLibraryW(arguments[1]) : nullptr;
    if (count == 2 && !library) return 1;
    auto entry = library ? reinterpret_cast<CueScriptGetModuleFunction>(GetProcAddress(library, "CueScriptGetModule"))
                         : &CueScriptGetModule;
    CueScriptModuleApi api{};
    const bool valid = entry && entry(CUE_SCRIPT_ABI_VERSION, &api).code == 0 && api.type_count(api.context) == 2;
    if (library) FreeLibrary(library);
    return valid ? 0 : 2;
}
]=])
set(taskConfigure "${CMAKE_COMMAND}" -S "${taskGame}" -B "${taskTree}" -G "${CUE_GENERATOR}"
    "-DCUE_ENGINE_ROOT=${CUE_SOURCE_ROOT}" "-DCUE_GAME_PLATFORM=Windows" "-DCUE_GAME_ARCHITECTURE=x64")
if(DEFINED CUE_GENERATOR_PLATFORM AND NOT CUE_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND taskConfigure -A "${CUE_GENERATOR_PLATFORM}")
endif()
execute_process(COMMAND ${taskConfigure} RESULT_VARIABLE taskResult OUTPUT_VARIABLE taskOutput ERROR_VARIABLE taskError)
if(NOT taskResult EQUAL 0)
    message(FATAL_ERROR "Standalone game SDK configure failed: ${taskOutput}\n${taskError}")
endif()
file(READ "${taskTree}/GameRoot.txt" taskActualRoot)
if(NOT taskActualRoot STREQUAL taskGame)
    message(FATAL_ERROR "The game is not the top-level CMake project")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${taskTree}" --config "${CUE_CONFIGURATION}"
    --target Cue.GameProjectBuild RESULT_VARIABLE taskResult OUTPUT_VARIABLE taskOutput ERROR_VARIABLE taskError)
if(NOT taskResult EQUAL 0)
    message(FATAL_ERROR "Standalone game SDK build failed: ${taskOutput}\n${taskError}")
endif()
foreach(taskOutput IN ITEMS Runtime/SdkGameModule.dll Runtime/SdkGameModule.pdb Libraries/SdkGameModuleStatic.lib GameModule.cuebuild)
    if(NOT EXISTS "${taskGame}/Build/Windows/x64/${CUE_CONFIGURATION}/${taskOutput}")
        message(FATAL_ERROR "Standalone game SDK output is missing: ${taskOutput}")
    endif()
endforeach()

# 対応表への追加で再構成し、単独 cpp と Header を分けた Script の登録が DLL / Static の双方に残ることを確認する
file(WRITE "${taskGame}/Source/SecondScript.h" [=[
#pragma once

#include <Foundation/Result.h>
#include <Script/Behaviour.h>

namespace game
{
class SecondScript final : public ::cue::Behaviour
{
  public:
    /// @brief 生成した Script の更新入口を提供する
    [[nodiscard]] ::cue::Result<void> update() override;
};
} // namespace game
]=])
file(WRITE "${taskGame}/Source/SecondScript.cpp" [=[
#include "SecondScript.h"

#include <Script/ModuleEntry.h>

::cue::Result<void> game::SecondScript::update()
{
    return ::cue::Result<void>::success();
}

CUE_DEFINE_SCRIPT(SecondScript,
                  ::cue::make_script_registration<::game::SecondScript>({"game.SecondScript", "Second Script", 1, {}}))
]=])
file(APPEND "${taskGame}/Source/ScriptSources.cueide"
    "script.game.SecondScript=SecondScript.cpp\nregistration.game.SecondScript=SecondScript\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${taskTree}" --config "${CUE_CONFIGURATION}"
    --target Cue.GameProjectBuild ModuleMetadataProbe RESULT_VARIABLE taskResult OUTPUT_VARIABLE taskOutput ERROR_VARIABLE taskError)
if(NOT taskResult EQUAL 0)
    message(FATAL_ERROR "Adding a second script did not regenerate its module: ${taskOutput}\n${taskError}")
endif()
foreach(taskMode IN ITEMS static dll)
    set(taskProbe "${taskTree}/${CUE_CONFIGURATION}/ModuleMetadataProbe.exe")
    set(taskArguments)
    if(taskMode STREQUAL "dll")
        list(APPEND taskArguments "${taskGame}/Build/Windows/x64/${CUE_CONFIGURATION}/Runtime/SdkGameModule.dll")
    endif()
    execute_process(COMMAND "${taskProbe}" ${taskArguments} RESULT_VARIABLE taskResult TIMEOUT 10)
    if(NOT taskResult EQUAL 0)
        message(FATAL_ERROR "Multiple script registrations were lost in ${taskMode}")
    endif()
endforeach()
# 不正な対応表を Code として実行せず、生成前に拒否する
file(READ "${taskGame}/Source/ScriptSources.cueide" taskValidCatalog)
foreach(taskInvalid IN ITEMS "registration.game.SecondScript=Invalid;Token\n" "unknown=value\n"
        "registration.game.SecondScript=EmptyBehaviour\n")
    file(WRITE "${taskGame}/Source/ScriptSources.cueide" "${taskValidCatalog}${taskInvalid}")
    execute_process(COMMAND ${taskConfigure} RESULT_VARIABLE taskRejected OUTPUT_QUIET ERROR_QUIET)
    if(taskRejected EQUAL 0)
        message(FATAL_ERROR "An invalid script source map was accepted")
    endif()
endforeach()
file(WRITE "${taskGame}/Source/ScriptSources.cueide" "${taskValidCatalog}")

# 同じ SDK で App を構成し、Editor の Target を生成しないことも確認する
if(NOT DEFINED CUE_PIX_ARCHIVE OR CUE_PIX_ARCHIVE STREQUAL "")
    file(STRINGS "${CUE_SOURCE_ROOT}/ThirdParty/WinPixEventRuntime.cmake" taskPixDefinition
        REGEX "^set\\(CUE_PIX_VERSION ")
    if(taskPixDefinition MATCHES "\"([^\"]+)\"")
        set(taskPixName "WinPixEventRuntime-${CMAKE_MATCH_1}.zip")
        foreach(taskPixCandidate IN ITEMS "${CUE_SOURCE_ROOT}/ThirdParty/Packages/${taskPixName}"
                "${CUE_SOURCE_ROOT}/out/build/windows-vs2026/ThirdParty/${taskPixName}")
            if(EXISTS "${taskPixCandidate}")
                set(CUE_PIX_ARCHIVE "${taskPixCandidate}")
                break()
            endif()
        endforeach()
    endif()
endif()
foreach(taskArchive IN ITEMS CUE_PIX_ARCHIVE CUE_DXC_ARCHIVE)
    if(DEFINED ${taskArchive} AND NOT "${${taskArchive}}" STREQUAL "")
        list(APPEND taskConfigure "-D${taskArchive}=${${taskArchive}}")
    endif()
endforeach()
list(APPEND taskConfigure "-DCUE_GAME_BUILD_APP=ON")
execute_process(COMMAND ${taskConfigure} RESULT_VARIABLE taskResult OUTPUT_VARIABLE taskOutput ERROR_VARIABLE taskError)
if(NOT taskResult EQUAL 0)
    message(FATAL_ERROR "Standalone App SDK configure failed: ${taskOutput}\n${taskError}")
endif()
message(STATUS "Standalone game CMake root, Script SDK build, API linkage and App target isolation passed")
