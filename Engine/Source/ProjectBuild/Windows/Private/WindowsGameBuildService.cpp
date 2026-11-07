#include <ProjectBuild/Windows/WindowsGameBuildService.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <EngineInstallation/EngineInstallation.h>
#include <EngineInstallation/EngineVersion.h>
#include <Foundation/Utf8.h>
#include <Foundation/Windows/UtfConversion.h>
#include <GameRuntime/GameStartup.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuild.h>
#include <Scene/Scene.h>

#include "BuildProcess.h"

namespace cue
{
namespace
{
constexpr std::size_t k_maxLogBytes = 1024 * 1024;
constexpr DWORD k_pollReadBytes = 128 * 1024;

/// @brief Build 前提の種類を確認する。Link を通常 File / Directory として扱わない
Result<void> require_type(IFileSystem &a_files, const Path &a_path, FileType a_type)
{
    if (!a_path.is_absolute())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.absolute_path_required"});
    auto info = a_files.stat(a_path);
    if (!info.has_value())
        return Result<void>::failure(*info.try_error());
    if (!*info.try_value() || (**info.try_value()).type != a_type)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.file_kind:" + a_path.utf8()});
    return Result<void>::success();
}

/// @brief 生成先の全要素を順に検証・作成し、既存の Junction の先へ出力しない
Result<void> prepare_directory(IFileSystem &a_files, const Path &a_path)
{
    if (!a_path.is_absolute())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.absolute_directory_required"});
    std::vector<Path> ancestors;
    auto current = a_path;
    for (;;)
    {
        ancestors.push_back(current);
        auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            break;
        current = std::move(parent);
    }
    for (auto position = ancestors.rbegin(); position != ancestors.rend(); ++position)
    {
        auto info = a_files.stat(*position);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (!*info.try_value())
        {
            auto created = a_files.create_directories(*position);
            if (!created.has_value())
                return created;
        }
        auto valid = require_type(a_files, *position, FileType::Directory);
        if (!valid.has_value())
            return valid;
    }
    return Result<void>::success();
}

/// @brief 誤った Tool 配置を Window 作成後の Build 失敗まで持ち越さない
Result<void> validate_tools(IFileSystem &a_files, const GameBuildTools &a_tools, bool a_isPackaged)
{
    auto root = require_type(a_files, a_tools.engineRoot, FileType::Directory);
    auto cmake = require_type(a_files, a_tools.cmakeExecutable, FileType::Regular);
    if (!root.has_value() || !cmake.has_value())
        return Result<void>::failure(!root.has_value() ? *root.try_error() : *cmake.try_error());
    if (!a_isPackaged)
    {
        auto vcpkg = require_type(a_files, a_tools.vcpkgRoot, FileType::Directory);
        auto toolchain = require_type(a_files, a_tools.toolchainFile, FileType::Regular);
        if (!vcpkg.has_value() || !toolchain.has_value())
            return Result<void>::failure(!vcpkg.has_value() ? *vcpkg.try_error() : *toolchain.try_error());
    }
    if (a_isPackaged)
    {
        // Source-backed SDK は同梱した Header / Library を直接検索し、vcpkg Tool を必要としない
        auto package = a_tools.engineRoot.join("ThirdParty/vcpkg_installed/x64-windows/share/imgui/imgui-config.cmake");
        if (!package.has_value())
            return Result<void>::failure(*package.try_error());
        auto valid = require_type(a_files, *package.try_value(), FileType::Regular);
        if (!valid.has_value())
            return valid;
    }
    for (const auto *name : {"CMakeLists.txt", "CMakePresets.json"})
    {
        auto file = a_tools.engineRoot.join(name);
        if (!file.has_value())
            return Result<void>::failure(*file.try_error());
        auto result = require_type(a_files, *file.try_value(), FileType::Regular);
        if (!result.has_value())
            return result;
    }
    return Result<void>::success();
}

/// @brief 他所から起動した製品も上書き不可能なら開始前に拒否する。後続の競合は Package の失敗で検出する
Result<void> check_replaceable(IFileSystem &a_files, const Path &a_executable)
{
    auto info = a_files.stat(a_executable);
    if (!info.has_value())
        return Result<void>::failure(*info.try_error());
    if (!*info.try_value())
        return Result<void>::success();
    if ((**info.try_value()).type != FileType::Regular)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.player_file_kind"});
    auto native = utf8_to_utf16(a_executable.utf8());
    if (!native.has_value())
        return Result<void>::failure(*native.try_error());
    detail::BuildHandle probe(CreateFileW(native.try_value()->c_str(), GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    return probe ? Result<void>::success()
                 : Result<void>::failure(detail::build_process_error("GameBuild.player_running_or_locked"));
}

/// @brief CMake が生成した Engine Solution だけを選び、複数形式の残存を曖昧に解決しない
Result<Path> find_solution(IFileSystem &a_files, const Path &a_tree)
{
    std::optional<Path> selected;
    for (const auto *name : {"CueEngine.slnx", "CueEngine.sln"})
    {
        auto file = a_tree.join(name);
        if (!file.has_value())
            return Result<Path>::failure(*file.try_error());
        auto info = a_files.stat(*file.try_value());
        if (!info.has_value())
            return Result<Path>::failure(*info.try_error());
        if (!*info.try_value())
            continue;
        if ((**info.try_value()).type != FileType::Regular || selected)
            return Result<Path>::failure({ErrorCategory::InvalidState, "GameBuild.solution_kind_or_ambiguous"});
        selected = file.take_value();
    }
    if (!selected)
        return Result<Path>::failure({ErrorCategory::InvalidState, "GameBuild.solution_missing"});
    return Result<Path>::success(std::move(*selected));
}

/// @brief Process 出力の一行を ImGui に渡せる UTF-8 へ揃え、埋込 NUL / 制御文字を除く
std::string display_text(std::string a_text)
{
    for (auto &character : a_text)
        if (static_cast<unsigned char>(character) < 32 && character != '\n' && character != '\r' && character != '\t')
            character = '?';
    if (is_valid_utf8_text(a_text))
        return a_text;
    const auto size = MultiByteToWideChar(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
    if (size <= 0)
        return "[Unrepresentable process output]\n";
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), wide.data(), size);
    auto converted = utf16_to_utf8(wide);
    return converted.has_value() ? converted.take_value() : "[Unrepresentable process output]\n";
}

/// @brief Windows Process の具体所有を非公開にし、Main の poll だけで進行する
class WindowsGameBuildService final : public IGameBuildService
{
  public:
    /// @brief Factory で検証済みの Tool と FileSystem を一意所有する
    WindowsGameBuildService(GameBuildTools a_tools, std::unique_ptr<IFileSystem> a_files,
                            std::optional<EngineInstallation> a_installation, std::unique_ptr<IFile> a_engineLease)
        : m_tools(std::move(a_tools)), m_files(std::move(a_files)), m_engineLease(std::move(a_engineLease)),
          m_installation(std::move(a_installation)), m_owner(std::this_thread::get_id())
    {
    }
    /// @brief Build 子孫だけを停止し、製品は独立して継続させる
    ~WindowsGameBuildService() override
    {
        try
        {
            auto stopped = shutdown();
            (void)stopped;
        }
        catch (...)
        {
        }
    }
    /// @brief 専用 Tree と Project 排他を準備し、CMake Configure を開始する
    Result<void> start(const Project &a_project, const GameBuildTarget &a_target) override;
    /// @brief IDE と開発 DLL を同じ生成 Tree に接続し、製品出力と区別する
    Result<void> start(const Project &a_project, const GameBuildTarget &a_target, GameBuildPurpose a_purpose) override;
    /// @brief 出力を上限付きで読み、Configure / Build / 製品の終了を進める
    Result<void> poll() override;
    /// @brief Job の子孫を停止要求し、次の poll で Cancelled へ移す
    Result<void> cancel() override;
    /// @brief 完成配布物を再検証して独立製品を開始する
    Result<void> launch() override;
    /// @brief 稼働していない Project の要求と結果を忘れ、Tool と Engine Lease は継続所有する
    Result<void> reset_project() override;
    /// @brief Build 停止を有界待機し、所有資源を回収する
    Result<void> shutdown() override;
    /// @brief 次の操作まで有効な現在値を Owner へ借用させる
    const GameBuildStatus &status() const noexcept override
    {
        return m_status;
    }

  private:
    /// @brief Owner Thread と停止状態を検証する
    Result<void> check_access() const;
    /// @brief ログの末尾を UTF-8 の文字境界で保持する
    void append_log(std::string a_text);
    /// @brief Pipe を待たずに一回の予算まで読み、EOF を所有値で返す
    Result<bool> drain_output();
    /// @brief 未完了の行を失わず、表示可能な行だけログへ公開する
    void consume_output(std::string_view a_bytes, bool a_flush);
    /// @brief 状態を失敗へ確定し、Build Job と Project 排他を回収する
    Result<void> fail(Error a_error);
    /// @brief Marker / Scene / exe と要求時点の Project / Target を再検証する
    Result<void> validate_player();
    /// @brief 完成 Manifest と DLL / Static の Hash、隣接する PDB を確認する
    Result<void> validate_module();
    /// @brief 所有製品の終了を非待機で確認する
    Result<void> poll_application();

    GameBuildTools m_tools;
    std::unique_ptr<IFileSystem> m_files;
    // 宣言順の逆順で Build Job を閉じてから Lease を解放し、FileSystem は最後まで保つ
    std::unique_ptr<IFile> m_engineLease;
    std::optional<EngineInstallation> m_installation;
    std::thread::id m_owner;
    GameBuildStatus m_status;
    std::optional<Project> m_project;
    Path m_tree;
    // 例外・停止失敗の最終 RAII 回収でも、Build Job を閉じてから Project の排他を解放する
    detail::BuildHandle m_lock;
    detail::BuildProcess m_build;
    detail::BuildHandle m_application;
    std::string m_pendingLog;
    bool m_isClosed = false;
    bool m_isCancelRequested = false;
};

Result<void> WindowsGameBuildService::check_access() const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "GameBuild.owner_thread"});
    if (m_isClosed)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.service_closed"});
    return Result<void>::success();
}

void WindowsGameBuildService::append_log(std::string a_text)
{
    m_status.log += a_text;
    if (m_status.log.size() > k_maxLogBytes)
    {
        auto first = m_status.log.size() - k_maxLogBytes;
        while (first < m_status.log.size() && (static_cast<unsigned char>(m_status.log[first]) & 0xc0) == 0x80)
            ++first;
        m_status.log.erase(0, first);
        m_status.isLogTruncated = true;
    }
}

void WindowsGameBuildService::consume_output(std::string_view a_bytes, bool a_flush)
{
    m_pendingLog += a_bytes;
    std::string batch;
    std::size_t first = 0;
    for (;;)
    {
        auto end = m_pendingLog.find('\n', first);
        if (end == std::string::npos)
        {
            if ((!a_flush && m_pendingLog.size() - first < 16 * 1024) || first == m_pendingLog.size())
                break;
            end = m_pendingLog.size() - 1;
        }
        batch += display_text(m_pendingLog.substr(first, end + 1 - first));
        first = end + 1;
    }
    // 短行の数に比例して 1 MiB の保持 Buffer を移動せず、Pipe chunk ごとに一度だけ確定する
    m_pendingLog.erase(0, first);
    if (!batch.empty())
        append_log(std::move(batch));
}

Result<bool> WindowsGameBuildService::drain_output()
{
    if (!m_build.output)
        return Result<bool>::success(true);
    DWORD budget = k_pollReadBytes;
    std::array<char, 8192> buffer{};
    while (budget)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(m_build.output.get(), nullptr, 0, nullptr, &available, nullptr))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE)
            {
                consume_output({}, true);
                m_build.output.reset();
                return Result<bool>::success(true);
            }
            return Result<bool>::failure(detail::build_process_error("GameBuild.peek_output"));
        }
        if (!available)
            return Result<bool>::success(true);
        DWORD read = 0;
        const auto size = (std::min)({available, budget, static_cast<DWORD>(buffer.size())});
        if (!ReadFile(m_build.output.get(), buffer.data(), size, &read, nullptr))
            return Result<bool>::failure(detail::build_process_error("GameBuild.read_output"));
        if (!read)
            return Result<bool>::success(true);
        consume_output(std::string_view(buffer.data(), read), false);
        budget -= read;
    }
    return Result<bool>::success(false);
}

Result<void> WindowsGameBuildService::fail(Error a_error)
{
    if (m_build.job)
        TerminateJobObject(m_build.job.get(), 1);
    m_build.output.reset();
    // 非待機の失敗回収でも、子孫終了を次の poll が確認するまで排他を保持する
    if (!m_build.process)
        detail::release_build_lock(m_lock);
    m_status.state = GameBuildState::Failed;
    m_status.error = std::move(a_error);
    consume_output({}, true);
    return Result<void>::failure(*m_status.error);
}

Result<void> WindowsGameBuildService::start(const Project &a_project, const GameBuildTarget &a_target)
{
    return start(a_project, a_target, GameBuildPurpose::Player);
}

Result<void> WindowsGameBuildService::start(const Project &a_project, const GameBuildTarget &a_target,
                                            GameBuildPurpose a_purpose)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    try
    {
        auto observed = poll_application();
        if (!observed.has_value())
            return observed;
        if (m_build.process || m_application)
            return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.already_running"});
        auto target = validate_game_build_target(a_target);
        if (!target.has_value())
            return target;
        if (a_purpose != GameBuildPurpose::Player && a_purpose != GameBuildPurpose::ScriptModule &&
            a_purpose != GameBuildPurpose::IdeProject)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.purpose_unsupported"});
        if (!a_project.descriptor().gameTarget ||
            (a_purpose == GameBuildPurpose::Player && !a_project.startup_scene_file()))
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.project_contract"});
        if (a_project.descriptor().engineVersion && *a_project.descriptor().engineVersion != CUE_ENGINE_VERSION)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.project_engine_version"});
        auto tools = validate_tools(*m_files, m_tools, m_installation.has_value());
        if (!tools.has_value())
            return tools;
        if (a_purpose == GameBuildPurpose::Player)
        {
            auto scene = Scene::load(*m_files, *a_project.startup_scene_file());
            if (!scene.has_value())
                return Result<void>::failure(*scene.try_error());
        }
        auto build = game_build_directory(a_project, a_target);
        if (!build.has_value())
            return Result<void>::failure(*build.try_error());
        auto output = a_purpose == GameBuildPurpose::Player ? build.try_value()->join("Player")
                                                            : Result<Path>::success(*build.try_value());
        if (!output.has_value())
            return Result<void>::failure(*output.try_error());
        Path executable;
        if (a_purpose == GameBuildPurpose::Player)
        {
            const auto *platform = find_game_build_platform(a_target.platform);
            auto file = output.try_value()->join(a_project.descriptor().applicationName +
                                                 std::string(platform->applicationSuffix));
            if (!file.has_value())
                return Result<void>::failure(*file.try_error());
            auto replaceable = check_replaceable(*m_files, *file.try_value());
            if (!replaceable.has_value())
                return replaceable;
            executable = file.take_value();
        }
        auto tree = a_project.root().join(
            std::string(a_purpose == GameBuildPurpose::Player ? "Intermediate/EditorBuild/" : "Intermediate/IDE/") +
            detail::build_path_identity(m_tools.engineRoot) + '/' + a_target.platform + '/' + a_target.architecture);
        if (!tree.has_value())
            return Result<void>::failure(*tree.try_error());
        auto directory = prepare_directory(*m_files, *tree.try_value());
        if (!directory.has_value())
            return directory;
        auto lock = detail::acquire_build_lock(a_project.root());
        if (!lock.has_value())
            return Result<void>::failure(*lock.try_error());
        std::vector<std::string> arguments{"--preset",
                                           "windows-vs2026",
                                           "-B",
                                           tree.try_value()->utf8(),
                                           "-DCUE_GAME_PROJECT=" + a_project.file().utf8(),
                                           "-DCUE_GAME_PLATFORM=" + a_target.platform,
                                           "-DCUE_GAME_ARCHITECTURE=" + a_target.architecture,
                                           "-DCMAKE_TOOLCHAIN_FILE=" + m_tools.toolchainFile.utf8(),
                                           "-DBUILD_TESTING=OFF",
                                           "-DCUE_BUILD_PERFORMANCE_TOOLS=OFF"};
        if (m_installation)
        {
            auto prefix = m_tools.engineRoot.join("ThirdParty/vcpkg_installed/x64-windows");
            if (!prefix.has_value())
                return Result<void>::failure(*prefix.try_error());
            // 配布 SDK の固定依存を使い、生成元の Toolchain / Registry を探索しない
            arguments.push_back("-DCMAKE_TOOLCHAIN_FILE=");
            arguments.push_back("-DCMAKE_PREFIX_PATH=" + prefix.try_value()->utf8());
            arguments.push_back("-DCUE_DISTRIBUTION_BUILD=ON");
        }
        auto process =
            detail::start_build_process(m_tools.cmakeExecutable, arguments, m_tools.engineRoot, m_tools.vcpkgRoot);
        if (!process.has_value())
            return Result<void>::failure(*process.try_error());
        // 全準備の成功後に旧結果を切り替え、開始前の入力失敗では再利用可能な結果を保つ
        m_project = a_project;
        m_tree = tree.take_value();
        m_lock = lock.take_value();
        m_build = process.take_value();
        m_pendingLog.clear();
        m_isCancelRequested = false;
        m_status = {};
        m_status.target = a_target;
        m_status.purpose = a_purpose;
        m_status.outputDirectory = output.take_value();
        m_status.executable = std::move(executable);
        m_status.state = GameBuildState::Configuring;
        append_log("[Configure] " + m_tree.utf8() + '\n');
        return Result<void>::success();
    }
    catch (...)
    {
        return fail({ErrorCategory::PlatformFailure, "GameBuild.start.exception"});
    }
}

Result<void> WindowsGameBuildService::validate_module()
{
    if (!m_project)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.no_request"});
    auto artifacts = load_game_build_artifacts(*m_files, *m_project, m_status.target);
    if (!artifacts.has_value())
        return Result<void>::failure(*artifacts.try_error());
    auto symbols = artifacts.try_value()->moduleFile.parent().join(*m_project->descriptor().gameTarget + ".pdb");
    if (!symbols.has_value())
        return Result<void>::failure(*symbols.try_error());
    auto regular = require_type(*m_files, *symbols.try_value(), FileType::Regular);
    if (!regular.has_value())
        return regular;
    // Manifest は従来の DLL / Static 契約を維持し、IDE 用 Symbol Path は今回の結果にだけ保持する
    m_status.moduleFile = artifacts.try_value()->moduleFile;
    m_status.symbolsFile = symbols.take_value();
    return Result<void>::success();
}

Result<void> WindowsGameBuildService::validate_player()
{
    if (!m_project)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.no_request"});
    auto startup = load_game_startup(*m_files, m_status.outputDirectory);
    if (!startup.has_value())
        return Result<void>::failure(*startup.try_error());
    const auto &value = *startup.try_value();
    const auto &project = m_project->descriptor();
    auto expectedScene = project.startupScene
                             ? Path::create("Content/" + project.startupScene->utf8())
                             : Result<Path>::failure({ErrorCategory::InvalidState, "GameBuild.startup_scene"});
    if (value.projectId != project.projectId || value.gameTarget != *project.gameTarget ||
        value.applicationName != project.applicationName || value.platform != m_status.target.platform ||
        value.architecture != m_status.target.architecture ||
        value.configuration != game_build_configuration_name(m_status.target.configuration) ||
        !expectedScene.has_value() || value.startupScene.utf8() != expectedScene.try_value()->utf8())
        return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.package_identity"});
    auto executable = require_type(*m_files, m_status.executable, FileType::Regular);
    if (!executable.has_value())
        return executable;
    for (const auto *name : {"dxcompiler.dll", "dxil.dll", "EngineResources/Shader/Hlsl/StaticMesh.hlsl",
                             "EngineResources/Shader/Hlsl/FullscreenTriangle.hlsl"})
    {
        auto required = m_status.outputDirectory.join(name);
        if (!required.has_value())
            return Result<void>::failure(*required.try_error());
        auto checked = require_type(*m_files, *required.try_value(), FileType::Regular);
        if (!checked.has_value())
            return checked;
        for (auto parent = required.try_value()->parent(); parent.utf8() != m_status.outputDirectory.utf8();
             parent = parent.parent())
        {
            auto directory = require_type(*m_files, parent, FileType::Directory);
            if (!directory.has_value())
                return directory;
        }
    }
    auto scenePath = m_status.outputDirectory.join(value.startupScene);
    if (!scenePath.has_value())
        return Result<void>::failure(*scenePath.try_error());
    auto scene = Scene::load(*m_files, *scenePath.try_value());
    return scene.has_value() ? Result<void>::success() : Result<void>::failure(*scene.try_error());
}

Result<void> WindowsGameBuildService::poll_application()
{
    if (!m_application)
        return Result<void>::success();
    const auto waited = WaitForSingleObject(m_application.get(), 0);
    if (waited == WAIT_TIMEOUT)
        return Result<void>::success();
    if (waited != WAIT_OBJECT_0)
        return Result<void>::failure(detail::build_process_error("GameBuild.wait_application"));
    DWORD code = 0;
    if (!GetExitCodeProcess(m_application.get(), &code))
        return Result<void>::failure(detail::build_process_error("GameBuild.application_exit_code"));
    m_status.applicationExitCode = code;
    m_status.isApplicationRunning = false;
    m_application.reset();
    detail::release_build_lock(m_lock);
    return Result<void>::success();
}

Result<void> WindowsGameBuildService::poll()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    try
    {
        auto application = poll_application();
        if (!application.has_value())
            return application;
        if (!m_build.process)
            return Result<void>::success();
        auto drained = drain_output();
        if (!drained.has_value())
            return fail(*drained.try_error());
        const auto waited = WaitForSingleObject(m_build.process.get(), 0);
        if (waited == WAIT_TIMEOUT)
            return Result<void>::success();
        if (waited != WAIT_OBJECT_0)
            return fail(detail::build_process_error("GameBuild.wait_process"));
        // Root が終了しても、残った Custom Command 子孫が Tree を使っていれば回収完了まで保持する
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(m_build.job.get(), JobObjectBasicAccountingInformation, &accounting,
                                       sizeof(accounting), nullptr))
            return fail(detail::build_process_error("GameBuild.job_status"));
        if (accounting.ActiveProcesses)
        {
            if (!TerminateJobObject(m_build.job.get(), 1))
                return fail(detail::build_process_error("GameBuild.stop_descendants"));
            return Result<void>::success();
        }
        if (!*drained.try_value())
            return Result<void>::success();
        consume_output({}, true);
        DWORD code = 0;
        if (!GetExitCodeProcess(m_build.process.get(), &code))
            return fail(detail::build_process_error("GameBuild.exit_code"));
        m_status.exitCode = code;
        m_build = {};
        if (m_status.state == GameBuildState::Failed)
        {
            detail::release_build_lock(m_lock);
            return Result<void>::success();
        }
        if (m_isCancelRequested)
        {
            m_status.state = GameBuildState::Cancelled;
            detail::release_build_lock(m_lock);
            append_log("[Cancelled]\n");
            return Result<void>::success();
        }
        if (code != 0)
            return fail({ErrorCategory::PlatformFailure,
                         m_status.state == GameBuildState::Configuring ? "GameBuild.configure_failed"
                                                                       : "GameBuild.build_failed",
                         code});
        if (m_status.state == GameBuildState::Configuring)
        {
            if (m_status.purpose != GameBuildPurpose::Player)
            {
                auto solution = find_solution(*m_files, m_tree);
                if (!solution.has_value())
                    return fail(*solution.try_error());
                m_status.solutionFile = solution.take_value();
                if (m_status.purpose == GameBuildPurpose::IdeProject)
                {
                    m_status.state = GameBuildState::Succeeded;
                    detail::release_build_lock(m_lock);
                    append_log("[Solution ready] " + m_status.solutionFile.utf8() + '\n');
                    return Result<void>::success();
                }
            }
            const std::vector<std::string> arguments{
                "--build",
                m_tree.utf8(),
                "--config",
                std::string(game_build_configuration_name(m_status.target.configuration)),
                "--target",
                m_status.purpose == GameBuildPurpose::Player ? "Cue.GamePlayerPackage" : "Cue.GameProjectBuild",
                "--parallel"};
            auto process =
                detail::start_build_process(m_tools.cmakeExecutable, arguments, m_tools.engineRoot, m_tools.vcpkgRoot);
            if (!process.has_value())
                return fail(*process.try_error());
            m_build = process.take_value();
            m_status.exitCode.reset();
            m_status.state = GameBuildState::Building;
            append_log("[Build / Package]\n");
            return Result<void>::success();
        }
        auto completed = m_status.purpose == GameBuildPurpose::Player ? validate_player() : validate_module();
        if (!completed.has_value())
            return fail(*completed.try_error());
        m_status.state = GameBuildState::Succeeded;
        detail::release_build_lock(m_lock);
        append_log("[Succeeded] " + m_status.outputDirectory.utf8() + '\n');
        return Result<void>::success();
    }
    catch (...)
    {
        return fail({ErrorCategory::PlatformFailure, "GameBuild.poll.exception"});
    }
}

Result<void> WindowsGameBuildService::cancel()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_build.process || m_isCancelRequested)
        return Result<void>::success();
    if (!TerminateJobObject(m_build.job.get(), ERROR_CANCELLED))
        return Result<void>::failure(detail::build_process_error("GameBuild.cancel"));
    m_isCancelRequested = true;
    append_log("[Cancellation requested]\n");
    return Result<void>::success();
}

Result<void> WindowsGameBuildService::launch()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    try
    {
        auto observed = poll_application();
        if (!observed.has_value())
            return observed;
        if (m_status.purpose != GameBuildPurpose::Player || m_status.state != GameBuildState::Succeeded ||
            m_build.process || m_application || !m_project)
            return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.launch_requires_success"});
        auto lock = detail::acquire_build_lock(m_project->root());
        if (!lock.has_value())
            return Result<void>::failure(*lock.try_error());
        auto valid = validate_player();
        if (!valid.has_value())
            return valid;
        auto process = detail::start_game_process(m_status.executable, m_status.outputDirectory);
        if (!process.has_value())
            return Result<void>::failure(*process.try_error());
        m_application = process.take_value();
        m_lock = lock.take_value();
        m_status.isApplicationRunning = true;
        m_status.applicationExitCode.reset();
        return Result<void>::success();
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "GameBuild.launch.exception"});
    }
}

Result<void> WindowsGameBuildService::reset_project()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    // 状態表示だけでなく Native 所有も確認し、終了をまだ poll していない Process を切り離さない
    if (m_build.process || m_build.job || m_build.output || m_application || m_status.isApplicationRunning ||
        m_status.state == GameBuildState::Configuring || m_status.state == GameBuildState::Building)
        return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.reset_project_busy"});
    // FileSystem / Tool / Engine の Lease は共通所有とし、Project に属する値だけを解放する
    detail::release_build_lock(m_lock);
    m_project.reset();
    m_tree = {};
    m_pendingLog.clear();
    m_isCancelRequested = false;
    m_status = {};
    return Result<void>::success();
}

Result<void> WindowsGameBuildService::shutdown()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "GameBuild.owner_thread"});
    if (m_isClosed)
        return Result<void>::success();
    if (m_build.process)
    {
        auto stopped = cancel();
        if (!stopped.has_value())
            return stopped;
        const auto deadline = GetTickCount64() + 5000;
        while (m_build.process && GetTickCount64() < deadline)
        {
            auto progressed = poll();
            if (!progressed.has_value())
                break;
            if (m_build.process)
                Sleep(1);
        }
        if (m_build.process)
            return Result<void>::failure({ErrorCategory::PlatformFailure, "GameBuild.shutdown_timeout"});
    }
    m_application.reset();
    m_status.isApplicationRunning = false;
    detail::release_build_lock(m_lock);
    m_engineLease.reset();
    m_isClosed = true;
    return Result<void>::success();
}
} // namespace

Result<std::unique_ptr<IGameBuildService>> create_windows_game_build_service(GameBuildTools a_tools)
{
    try
    {
        auto files = create_windows_file_system();
        if (!files.has_value())
            return Result<std::unique_ptr<IGameBuildService>>::failure(*files.try_error());
        std::optional<EngineInstallation> installation;
        std::unique_ptr<IFile> engineLease;
        auto descriptor = a_tools.engineRoot.join("engine.cueengine");
        if (!descriptor.has_value())
            return Result<std::unique_ptr<IGameBuildService>>::failure(*descriptor.try_error());
        auto info = (*files.try_value())->stat(*descriptor.try_value());
        if (!info.has_value())
            return Result<std::unique_ptr<IGameBuildService>>::failure(*info.try_error());
        if (*info.try_value())
        {
            auto loaded = load_engine_installation(**files.try_value(), a_tools.engineRoot);
            if (!loaded.has_value())
                return Result<std::unique_ptr<IGameBuildService>>::failure(*loaded.try_error());
            if (loaded.try_value()->version != CUE_ENGINE_VERSION || loaded.try_value()->buildId != CUE_ENGINE_BUILD_ID)
                return Result<std::unique_ptr<IGameBuildService>>::failure(
                    {ErrorCategory::InvalidArgument, "GameBuild.engine_binary_version"});
            installation = loaded.take_value();
            // 配布 SDK は外部 vcpkg に依存させず、古い設定を子の環境にも渡さない
            a_tools.toolchainFile = {};
            a_tools.vcpkgRoot = {};
            auto leased = acquire_engine_use_lease(**files.try_value(), *installation);
            if (!leased.has_value())
                return Result<std::unique_ptr<IGameBuildService>>::failure(*leased.try_error());
            engineLease = leased.take_value();
        }
        auto valid = validate_tools(**files.try_value(), a_tools, installation.has_value());
        if (!valid.has_value())
            return Result<std::unique_ptr<IGameBuildService>>::failure(*valid.try_error());
        return Result<std::unique_ptr<IGameBuildService>>::success(std::make_unique<WindowsGameBuildService>(
            std::move(a_tools), files.take_value(), std::move(installation), std::move(engineLease)));
    }
    catch (...)
    {
        return Result<std::unique_ptr<IGameBuildService>>::failure(
            {ErrorCategory::PlatformFailure, "GameBuild.create.exception"});
    }
}
} // namespace cue
