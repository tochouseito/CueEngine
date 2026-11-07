#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objbase.h>

#include <EngineInstallation/EngineInstallation.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Project/Project.h>
#include <ProjectTools/ProjectCreator.h>

namespace
{
/// @brief 文字列を JSON の所有値へ変換し、診断や表示名から Protocol 行を追加させない
std::string quoted(std::string_view a_text)
{
    std::string text = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : a_text)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\')
        {
            text += '\\';
            text += character;
        }
        else if (byte < 32)
        {
            text += "\\u00";
            text += hex[byte >> 4];
            text += hex[byte & 15];
        }
        else
            text += character;
    }
    return text + '"';
}

/// @brief 成功・失敗とも同じ Schema の UTF-8 JSON を一度だけ stdout へ返す
void respond(bool a_success, const cue::ProjectCreation &a_created, std::string_view a_message)
{
    const auto json = "{\"schemaVersion\":1,\"success\":" + std::string(a_success ? "true" : "false") +
                      ",\"projectFile\":" + quoted(a_created.projectFile.utf8()) +
                      ",\"projectId\":" + quoted(a_created.projectId) + ",\"name\":" + quoted(a_created.name) +
                      ",\"engineVersion\":" + quoted(a_created.engineVersion) + ",\"message\":" + quoted(a_message) +
                      "}\n";
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
}

/// @brief UUID の生成だけを Windows に委譲し、共通作成処理へ所有 UTF-8 値を渡す
cue::Result<std::string> generate_id()
{
    GUID guid{};
    const auto status = CoCreateGuid(&guid);
    if (FAILED(status))
        return cue::Result<std::string>::failure(
            {cue::ErrorCategory::PlatformFailure, "ProjectTool.CoCreateGuid", status});
    char text[37]{};
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2,
                  guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                  guid.Data4[6], guid.Data4[7]);
    return cue::Result<std::string>::success(text);
}

/// @brief 既知の Flag を一度ずつ読み、Path も Shell を介さない個別引数として受け取る
cue::Result<std::map<std::string, std::string>> read_arguments(int a_count, wchar_t **a_arguments)
{
    if (a_count < 4 || a_count > 14 || a_count % 2 != 0)
        return cue::Result<std::map<std::string, std::string>>::failure(
            {cue::ErrorCategory::InvalidArgument, "ProjectTool.arguments"});
    std::map<std::string, std::string> values;
    for (int index = 2; index < a_count; index += 2)
    {
        auto flag = cue::utf16_to_utf8(a_arguments[index]);
        auto value = cue::utf16_to_utf8(a_arguments[index + 1]);
        if (!flag.has_value() || !value.has_value() || value.try_value()->empty() || values.contains(*flag.try_value()))
            return cue::Result<std::map<std::string, std::string>>::failure(
                {cue::ErrorCategory::InvalidArgument, "ProjectTool.arguments"});
        values.emplace(flag.take_value(), value.take_value());
    }
    return cue::Result<std::map<std::string, std::string>>::success(std::move(values));
}
} // namespace

/// @brief Project 操作を独立 Process で実行し、例外も一つの JSON 失敗応答へ変換する
int wmain(int a_count, wchar_t **a_arguments)
{
    try
    {
        auto arguments = read_arguments(a_count, a_arguments);
        if (!arguments.has_value())
        {
            respond(false, {}, arguments.try_error()->operation);
            return 1;
        }
        auto createdFiles = cue::create_windows_file_system();
        if (!createdFiles.has_value())
        {
            respond(false, {}, createdFiles.try_error()->operation);
            return 1;
        }
        auto files = createdFiles.take_value();
        // 配布用 Bin/Tools では欠損 Descriptor を開発 Path で補完せず、同じ配置解決と Lease を使う
        auto developmentTemplates = cue::Path::create(CUE_PROJECT_TEMPLATES);
        auto installation = developmentTemplates.has_value()
                                ? cue::resolve_engine_installation(*files, developmentTemplates.try_value()->parent())
                                : cue::Result<cue::EngineInstallation>::failure(*developmentTemplates.try_error());
        if (!installation.has_value())
        {
            respond(false, {}, installation.try_error()->operation);
            return 1;
        }
        auto lease = cue::acquire_engine_use_lease(*files, *installation.try_value());
        if (!lease.has_value())
        {
            respond(false, {}, lease.try_error()->operation);
            return 1;
        }
        auto &values = *arguments.try_value();
        const std::wstring_view command(a_arguments[1]);
        if (command == L"validate" && values.size() == 1 && values.contains("--project"))
        {
            auto path = cue::Path::create(values["--project"]);
            auto project = path.has_value() && path.try_value()->is_absolute()
                               ? cue::Project::load(*files, *path.try_value())
                               : cue::Result<cue::Project>::failure(
                                     {cue::ErrorCategory::InvalidArgument, "ProjectTool.absolute_project"});
            if (!project.has_value())
            {
                respond(false, {}, project.try_error()->operation);
                return 1;
            }
            const auto &descriptor = project.try_value()->descriptor();
            // 一覧登録では別 Engine の指定も所有 Metadata として返す。実行・Build 時の一致検証は別境界で行う
            respond(true,
                    {project.try_value()->file(), descriptor.projectId, descriptor.name,
                     descriptor.engineVersion.value_or("")},
                    "Validated");
            return 0;
        }
        if (command != L"create" || values.size() != 5 || !values.contains("--directory") ||
            !values.contains("--name") || !values.contains("--application-name") || !values.contains("--template") ||
            !values.contains("--engine-version"))
        {
            respond(false, {}, "ProjectTool.arguments");
            return 1;
        }
        auto directory = cue::Path::create(values["--directory"]);
        auto templates = installation.try_value()->root.join("Templates");
        if (!directory.has_value() || !templates.has_value() ||
            values["--engine-version"] != installation.try_value()->version ||
            (values["--template"] != "Empty3D" && values["--template"] != "RotateCube"))
        {
            respond(false, {}, "ProjectTool.create_arguments");
            return 1;
        }
        cue::ProjectCreateDesc descriptor{
            directory.take_value(), values["--name"], values["--application-name"], values["--engine-version"],
            values["--template"] == "RotateCube" ? cue::ProjectTemplate::RotateCube : cue::ProjectTemplate::Empty3D};
        auto result = cue::create_project(*files, *templates.try_value(), descriptor, generate_id);
        if (!result.has_value())
        {
            respond(false, {}, result.try_error()->operation);
            return 1;
        }
        respond(true, *result.try_value(), "Created");
        return 0;
    }
    catch (...)
    {
        // Allocation 失敗の境界でも追加確保を行わず、Protocol の形だけを保持する
        std::fputs("{\"schemaVersion\":1,\"success\":false,\"projectFile\":\"\",\"projectId\":\"\",\"name\":\"\","
                   "\"engineVersion\":\"\",\"message\":\"ProjectTool.exception\"}\n",
                   stdout);
        std::fflush(stdout);
        return 2;
    }
}
