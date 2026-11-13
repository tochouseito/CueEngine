#include <ProjectTools/ProjectCreator.h>

#include <algorithm>
#include <array>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <EngineInstallation/EngineInstallation.h>
#include <Foundation/Utf8.h>
#include <Foundation/Uuid.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>
#include <Scene/Scene.h>

namespace cue
{
namespace
{
/// @brief Windows の末尾 Dot / Space や ADS の別名で保存先が変わることを防ぐ
bool is_normal_directory_path(const Path &a_path)
{
    auto current = a_path;
    while (current.parent().utf8() != current.utf8())
    {
        const auto name = current.filename();
        if (name.empty() || name.back() == '.' || name.back() == ' ' ||
            name.find_first_of("<>:\"|?*") != std::string::npos)
            return false;
        current = current.parent();
    }
    return true;
}

/// @brief 既存の全親を通常 Directory と確認し、Link / Junction を越える作成・読込を拒否する
Result<void> validate_directory(IFileSystem &a_files, const Path &a_path)
{
    if (!a_path.is_absolute())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "ProjectCreate.absolute_directory"});
    auto current = a_path;
    for (;;)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (!*info.try_value() || (**info.try_value()).type != FileType::Directory)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "ProjectCreate.directory:" + current.utf8()});
        const auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            break;
        current = parent;
    }
    return Result<void>::success();
}

/// @brief 設定の一行として保存できる表示名と、Windows でも単一 File 名になる識別名を検証する
bool valid_names(const ProjectCreateDesc &a_desc)
{
    if (a_desc.name.empty() || a_desc.name.size() > 256 || !is_valid_utf8_text(a_desc.name) ||
        a_desc.name.front() == ' ' || a_desc.name.back() == ' ' || a_desc.applicationName.empty() ||
        a_desc.applicationName.size() > 120)
        return false;
    for (const auto character : a_desc.name)
        if (static_cast<unsigned char>(character) < 32 || character == 127)
            return false;
    std::string lower;
    for (const auto character : a_desc.applicationName)
    {
        if (!(character >= 'A' && character <= 'Z') && !(character >= 'a' && character <= 'z') &&
            !(character >= '0' && character <= '9') && character != '_' && character != '-')
            return false;
        lower += character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul")
        return false;
    return !(lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) && lower[3] >= '1' &&
             lower[3] <= '9');
}

/// @brief 管理する固定 Token だけを置換し、表示名を CMake Source へ埋め込まない
void substitute(std::string &a_text, std::string_view a_token, std::string_view a_value)
{
    for (std::size_t offset = 0; (offset = a_text.find(a_token, offset)) != std::string::npos;)
    {
        a_text.replace(offset, a_token.size(), a_value);
        offset += a_value.size();
    }
}

/// @brief 生成した既知の単一要素だけを逆順に回収し、外部の File を再帰削除しない
class StagingOwner final
{
  public:
    /// @brief FileSystem は create_project の呼出中だけ借用する
    explicit StagingOwner(IFileSystem &a_files) : m_files(a_files)
    {
    }
    /// @brief 例外時も既知要素の回収だけを試み、失敗した Staging は保持する
    ~StagingOwner()
    {
        if (!isPublished)
        {
            try
            {
                (void)cleanup();
            }
            catch (...)
            {
            }
        }
    }
    /// @brief 現在存在しない Directory だけを生成候補とし、既存内容へ書き込まない
    Result<void> directory(const Path &a_path)
    {
        auto info = m_files.stat(a_path);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (*info.try_value())
            return Result<void>::failure({ErrorCategory::InvalidArgument, "ProjectCreate.staging_exists"});
        auto made = m_files.create_directories(a_path);
        if (!made.has_value())
            return made;
        m_directories.push_back(a_path);
        return Result<void>::success();
    }
    /// @brief CreateNew の成功だけを所有根拠とし、完全書込み・Flush・Close を確認する
    Result<void> file(const Path &a_path, std::string_view a_text, bool a_keepOpen = false)
    {
        auto parent = validate_directory(m_files, a_path.parent());
        if (!parent.has_value())
            return parent;
        FileOpenDesc descriptor;
        descriptor.access = FileAccess::Write;
        descriptor.creation = FileCreation::CreateNew;
        descriptor.isReadShared = false;
        auto opened = m_files.open(a_path, descriptor);
        if (!opened.has_value())
            return Result<void>::failure(*opened.try_error());
        auto handle = opened.take_value();
        m_paths.push_back(a_path);
        auto bytes = std::as_bytes(std::span(a_text.data(), a_text.size()));
        while (!bytes.empty())
        {
            auto written = handle->write(bytes);
            if (!written.has_value())
                return Result<void>::failure(*written.try_error());
            if (!*written.try_value() || *written.try_value() > bytes.size())
                return Result<void>::failure({ErrorCategory::PlatformFailure, "ProjectCreate.short_write"});
            bytes = bytes.subspan(*written.try_value());
        }
        auto flushed = handle->flush();
        if (!flushed.has_value())
            return flushed;
        if (a_keepOpen)
            m_marker = std::move(handle);
        else
            return handle->close();
        return Result<void>::success();
    }
    /// @brief 全 File の生成後に所有 Marker を閉じて取り除き、Directory Rename を可能にする
    Result<void> release_marker()
    {
        if (!m_marker)
            return Result<void>::failure({ErrorCategory::InvalidState, "ProjectCreate.marker"});
        auto closed = m_marker->close();
        m_marker.reset();
        if (!closed.has_value())
            return closed;
        auto removed = m_files.remove(m_paths.front());
        if (!removed.has_value() || !*removed.try_value())
            return removed.has_value()
                       ? Result<void>::failure({ErrorCategory::PlatformFailure, "ProjectCreate.marker_missing"})
                       : Result<void>::failure(*removed.try_error());
        m_paths.erase(m_paths.begin());
        return Result<void>::success();
    }
    /// @brief Cleanup の失敗を主原因へ付記し、残った Staging を運用側で確認できるようにする
    Result<ProjectCreation> fail(Error a_error)
    {
        auto cleaned = cleanup();
        if (!cleaned.has_value())
            a_error.operation += ";cleanup:" + cleaned.try_error()->operation;
        return Result<ProjectCreation>::failure(std::move(a_error));
    }
    bool isPublished = false;

  private:
    /// @brief 不明要素が残った Directory は remove が拒否し、その内容を保持する
    Result<void> cleanup()
    {
        if (m_marker)
        {
            auto closed = m_marker->close();
            m_marker.reset();
            if (!closed.has_value())
                return closed;
        }
        std::optional<Error> failure;
        for (auto iterator = m_paths.rbegin(); iterator != m_paths.rend(); ++iterator)
        {
            auto info = m_files.stat(*iterator);
            if (!info.has_value() || (*info.try_value() && (**info.try_value()).type != FileType::Regular))
            {
                if (!failure)
                    failure = info.has_value() ? Error{ErrorCategory::PlatformFailure, "ProjectCreate.changed_file"}
                                               : *info.try_error();
                continue;
            }
            auto removed = m_files.remove(*iterator);
            if (!removed.has_value() && !failure)
                failure = *removed.try_error();
        }
        m_paths.clear();
        for (auto iterator = m_directories.rbegin(); iterator != m_directories.rend(); ++iterator)
        {
            auto removed = m_files.remove(*iterator);
            if (!removed.has_value() && !failure)
                failure = *removed.try_error();
        }
        m_directories.clear();
        return failure ? Result<void>::failure(*failure) : Result<void>::success();
    }
    IFileSystem &m_files;
    std::vector<Path> m_paths;
    std::vector<Path> m_directories;
    std::unique_ptr<IFile> m_marker;
};

struct PlannedFile final
{
    std::string path;
    std::string text;
};
} // namespace

Result<ProjectCreation> create_project(IFileSystem &a_files, const Path &a_templates, const ProjectCreateDesc &a_desc,
                                       const projectIdGenerator &a_generateId)
{
    try
    {
        // 名前・保存先・Version を先に検証し、UUID Callback も File 作成より前に完了する
        if (!a_desc.directory.is_absolute() || !is_normal_directory_path(a_desc.directory) ||
            a_desc.directory.parent().utf8() == a_desc.directory.utf8() || !valid_names(a_desc) ||
            !is_valid_engine_version(a_desc.engineVersion) || !a_generateId ||
            (a_desc.projectTemplate != ProjectTemplate::Empty3D &&
             a_desc.projectTemplate != ProjectTemplate::RotateCube))
            return Result<ProjectCreation>::failure({ErrorCategory::InvalidArgument, "ProjectCreate.request"});
        auto parentValid = validate_directory(a_files, a_desc.directory.parent());
        auto templatesValid = validate_directory(a_files, a_templates);
        if (!parentValid.has_value() || !templatesValid.has_value())
            return Result<ProjectCreation>::failure(!parentValid.has_value() ? *parentValid.try_error()
                                                                             : *templatesValid.try_error());
        auto destination = a_files.stat(a_desc.directory);
        if (!destination.has_value() || *destination.try_value())
            return Result<ProjectCreation>::failure(destination.has_value()
                                                        ? Error{ErrorCategory::InvalidArgument, "ProjectCreate.exists"}
                                                        : *destination.try_error());
        std::array<std::string, 6> ids;
        for (auto &id : ids)
        {
            auto generated = a_generateId();
            if (!generated.has_value())
                return Result<ProjectCreation>::failure(*generated.try_error());
            id = generated.take_value();
            if (!is_valid_uuid_text(id) || std::count(ids.begin(), ids.end(), id) != 1)
                return Result<ProjectCreation>::failure({ErrorCategory::InvalidArgument, "ProjectCreate.uuid"});
        }
        const auto templateName = a_desc.projectTemplate == ProjectTemplate::RotateCube ? "RotateCube" : "Empty3D";
        auto templateDirectory = a_templates.join(templateName);
        if (!templateDirectory.has_value())
            return Result<ProjectCreation>::failure(*templateDirectory.try_error());
        std::vector<PlannedFile> planned;
        const auto scriptFile = a_desc.projectTemplate == ProjectTemplate::RotateCube ? "Source/RotateCube.cpp"
                                                                                      : "Source/EmptyBehaviour.cpp";
        for (const auto *relative : {"Source/CMakeLists.txt.in", scriptFile, "Source/ScriptSources.cueide",
                                     "Content/Scenes/Main.cuescene.in", "CMakeLists.txt", "CMakePresets.json",
                                     ".gitignore"})
        {
            const std::string_view relativeName(relative);
            const bool isWorkspace =
                relativeName == "CMakeLists.txt" || relativeName == "CMakePresets.json" || relativeName == ".gitignore";
            auto source = isWorkspace ? a_templates.join("GameWorkspace/" + std::string(relativeName))
                                      : templateDirectory.try_value()->join(relative);
            if (!source.has_value())
                return Result<ProjectCreation>::failure(*source.try_error());
            auto sourceParent = validate_directory(a_files, source.try_value()->parent());
            if (!sourceParent.has_value())
                return Result<ProjectCreation>::failure(*sourceParent.try_error());
            auto info = a_files.stat(*source.try_value());
            if (!info.has_value() || !*info.try_value() || (**info.try_value()).type != FileType::Regular)
                return Result<ProjectCreation>::failure(
                    {ErrorCategory::InvalidArgument, "ProjectCreate.template_file"});
            auto bytes = a_files.read_all(*source.try_value(), Scene::k_maxFileBytes);
            if (!bytes.has_value())
                return Result<ProjectCreation>::failure(*bytes.try_error());
            auto &data = *bytes.try_value();
            std::string text(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
            if (!is_valid_utf8_text(text))
                return Result<ProjectCreation>::failure(
                    {ErrorCategory::InvalidArgument, "ProjectCreate.template_utf8"});
            substitute(text, "@GAME_TARGET@", "CueGame_" + a_desc.applicationName);
            substitute(text, "@SCENE_ID@", ids[1]);
            substitute(text, "@CAMERA_ID@", ids[2]);
            substitute(text, "@CUBE_ID@", ids[3]);
            substitute(text, "@SCRIPT_ID@", ids[4]);
            std::string output(relative);
            if (output.ends_with(".in"))
                output.resize(output.size() - 3);
            planned.push_back({std::move(output), std::move(text)});
        }
        planned.push_back(
            {a_desc.applicationName + ".cueproj",
             "formatVersion=2\nprojectId=" + ids[0] + "\nname=" + a_desc.name +
                 "\napplicationName=" + a_desc.applicationName +
                 "\nsourceRoot=Source\ncontentRoot=Content\nstartupScene=Scenes/Main.cuescene\ngameTarget=CueGame_" +
                 a_desc.applicationName + "\nengineVersion=" + a_desc.engineVersion + "\n"});
        // 同じ親の Staging を所有 Marker で予約し、固定の通常 File だけを CreateNew で作る
        auto staging = a_desc.directory.parent().join(".cue-create-" + ids[5]);
        auto projectFile = a_desc.directory.join(a_desc.applicationName + ".cueproj");
        if (!staging.has_value() || !projectFile.has_value())
            return Result<ProjectCreation>::failure({ErrorCategory::InvalidArgument, "ProjectCreate.path"});
        ProjectCreation result{projectFile.take_value(), ids[0], a_desc.name, a_desc.engineVersion};
        StagingOwner owner(a_files);
        auto started = owner.directory(*staging.try_value());
        if (!started.has_value())
            return owner.fail(*started.try_error());
        auto marker = staging.try_value()->join(".cue-create-owner");
        if (!marker.has_value())
            return owner.fail(*marker.try_error());
        auto owned = owner.file(*marker.try_value(), ids[5], true);
        if (!owned.has_value())
            return owner.fail(*owned.try_error());
        auto existing = a_files.list_directory(*staging.try_value());
        if (!existing.has_value() || existing.try_value()->size() != 1)
            return owner.fail({ErrorCategory::InvalidState, "ProjectCreate.staging_not_empty"});
        for (const auto *relative : {"Source", "Content", "Content/Scenes"})
        {
            auto directory = staging.try_value()->join(relative);
            if (!directory.has_value())
                return owner.fail(*directory.try_error());
            auto made = owner.directory(*directory.try_value());
            if (!made.has_value())
                return owner.fail(*made.try_error());
        }
        for (const auto &file : planned)
        {
            auto path = staging.try_value()->join(file.path);
            if (!path.has_value())
                return owner.fail(*path.try_error());
            auto written = owner.file(*path.try_value(), file.text);
            if (!written.has_value())
                return owner.fail(*written.try_error());
        }
        // 公開前に Project と Scene の実 Parser で検証し、元 Source の手編集値は一切流用しない
        auto stagedProject = staging.try_value()->join(a_desc.applicationName + ".cueproj");
        if (!stagedProject.has_value())
            return owner.fail(*stagedProject.try_error());
        auto validated = Project::load(a_files, *stagedProject.try_value());
        if (!validated.has_value())
            return owner.fail(*validated.try_error());
        auto scene = Scene::load(a_files, *validated.try_value()->startup_scene_file());
        if (!scene.has_value())
            return owner.fail(*scene.try_error());
        auto released = owner.release_marker();
        if (!released.has_value())
            return owner.fail(*released.try_error());
        auto published = a_files.rename(*staging.try_value(), a_desc.directory);
        if (!published.has_value())
            return owner.fail(*published.try_error());
        owner.isPublished = true;
        return Result<ProjectCreation>::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        return Result<ProjectCreation>::failure({ErrorCategory::PlatformFailure, "ProjectCreate.allocation"});
    }
    catch (...)
    {
        return Result<ProjectCreation>::failure({ErrorCategory::PlatformFailure, "ProjectCreate.exception"});
    }
}
} // namespace cue
