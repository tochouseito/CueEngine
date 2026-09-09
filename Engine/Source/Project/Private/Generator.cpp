#include <Cue/Project/Generator.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/Project/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maximumDescriptorBytes = 1024U * 1024U;
constexpr std::size_t k_maximumGeneratedFileBytes = 1024U * 1024U;

constexpr std::string_view k_projectCMake = R"cmake(# CueGame Project の正式なビルド入口を定義する
cmake_minimum_required(VERSION 4.2.0)
project(CueGame LANGUAGES CXX)

set(CUE_GAME_CONFIGURATION "Debug" CACHE STRING "CueGame build configuration")
set_property(CACHE CUE_GAME_CONFIGURATION PROPERTY STRINGS Debug Development Release)
set(cueGameConfigurations Debug Development Release)
list(FIND cueGameConfigurations "${CUE_GAME_CONFIGURATION}" cueGameConfigurationIndex)
if(cueGameConfigurationIndex EQUAL -1)
    message(FATAL_ERROR "CUE_GAME_CONFIGURATION must be Debug, Development, or Release")
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CUE_GAME_CONFIGURATION}" CACHE STRING "CueGame configurations" FORCE)
else()
    set(CMAKE_BUILD_TYPE "${CUE_GAME_CONFIGURATION}" CACHE STRING "CueGame configuration" FORCE)
endif()

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "CueGame initially supports Windows x64 with MSVC only")
endif()
if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
    message(FATAL_ERROR "CueGame initially supports the Visual Studio x64 platform only")
endif()
if(NOT DEFINED CUE_ENGINE_ROOT OR CUE_ENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR "CUE_ENGINE_ROOT must locate the CueEngine source checkout")
endif()
cmake_path(ABSOLUTE_PATH CUE_ENGINE_ROOT NORMALIZE OUTPUT_VARIABLE cueEngineRoot)
set(cueGameModuleCMake "${cueEngineRoot}/Engine/Source/GameModule/CMakeLists.txt")
if(NOT EXISTS "${cueGameModuleCMake}")
    message(FATAL_ERROR "CUE_ENGINE_ROOT does not contain Engine/Source/GameModule")
endif()

add_subdirectory("${cueEngineRoot}/Engine/Source/GameModule" "${CMAKE_BINARY_DIR}/CueEngine/GameModule")
add_subdirectory(Source/Game)
)cmake";

constexpr std::string_view k_gameCMake = R"cmake(add_library(CueGameModule SHARED)

target_sources(CueGameModule PRIVATE GameModule.cpp)
target_link_libraries(CueGameModule PRIVATE Cue.GameModule.Abi)
target_compile_features(CueGameModule PRIVATE cxx_std_20)
target_compile_definitions(
    CueGameModule
    PRIVATE
        CUE_GAME_MODULE_BUILD=1
        $<$<CONFIG:Debug>:CUE_GAME_MODULE_CONFIGURATION=1>
        $<$<CONFIG:Development>:CUE_GAME_MODULE_CONFIGURATION=2>
        $<$<CONFIG:Release>:CUE_GAME_MODULE_CONFIGURATION=3>
)

set_target_properties(
    CueGameModule
    PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        OUTPUT_NAME "CueGameModule"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

target_compile_options(
    CueGameModule
    PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
)
target_link_options(CueGameModule PRIVATE $<$<CONFIG:Development>:/DEBUG>)
)cmake";

constexpr std::string_view k_projectPresets = R"json({
    "version": 9,
    "cmakeMinimumRequired": {
        "major": 4,
        "minor": 2,
        "patch": 0
    },
    "configurePresets": [
        {
            "name": "windows-vs2026-base",
            "hidden": true,
            "generator": "Visual Studio 18 2026",
            "architecture": "x64",
            "cacheVariables": {
                "CUE_ENGINE_ROOT": "$env{CUE_ENGINE_ROOT}"
            }
        },
        {
            "name": "windows-vs2026-debug",
            "displayName": "Windows x64 Debug",
            "inherits": "windows-vs2026-base",
            "binaryDir": "${sourceDir}/Generated/Build/windows-vs2026-x64-debug",
            "cacheVariables": {
                "CUE_GAME_CONFIGURATION": "Debug"
            }
        },
        {
            "name": "windows-vs2026-development",
            "displayName": "Windows x64 Development",
            "inherits": "windows-vs2026-base",
            "binaryDir": "${sourceDir}/Generated/Build/windows-vs2026-x64-development",
            "cacheVariables": {
                "CUE_GAME_CONFIGURATION": "Development"
            }
        },
        {
            "name": "windows-vs2026-release",
            "displayName": "Windows x64 Release",
            "inherits": "windows-vs2026-base",
            "binaryDir": "${sourceDir}/Generated/Build/windows-vs2026-x64-release",
            "cacheVariables": {
                "CUE_GAME_CONFIGURATION": "Release"
            }
        }
    ],
    "buildPresets": [
        {
            "name": "windows-vs2026-debug",
            "configurePreset": "windows-vs2026-debug",
            "configuration": "Debug"
        },
        {
            "name": "windows-vs2026-development",
            "configurePreset": "windows-vs2026-development",
            "configuration": "Development"
        },
        {
            "name": "windows-vs2026-release",
            "configurePreset": "windows-vs2026-release",
            "configuration": "Release"
        }
    ]
}
)json";

constexpr std::string_view k_gameModuleSource = R"cpp(#include <Cue/GameModule/GameModuleAbi.h>

#include <new>

namespace
{
struct ModuleState final
{
};

#if CUE_GAME_MODULE_CONFIGURATION == CUE_GAME_MODULE_CONFIGURATION_DEBUG
constexpr uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEBUG;
#elif CUE_GAME_MODULE_CONFIGURATION == CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT
constexpr uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
#elif CUE_GAME_MODULE_CONFIGURATION == CUE_GAME_MODULE_CONFIGURATION_RELEASE
constexpr uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_RELEASE;
#else
#error CUE_GAME_MODULE_CONFIGURATION must identify a supported configuration
#endif

constexpr CueGameUuidV1 k_projectId = {
    static_cast<uint32_t>(sizeof(CueGameUuidV1)), CUE_GAME_MODULE_STRUCTURE_VERSION_1, {@PROJECT_UUID_BYTES@}};

/// @brief 呼出中だけ有効な診断文字列をHost出力へ設定する
void set_diagnostic(CueGameModuleDiagnosticV1 *a_diagnostic, CueGameModuleResult a_code, const char *a_message,
                    uint64_t a_size) noexcept
{
    if (a_diagnostic == nullptr || a_diagnostic->structSize < sizeof(CueGameModuleDiagnosticV1) ||
        a_diagnostic->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return;
    }
    a_diagnostic->code = a_code;
    a_diagnostic->reserved = 0U;
    a_diagnostic->message.structSize = static_cast<uint32_t>(sizeof(CueGameUtf8ViewV1));
    a_diagnostic->message.version = CUE_GAME_MODULE_STRUCTURE_VERSION_1;
    a_diagnostic->message.data = a_message;
    a_diagnostic->message.size = a_size;
}

/// @brief Project Scopeで所有する最小Module Stateを生成する
CueGameModuleResult CUE_GAME_MODULE_CALL create_module(CueGameModuleHandle *a_module,
                                                        CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_module == nullptr || *a_module != nullptr)
    {
        constexpr char message[] = "Module output must be a non-null pointer containing null";
        set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT, message, sizeof(message) - 1U);
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    ModuleState *state = new (std::nothrow) ModuleState{};
    if (state == nullptr)
    {
        constexpr char message[] = "Module state allocation failed";
        set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY, message, sizeof(message) - 1U);
        return CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY;
    }
    *a_module = state;
    set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_SUCCESS, nullptr, 0U);
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief 空登録でもHost所有SinkとModule Handleの契約を検証する
CueGameModuleResult validate_registration(CueGameModuleHandle a_module, const CueGameRegistrationSinkV1 *a_sink,
                                          CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_module == nullptr || a_sink == nullptr || a_sink->structSize < sizeof(CueGameRegistrationSinkV1) ||
        a_sink->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 || a_sink->registerSchema == nullptr ||
        a_sink->registerComponent == nullptr || a_sink->registerSystem == nullptr || a_sink->reserved[0] != 0U ||
        a_sink->reserved[1] != 0U || a_sink->reserved[2] != 0U || a_sink->reserved[3] != 0U)
    {
        constexpr char message[] = "Registration input is invalid";
        set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT, message, sizeof(message) - 1U);
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_SUCCESS, nullptr, 0U);
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Blank Projectが追加Schemaを持たないことを成功として登録する
CueGameModuleResult CUE_GAME_MODULE_CALL register_schemas(CueGameModuleHandle a_module,
                                                          const CueGameRegistrationSinkV1 *a_sink,
                                                          CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    return validate_registration(a_module, a_sink, a_diagnostic);
}

/// @brief Blank Projectが追加Component宣言を持たないことを成功として登録する
CueGameModuleResult CUE_GAME_MODULE_CALL register_components(CueGameModuleHandle a_module,
                                                             const CueGameRegistrationSinkV1 *a_sink,
                                                             CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    return validate_registration(a_module, a_sink, a_diagnostic);
}

/// @brief Blank ProjectがRuntime Systemを持たないことを成功として登録する
CueGameModuleResult CUE_GAME_MODULE_CALL register_systems(CueGameModuleHandle a_module,
                                                          const CueGameRegistrationSinkV1 *a_sink,
                                                          CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    return validate_registration(a_module, a_sink, a_diagnostic);
}

/// @brief 生成元DLLのAllocatorでModule Stateを一度だけ破棄する
void CUE_GAME_MODULE_CALL destroy_module(CueGameModuleHandle a_module) noexcept
{
    delete static_cast<ModuleState *>(a_module);
}

const CueGameModuleApiV1 k_api = {
    static_cast<uint32_t>(sizeof(CueGameModuleApiV1)), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    CUE_GAME_MODULE_ABI_VERSION_1, k_configuration, CUE_GAME_MODULE_ARCHITECTURE_X64, 0U, k_projectId,
    &create_module, &register_schemas, &register_components, &register_systems, &destroy_module, {0U, 0U, 0U, 0U}};
} // namespace

/// @brief Host要求Versionと互換なDLL所有API Tableを借用出力へ返す
CueGameModuleResult CUE_GAME_MODULE_CALL cue_game_module_query(uint32_t a_requestedAbiVersion,
                                                               CueGameModuleQueryOutputV1 *a_output,
                                                               CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_output == nullptr || a_output->structSize < sizeof(CueGameModuleQueryOutputV1) ||
        a_output->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        constexpr char message[] = "Query output is invalid";
        set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT, message, sizeof(message) - 1U);
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    a_output->api = nullptr;
    a_output->reserved[0] = 0U;
    a_output->reserved[1] = 0U;
    if (a_requestedAbiVersion != CUE_GAME_MODULE_ABI_VERSION_1)
    {
        constexpr char message[] = "Requested Game Module ABI is unsupported";
        set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_UNSUPPORTED_ABI, message, sizeof(message) - 1U);
        return CUE_GAME_MODULE_RESULT_UNSUPPORTED_ABI;
    }
    a_output->api = &k_api;
    set_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_SUCCESS, nullptr, 0U);
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}
)cpp";

struct GeneratedProjectFile final
{
    std::string_view path;
    std::string contents;
};

using GameWorkspaceFiles = std::array<GeneratedProjectFile, 4U>;

/// @brief Generator 処理中の予期しない例外を追加 Allocation なしで Fatal 境界へ渡す
[[noreturn]] void terminate_generator_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Project generator operation failed unexpectedly");
    std::abort();
}

/// @brief 下位 IO 失敗を原因として保持しながら Project 生成失敗へ再分類する
[[nodiscard]] cue::Error reclassify_io_error(const cue::AssertContext &a_assertContext, std::string_view a_summary,
                                             cue::Error &&a_cause) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Project",
                                                 static_cast<std::int64_t>(cue::ProjectError::IoFailure));
    return cue::Error::reclassify(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(a_cause));
}

/// @brief 無効な Project 名の下位 Path 診断を保持して Project 分類へ再分類する
[[nodiscard]] cue::Error reclassify_project_name_error(const cue::AssertContext &a_assertContext,
                                                       cue::Error &&a_cause) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Project",
                                                 static_cast<std::int64_t>(cue::ProjectError::InvalidProjectName));
    return cue::Error::reclassify(a_assertContext.fatal_handler(), std::move(code), "Project name is invalid",
                                  std::move(a_cause));
}

/// @brief Staging 基点と Template 相対 Path を結合して再検証済み Path を返す
[[nodiscard]] cue::Result<cue::RelativePath> make_staging_path(const cue::StagingArea &a_staging,
                                                               std::string_view a_suffix,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::string path(a_staging.path().text());
        path.push_back('/');
        path.append(a_suffix);
        return cue::RelativePath::parse(path, a_assertContext);
    }
    catch (...)
    {
        terminate_generator_exception(a_assertContext);
    }
}

/// @brief Primary 失敗を維持したまま Operation 所有 Staging の Rollback 失敗を Secondary 診断へ追加する
void rollback_staging(cue::FilesystemRoot &a_filesystem, cue::StagingArea &a_staging, cue::Error &a_primary,
                      const cue::AssertContext &a_assertContext) noexcept
{
    auto rollback = a_filesystem.rollback_staging_area(std::move(a_staging));
    if (!rollback)
    {
        a_primary.append_secondary_diagnostics(a_assertContext, *rollback.try_error(),
                                               "Project staging rollback failed", "Rollback");
    }
}

/// @brief IO 失敗が Publish 済みで Rollback 不能な DurabilityUnknown か判定する
[[nodiscard]] bool is_durability_unknown(const cue::Error &a_error) noexcept
{
    return a_error.code().domain() == "Cue.IO" &&
           a_error.code().value() == static_cast<std::int64_t>(cue::IoError::DurabilityUnknown);
}

/// @brief Byte 列を Copy せず Descriptor Parser へ渡せる UTF-8 View へ変換する
[[nodiscard]] std::string_view bytes_as_string(std::span<const std::byte> a_bytes) noexcept
{
    return std::string_view(reinterpret_cast<const char *>(a_bytes.data()), a_bytes.size());
}

/// @brief Canonical UUIDの16進文字を生成Source用Byte値へ変換する
[[nodiscard]] std::uint8_t decode_hex_digit(char a_value) noexcept
{
    if (a_value >= '0' && a_value <= '9')
    {
        return static_cast<std::uint8_t>(a_value - '0');
    }
    return static_cast<std::uint8_t>(a_value - 'a' + 10);
}

/// @brief 検証済みProject IDをC ABIの16 byte UUID初期化子へ変換する
[[nodiscard]] std::string make_project_uuid_bytes(std::string_view a_projectId)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string compact;
    compact.reserve(32U);
    for (const char value : a_projectId)
    {
        if (value != '-')
        {
            compact.push_back(value);
        }
    }

    std::string result;
    result.reserve(16U * 7U);
    for (std::size_t index = 0U; index < compact.size(); index += 2U)
    {
        const std::uint8_t value =
            static_cast<std::uint8_t>((decode_hex_digit(compact[index]) << 4U) | decode_hex_digit(compact[index + 1U]));
        if (!result.empty())
        {
            result.append(", ");
        }
        result.append("0x");
        result.push_back(digits[(value >> 4U) & 0x0FU]);
        result.push_back(digits[value & 0x0FU]);
        result.push_back('U');
    }
    return result;
}

/// @brief Blank Project固有IDだけを固定Templateへ埋め込んだ最小Game Module Sourceを生成する
[[nodiscard]] std::string make_game_module_source(const cue::ProjectId &a_projectId)
{
    constexpr std::string_view marker = "@PROJECT_UUID_BYTES@";
    std::string source(k_gameModuleSource);
    source.replace(source.find(marker), marker.size(), make_project_uuid_bytes(a_projectId.text()));
    return source;
}

/// @brief Project固有値を反映したGame SourceとCMake Workspace Templateを所有値として構築する
[[nodiscard]] GameWorkspaceFiles make_game_workspace_files(const cue::ProjectId &a_projectId)
{
    return {GeneratedProjectFile{"CMakeLists.txt", std::string(k_projectCMake)},
            GeneratedProjectFile{"CMakePresets.json", std::string(k_projectPresets)},
            GeneratedProjectFile{"Source/Game/CMakeLists.txt", std::string(k_gameCMake)},
            GeneratedProjectFile{"Source/Game/GameModule.cpp", make_game_module_source(a_projectId)}};
}

/// @brief Blank ProjectのDefault SceneをScene Serializerと同じcanonical最小JSONとして生成する
[[nodiscard]] std::string make_default_scene(std::string_view a_sceneAssetId)
{
    std::string scene;
    scene.reserve(112U);
    scene.append("{\"formatVersion\":1,\"sceneAssetId\":\"");
    scene.append(a_sceneAssetId);
    scene.append("\",\"objects\":[],\"extensions\":{}}");
    return scene;
}
} // namespace

namespace cue
{
Result<ProjectDescriptor> generate_blank_project(FilesystemRoot &a_parentFilesystem, std::string_view a_projectName,
                                                 std::string_view a_displayName, const ProjectId &a_projectId,
                                                 std::string_view a_defaultSceneAssetId,
                                                 BlankProjectTemplate a_template,
                                                 const AssertContext &a_assertContext) noexcept
{
    try
    {
        auto destination = RelativePath::parse(a_projectName, a_assertContext);
        if (!destination)
        {
            return Result<ProjectDescriptor>::failure(
                reclassify_project_name_error(a_assertContext, std::move(*destination.try_error())));
        }
        if (a_projectName.find('/') != std::string_view::npos)
        {
            return Result<ProjectDescriptor>::failure(make_project_error(
                a_assertContext, ProjectError::InvalidProjectName, "Project name must be one directory segment"));
        }

        auto descriptor = create_blank_project_descriptor(a_projectId, a_displayName, a_template.engineCompatibility,
                                                          a_defaultSceneAssetId, a_assertContext);
        if (!descriptor)
        {
            return Result<ProjectDescriptor>::failure(std::move(*descriptor.try_error()));
        }
        auto serialized = serialize_project_descriptor(*descriptor.try_value(), a_assertContext);
        if (!serialized)
        {
            return Result<ProjectDescriptor>::failure(std::move(*serialized.try_error()));
        }

        GameWorkspaceFiles gameWorkspaceFiles = make_game_workspace_files(a_projectId);
        const std::array files = {
            GeneratedProjectFile{"CueProject.json", *serialized.try_value()},
            GeneratedProjectFile{"Assets/Source/Scenes/Default.cuescene", make_default_scene(a_defaultSceneAssetId)},
            std::move(gameWorkspaceFiles[0U]),
            std::move(gameWorkspaceFiles[1U]),
            std::move(gameWorkspaceFiles[2U]),
            std::move(gameWorkspaceFiles[3U])};

        auto staging = a_parentFilesystem.create_staging_area(*destination.try_value());
        if (!staging)
        {
            return Result<ProjectDescriptor>::failure(reclassify_io_error(
                a_assertContext, "Project staging directory creation failed", std::move(*staging.try_error())));
        }

        constexpr std::array<std::string_view, 6U> directories = {
            "Assets/Source", "Assets/Source/Scenes", "Assets/Runtime", "Generated", "Saved", "Source/Game"};
        for (const std::string_view directory : directories)
        {
            auto path = make_staging_path(*staging.try_value(), directory, a_assertContext);
            if (!path)
            {
                Error primary = reclassify_io_error(a_assertContext, "Project directory path creation failed",
                                                    std::move(*path.try_error()));
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
                return Result<ProjectDescriptor>::failure(std::move(primary));
            }
            auto created = a_parentFilesystem.create_directories(*path.try_value());
            if (!created)
            {
                Error primary = reclassify_io_error(a_assertContext, "Project directory creation failed",
                                                    std::move(*created.try_error()));
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
                return Result<ProjectDescriptor>::failure(std::move(primary));
            }
        }

        for (const GeneratedProjectFile &file : files)
        {
            auto path = make_staging_path(*staging.try_value(), file.path, a_assertContext);
            if (!path)
            {
                Error primary = reclassify_io_error(a_assertContext, "Project file path creation failed",
                                                    std::move(*path.try_error()));
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
                return Result<ProjectDescriptor>::failure(std::move(primary));
            }

            const std::span<const char> characters(file.contents.data(), file.contents.size());
            auto written = a_parentFilesystem.write_file_atomic(*path.try_value(), std::as_bytes(characters));
            if (!written)
            {
                Error primary =
                    reclassify_io_error(a_assertContext, "Project file write failed", std::move(*written.try_error()));
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
                return Result<ProjectDescriptor>::failure(std::move(primary));
            }

            const std::size_t maximumBytes =
                file.path == "CueProject.json" ? k_maximumDescriptorBytes : k_maximumGeneratedFileBytes;
            auto stagedBytes = a_parentFilesystem.read_file(*path.try_value(), maximumBytes);
            if (!stagedBytes)
            {
                Error primary = reclassify_io_error(a_assertContext, "Project file verification read failed",
                                                    std::move(*stagedBytes.try_error()));
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
                return Result<ProjectDescriptor>::failure(std::move(primary));
            }
            if (bytes_as_string(*stagedBytes.try_value()) != file.contents)
            {
                Error primary = make_project_error(a_assertContext, ProjectError::InvalidFormat,
                                                   "Staged project file changed during verification");
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
                return Result<ProjectDescriptor>::failure(std::move(primary));
            }
        }

        auto reparsed = parse_project_descriptor(files[0U].contents, a_assertContext);
        if (!reparsed || !descriptor.try_value()->equivalent_to(*reparsed.try_value()))
        {
            Error primary = reparsed ? make_project_error(a_assertContext, ProjectError::InvalidFormat,
                                                          "Staged project descriptor changed during verification")
                                     : std::move(*reparsed.try_error());
            rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
            return Result<ProjectDescriptor>::failure(std::move(primary));
        }

        auto published =
            a_parentFilesystem.publish_staging_area(std::move(*staging.try_value()), *destination.try_value());
        if (!published)
        {
            const bool wasPublished = is_durability_unknown(*published.try_error());
            Error primary = reclassify_io_error(a_assertContext, "Project directory publish failed",
                                                std::move(*published.try_error()));
            if (!wasPublished)
            {
                rollback_staging(a_parentFilesystem, *staging.try_value(), primary, a_assertContext);
            }
            return Result<ProjectDescriptor>::failure(std::move(primary));
        }

        return Result<ProjectDescriptor>::success(std::move(*descriptor.try_value()));
    }
    catch (...)
    {
        terminate_generator_exception(a_assertContext);
    }
}

Result<void> ensure_project_game_workspace(FilesystemRoot &a_projectFilesystem,
                                           const ProjectDescriptor &a_expectedDescriptor,
                                           const AssertContext &a_assertContext) noexcept
{
    try
    {
        auto validated = validate_project_descriptor(a_expectedDescriptor, a_assertContext);
        if (!validated)
        {
            return Result<void>::failure(std::move(*validated.try_error()));
        }

        auto loaded = load_project_descriptor(a_projectFilesystem, a_assertContext);
        if (!loaded)
        {
            return Result<void>::failure(std::move(*loaded.try_error()));
        }
        if (!a_expectedDescriptor.equivalent_to(*loaded.try_value()))
        {
            return Result<void>::failure(make_project_error(a_assertContext, ProjectError::InvalidFormat,
                                                            "Project descriptor does not match expected project"));
        }

        const GameWorkspaceFiles files = make_game_workspace_files(a_expectedDescriptor.project_id());
        std::vector<RelativePath> paths;
        paths.reserve(files.size());
        std::array<std::size_t, 4U> missingIndexes{};
        std::size_t missingCount = 0U;

        for (std::size_t index = 0U; index < files.size(); ++index)
        {
            auto path = RelativePath::parse(files[index].path, a_assertContext);
            if (!path)
            {
                return Result<void>::failure(reclassify_io_error(
                    a_assertContext, "Project workspace path creation failed", std::move(*path.try_error())));
            }
            paths.push_back(std::move(*path.try_value()));

            auto type = a_projectFilesystem.query_entry(paths.back());
            if (!type)
            {
                return Result<void>::failure(reclassify_io_error(a_assertContext, "Project workspace preflight failed",
                                                                 std::move(*type.try_error())));
            }
            if (*type.try_value() == EntryType::Missing)
            {
                missingIndexes[missingCount++] = index;
                continue;
            }
            if (*type.try_value() != EntryType::RegularFile)
            {
                Error cause = make_io_error(a_assertContext, IoError::TypeMismatch,
                                            "Project workspace path is not a regular file");
                return Result<void>::failure(reclassify_io_error(
                    a_assertContext, "Project workspace create-only validation failed", std::move(cause)));
            }

            auto bytes = a_projectFilesystem.read_file(paths.back(), k_maximumGeneratedFileBytes);
            if (!bytes)
            {
                return Result<void>::failure(reclassify_io_error(
                    a_assertContext, "Project workspace preflight read failed", std::move(*bytes.try_error())));
            }
            if (bytes_as_string(*bytes.try_value()) != files[index].contents)
            {
                Error cause = make_io_error(a_assertContext, IoError::AlreadyExists,
                                            "Existing project workspace file differs from the generator template");
                return Result<void>::failure(reclassify_io_error(
                    a_assertContext, "Project workspace create-only validation failed", std::move(cause)));
            }
        }

        if (missingCount == 0U)
        {
            return Result<void>::success();
        }

        auto sourceGame = RelativePath::parse("Source/Game", a_assertContext);
        if (!sourceGame)
        {
            return Result<void>::failure(reclassify_io_error(a_assertContext,
                                                             "Project workspace directory path creation failed",
                                                             std::move(*sourceGame.try_error())));
        }
        auto createdDirectory = a_projectFilesystem.create_directories(*sourceGame.try_value());
        if (!createdDirectory)
        {
            return Result<void>::failure(reclassify_io_error(a_assertContext,
                                                             "Project workspace directory creation failed",
                                                             std::move(*createdDirectory.try_error())));
        }

        std::vector<FileWriteLease> leases;
        leases.reserve(missingCount);
        std::vector<std::size_t> createIndexes;
        createIndexes.reserve(missingCount);
        for (std::size_t offset = 0U; offset < missingCount; ++offset)
        {
            const std::size_t index = missingIndexes[offset];
            auto lease = a_projectFilesystem.acquire_file_write_lease(paths[index]);
            if (!lease)
            {
                return Result<void>::failure(reclassify_io_error(a_assertContext,
                                                                 "Project workspace write lease acquisition failed",
                                                                 std::move(*lease.try_error())));
            }
            auto fingerprint =
                fingerprint_file(a_projectFilesystem, paths[index], k_maximumGeneratedFileBytes, a_assertContext);
            if (!fingerprint)
            {
                return Result<void>::failure(reclassify_io_error(
                    a_assertContext, "Project workspace leased preflight failed", std::move(*fingerprint.try_error())));
            }
            if (fingerprint.try_value()->exists)
            {
                auto bytes = a_projectFilesystem.read_file(paths[index], k_maximumGeneratedFileBytes);
                if (!bytes)
                {
                    return Result<void>::failure(reclassify_io_error(
                        a_assertContext, "Project workspace leased read failed", std::move(*bytes.try_error())));
                }
                if (bytes_as_string(*bytes.try_value()) == files[index].contents)
                {
                    continue;
                }
                Error cause = make_io_error(a_assertContext, IoError::AlreadyExists,
                                            "Project workspace file changed before create-only write");
                return Result<void>::failure(reclassify_io_error(
                    a_assertContext, "Project workspace create-only validation failed", std::move(cause)));
            }
            leases.push_back(std::move(*lease.try_value()));
            createIndexes.push_back(index);
        }

        for (std::size_t offset = 0U; offset < createIndexes.size(); ++offset)
        {
            const std::size_t index = createIndexes[offset];
            const std::span<const char> characters(files[index].contents.data(), files[index].contents.size());
            auto written = a_projectFilesystem.write_file_atomic_if_unchanged(
                leases[offset], paths[index], FileFingerprint{}, k_maximumGeneratedFileBytes,
                std::as_bytes(characters));
            if (!written)
            {
                return Result<void>::failure(reclassify_io_error(a_assertContext, "Project workspace file write failed",
                                                                 std::move(*written.try_error())));
            }

            auto bytes = a_projectFilesystem.read_file(paths[index], k_maximumGeneratedFileBytes);
            if (!bytes || bytes_as_string(*bytes.try_value()) != files[index].contents)
            {
                Error primary = bytes
                                    ? make_project_error(a_assertContext, ProjectError::InvalidFormat,
                                                         "Generated project workspace file changed during verification")
                                    : reclassify_io_error(a_assertContext, "Project workspace verification read failed",
                                                          std::move(*bytes.try_error()));
                return Result<void>::failure(std::move(primary));
            }
        }

        return Result<void>::success();
    }
    catch (...)
    {
        terminate_generator_exception(a_assertContext);
    }
}
} // namespace cue
