#include <EngineInstallation/EngineInstallation.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include <EngineInstallation/EngineVersion.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Engine installation failed %d: %s\n", __LINE__, #__VA_ARGS__);                       \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Fixture の Path 生成失敗を Test の不正状態として止める
cue::Path path(cue::Result<cue::Path> a_path)
{
    if (!a_path.has_value())
        throw std::runtime_error("fixture path");
    return a_path.take_value();
}

/// @brief 限定した Test Root へ所有する小さい File だけを作る
bool write(cue::IFileSystem &a_files, const cue::Path &a_path, std::string_view a_text)
{
    return a_files.write_all(a_path, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief 現在の Binary と一致する配布 Descriptor を作る
std::string descriptor()
{
    return "formatVersion=1\nproduct=engine\nversion=" CUE_ENGINE_VERSION "\nbuildId=" CUE_ENGINE_BUILD_ID
           "\nplatform=Windows\narchitecture=x64\nconfiguration=Development\nsdkAbiVersion=1\n"
           "projectFormatMin=1\nprojectFormatMax=2\neditor=Bin/Editor/CueEditorHost.exe\n"
           "projectTool=Bin/Tools/CueProjectTool.exe\nuseLeaseId=engine-" CUE_ENGINE_VERSION "-windows-x64\n";
}

/// @brief 実行位置とユーザー保存先を Test Root の中へ限定する
class InstallationFiles final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief 実 FileSystem の借用と擬似配置を所有する
    InstallationFiles(cue::IFileSystem &a_files, cue::Path a_executable, cue::Path a_local)
        : FileSystemProbe(a_files), executable(std::move(a_executable)), local(std::move(a_local))
    {
    }
    /// @brief 実 Process の配置を変更せず、配布用の探索経路を再現する
    cue::Result<cue::Path> executable_directory() override
    {
        return cue::Result<cue::Path>::success(executable);
    }
    /// @brief 本物の LocalAppData に Test の Lease を残さない
    cue::Result<cue::Path> local_data_directory() override
    {
        return cue::Result<cue::Path>::success(local);
    }
    cue::Path executable;
    cue::Path local;
};

/// @brief 明示 CMake 設定の上書きをこの Test Process の寿命に限定する
class CmakeEnvironment final
{
  public:
    /// @brief 元の Process 環境変数を所有してから探索を分離する
    CmakeEnvironment()
    {
        const auto size = GetEnvironmentVariableW(L"CUE_CMAKE_EXECUTABLE", nullptr, 0);
        if (size)
        {
            m_value.resize(size);
            const auto count = GetEnvironmentVariableW(L"CUE_CMAKE_EXECUTABLE", m_value.data(), size);
            m_value.resize(count);
            m_hadValue = true;
        }
        SetEnvironmentVariableW(L"CUE_CMAKE_EXECUTABLE", nullptr);
    }
    /// @brief Test 成否にかかわらず元の環境を復元する
    ~CmakeEnvironment()
    {
        SetEnvironmentVariableW(L"CUE_CMAKE_EXECUTABLE", m_hadValue ? m_value.c_str() : nullptr);
    }

  private:
    std::wstring m_value;
    bool m_hadValue = false;
};
} // namespace

/// @brief 移動可能な配布識別、旧 Root への戻り禁止、共有 / 排他 Lease と入力拒否を確認する
int main()
{
    for (const auto *version : {"0.1.0", "1.2.3-preview.1", "1.2.3+local.01", "1.2.3-rc.1+build.2"})
        CHECK(cue::is_valid_engine_version(version));
    for (const auto *version : {"", "1", "1.2", "01.2.3", "1.2.03", "1.2.3-01", "1.2.3-", "1.2.3+", "1.2.3-preview..1",
                                "1.2.3/path", "1.2.3.4", "1.2.3+one+two"})
        CHECK(!cue::is_valid_engine_version(version));
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto root = path(fixture.root.join("Moved 日本語 Engine"));
    const auto editor = path(root.join("Bin/Editor/CueEditorHost.exe"));
    const auto tool = path(root.join("Bin/Tools/CueProjectTool.exe"));
    const auto config = path(root.join("engine.cueengine"));
    CHECK(write(files, editor, "fixture") && write(files, tool, "fixture"));
    CHECK(write(files, path(root.join("CMakeLists.txt")), "# fixture\n"));
    CHECK(write(files, path(root.join("CMakePresets.json")), "{}\n"));
    const auto original = descriptor();
    CHECK(write(files, config, original));
    auto loaded = cue::load_engine_installation(files, root);
    CHECK(loaded.has_value() && loaded.try_value()->isPackaged && loaded.try_value()->version == CUE_ENGINE_VERSION);
    CHECK(loaded.try_value()->root.utf8() == root.utf8() &&
          loaded.try_value()->editorExecutable.utf8() == editor.utf8());
    InstallationFiles probe(files, editor.parent(), path(fixture.root.join("UserData")));
    auto resolved = cue::resolve_engine_installation(probe, fixture.root);
    CHECK(resolved.has_value() && resolved.try_value()->isPackaged && resolved.try_value()->root.utf8() == root.utf8());

    // 同じ Engine は共有利用できるが、Hub 相当の修復・削除の排他とは両方向で競合する
    auto first = cue::acquire_engine_use_lease(probe, *loaded.try_value());
    auto second = cue::acquire_engine_use_lease(probe, *loaded.try_value());
    CHECK(first.has_value() && *first.try_value() && second.has_value() && *second.try_value());
    const auto lock = path(probe.local.join("CueEngine/Locks/" + loaded.try_value()->useLeaseId + ".lock"));
    auto exclusive = files.open(lock, {cue::FileAccess::ReadWrite, cue::FileCreation::OpenAlways, false, false, false});
    CHECK(!exclusive.has_value());
    first.try_value()->reset();
    second.try_value()->reset();
    exclusive = files.open(lock, {cue::FileAccess::ReadWrite, cue::FileCreation::OpenAlways, false, false, false});
    CHECK(exclusive.has_value());
    CHECK(!cue::acquire_engine_use_lease(probe, *loaded.try_value()).has_value());
    exclusive.try_value()->reset();
    cue::EngineInstallation development;
    auto noLease = cue::acquire_engine_use_lease(probe, development);
    CHECK(noLease.has_value() && !*noLease.try_value());

    const std::pair<std::string_view, std::string_view> invalidValues[]{
        {"formatVersion=1", "formatVersion=2"},
        {"product=engine", "product=hub"},
        {"architecture=x64", "architecture=arm64"},
        {"sdkAbiVersion=1", "sdkAbiVersion=2"},
        {"projectFormatMax=2", "projectFormatMax=3"},
        {"editor=Bin/Editor/CueEditorHost.exe", "editor=../outside.exe"},
        {"projectTool=Bin/Tools/CueProjectTool.exe", "projectTool=C:/outside.exe"}};
    for (const auto &[from, to] : invalidValues)
    {
        auto invalid = original;
        invalid.replace(invalid.find(from), from.size(), to);
        CHECK(write(files, config, invalid));
        CHECK(!cue::load_engine_installation(probe, root).has_value());
    }
    CHECK(write(files, config, original + "unknown=value\n"));
    CHECK(!cue::resolve_engine_installation(probe, fixture.root).has_value());
    CHECK(write(files, config, original + "version=0.1.0\n"));
    CHECK(!cue::load_engine_installation(probe, root).has_value());
    CHECK(files.remove(config).has_value());
    CHECK(!cue::resolve_engine_installation(probe, fixture.root).has_value());
    CHECK(write(files, config, original));

    CmakeEnvironment environment;
    const auto cmake = path(root.join("Tools/CMake/bin/cmake.exe"));
    CHECK(write(files, cmake, "fixture"));
    auto located = cue::locate_engine_cmake(probe, *loaded.try_value());
    CHECK(located.has_value() && located.try_value()->utf8() == cmake.utf8());
    CHECK(SetEnvironmentVariableW(L"CUE_CMAKE_EXECUTABLE", L"C:\\missing-cue-test\\cmake.exe"));
    CHECK(!cue::locate_engine_cmake(probe, *loaded.try_value()).has_value());
    return 0;
}
