#include "EditorPlayServices.h"

#include <utility>

#include <Math/Projection.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuild.h>

namespace cue
{
namespace
{
/// @brief 外部 Process の起動先を絶対 Path の通常 File に限定する
Result<void> validate_play_file(IFileSystem &a_files, const Path &a_file, const char *a_operation)
{
    if (!a_file.is_absolute())
        return Result<void>::failure({ErrorCategory::InvalidArgument, a_operation});
    auto info = a_files.stat(a_file);
    if (!info.has_value())
        return Result<void>::failure(*info.try_error());
    if (!*info.try_value() || (*info.try_value())->type != FileType::Regular)
        return Result<void>::failure({ErrorCategory::InvalidArgument, a_operation});
    return Result<void>::success();
}
} // namespace

Result<void> validate_editor_play_view(const SceneRenderSnapshot &a_snapshot, float a_aspect)
{
    if (a_snapshot.objects.empty())
        return Result<void>::success();
    if (!a_snapshot.camera)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.play.camera_missing"});
    const auto &camera = *a_snapshot.camera;
    auto projection = math::perspective_rh_zo(camera.verticalFovRadians, a_aspect, camera.nearClip, camera.farClip);
    if (!projection.has_value())
        return Result<void>::failure(*projection.try_error());
    for (const auto &object : a_snapshot.objects)
        if (!math::is_finite(object.world * camera.view * *projection.try_value()))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorHost.play.matrix_overflow"});
    return Result<void>::success();
}

Result<PlayClientConfig> resolve_editor_play_config(IFileSystem &a_files, const Project *a_project,
                                                    const std::optional<Path> &a_executable,
                                                    const std::optional<Path> &a_module,
                                                    GameBuildConfiguration a_configuration)
{
    if (a_executable.has_value() != a_module.has_value())
        return Result<PlayClientConfig>::failure(
            {ErrorCategory::InvalidArgument, "EditorHost.play.partial_configuration"});
    PlayClientConfig config;
    switch (a_configuration)
    {
    case GameBuildConfiguration::Debug:
        config.moduleConfiguration = ScriptConfiguration::Debug;
        break;
    case GameBuildConfiguration::Development:
        config.moduleConfiguration = ScriptConfiguration::Development;
        break;
    case GameBuildConfiguration::Release:
        config.moduleConfiguration = ScriptConfiguration::Release;
        break;
    default:
        return Result<PlayClientConfig>::failure(
            {ErrorCategory::InvalidArgument, "EditorHost.play.game_configuration"});
    }
    // Editor の SceneView が描画を担当し、Game DLL は表示 Window のない子 Process に隔離する
    config.isHeadless = true;
    config.isEmbedded = true;
    if (a_executable)
    {
        // Library の明示設定を優先し、不正でも Project の DLL に黙って戻らない
        config.executable = *a_executable;
        config.module = *a_module;
        auto valid = validate_play_file(a_files, config.module, "EditorHost.play.module_file");
        if (!valid.has_value())
            return Result<PlayClientConfig>::failure(*valid.try_error());
    }
    else
    {
        if (!a_project || !a_project->descriptor().gameTarget)
            return Result<PlayClientConfig>::failure({ErrorCategory::InvalidState, "EditorHost.play.not_configured"});
        // Build 配置・ABI・構成・Project Identity・Hash の検証は ProjectBuild の契約へ集約する
        auto artifacts = load_game_build_artifacts(a_files, *a_project, a_configuration);
        if (!artifacts.has_value())
            return Result<PlayClientConfig>::failure(*artifacts.try_error());
        config.module = artifacts.try_value()->moduleFile;
        auto directory = a_files.executable_directory();
        if (!directory.has_value())
            return Result<PlayClientConfig>::failure(*directory.try_error());
        auto executable = directory.try_value()->join("CuePlayHost.exe");
        if (!executable.has_value())
            return Result<PlayClientConfig>::failure(*executable.try_error());
        config.executable = executable.take_value();
    }
    auto valid = validate_play_file(a_files, config.executable, "EditorHost.play.executable_file");
    if (!valid.has_value())
        return Result<PlayClientConfig>::failure(*valid.try_error());
    return Result<PlayClientConfig>::success(std::move(config));
}
} // namespace cue
