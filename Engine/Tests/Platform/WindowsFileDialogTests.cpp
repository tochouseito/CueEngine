#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Platform/Windows/WindowsFileDialog.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Platform/Windows/WindowsWindowInterop.h>

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 契約違反をTest Processの固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(75);
    }

    /// @brief Message付き契約違反をTest Processの固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

class ForeignWindow final : public cue::Window
{
  public:
    /// @brief Test用異種Windowを表示済みとして扱う
    [[nodiscard]] cue::Result<void> show() noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief Test用異種Windowを破棄済みとして扱う
    [[nodiscard]] cue::Result<void> destroy() noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief Test用異種WindowのLifecycle状態を返す
    [[nodiscard]] cue::WindowState state() const noexcept override
    {
        return cue::WindowState::Created;
    }

    /// @brief Test用異種WindowのClient Sizeを返す
    [[nodiscard]] cue::WindowSize client_size() const noexcept override
    {
        return {320U, 180U};
    }

    /// @brief Test用異種WindowがEventを持たないことを返す
    [[nodiscard]] bool try_pop_event(cue::WindowEvent &) noexcept override
    {
        return false;
    }
};

/// @brief Test用Loggerを追加Sinkなしで生成する
[[nodiscard]] std::unique_ptr<cue::Logger> create_logger(TestFatalHandler &a_handler)
{
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    return std::make_unique<cue::Logger>(a_handler, std::move(sinks));
}

/// @brief Resultが指定Windows File Dialog Errorを保持するか判定する
template <typename T>
[[nodiscard]] bool has_dialog_error(const cue::Result<T> &a_result, cue::WindowsFileDialogError a_error) noexcept
{
    return !a_result && a_result.try_error() != nullptr &&
           a_result.try_error()->code().domain() == "Cue.Platform.Windows" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief UTF変換失敗が安定分類のCauseとして保持されたか判定する
template <typename T> [[nodiscard]] bool has_utf8_conversion_cause(const cue::Result<T> &a_result) noexcept
{
    return !a_result && a_result.try_error() != nullptr && !a_result.try_error()->causes().empty() &&
           a_result.try_error()->causes().front().code().domain() == "Cue.Platform.Windows" &&
           a_result.try_error()->causes().front().code().value() == 2;
}

/// @brief Test用Windows WindowをCreated状態で生成する
[[nodiscard]] cue::Result<std::unique_ptr<cue::Window>> create_test_window(cue::WindowSystem &a_system) noexcept
{
    const cue::WindowDescriptor descriptor{"File Dialog Test", {640U, 360U}};
    return a_system.create_window(descriptor);
}

/// @brief 新しいOwner Tokenで要求を表示し、Modal表示前の入力検証結果を返す
[[nodiscard]] cue::Result<cue::FileDialogResult> show_request(cue::FileDialogService &a_service, cue::Window &a_window,
                                                              cue::FileDialogKind a_kind,
                                                              std::vector<cue::FileDialogFilter> a_filters,
                                                              std::string a_defaultExtension,
                                                              std::string a_initialLocation,
                                                              const cue::AssertContext &a_context) noexcept
{
    cue::Result<cue::FileDialogOwnerToken> owner = cue::create_windows_file_dialog_owner(a_window, a_context);
    if (!owner)
    {
        return cue::Result<cue::FileDialogResult>::failure(std::move(*owner.try_error()));
    }
    cue::FileDialogRequest request(a_kind, std::move(a_filters), std::move(a_defaultExtension),
                                   std::move(a_initialLocation), std::move(*owner.try_value()));
    return a_service.show(request);
}

/// @brief Modal UIを表示せずOwner、Request、Thread失敗境界を検証する
[[nodiscard]] bool run_automated_tests(cue::AssertContext &a_context, cue::WindowSystem &a_system) noexcept
{
    ForeignWindow foreignWindow;
    cue::Result<cue::FileDialogOwnerToken> foreignOwner =
        cue::create_windows_file_dialog_owner(foreignWindow, a_context);
    if (!has_dialog_error(foreignOwner, cue::WindowsFileDialogError::OwnerUnavailable))
    {
        return false;
    }

    std::unique_ptr<cue::FileDialogService> service = cue::create_windows_file_dialog_service(a_context);
    cue::Result<std::unique_ptr<cue::Window>> destroyedWindowResult = create_test_window(a_system);
    if (!destroyedWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> destroyedWindow = std::move(*destroyedWindowResult.try_value());
    if (!destroyedWindow->destroy())
    {
        return false;
    }
    cue::Result<cue::FileDialogOwnerToken> destroyedOwner =
        cue::create_windows_file_dialog_owner(*destroyedWindow, a_context);
    if (!has_dialog_error(destroyedOwner, cue::WindowsFileDialogError::OwnerUnavailable))
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::Window>> ownerThreadWindowResult = create_test_window(a_system);
    if (!ownerThreadWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> ownerThreadWindow = std::move(*ownerThreadWindowResult.try_value());
    bool rejectedOwnerCreationThread = false;
    /// @brief 別ThreadからのOwner Token生成がAssert終了せず通常Errorになることを検証する
    std::thread ownerWorker(
        [&ownerThreadWindow, &a_context, &rejectedOwnerCreationThread]() noexcept
        {
            cue::Result<cue::FileDialogOwnerToken> owner =
                cue::create_windows_file_dialog_owner(*ownerThreadWindow, a_context);
            rejectedOwnerCreationThread = has_dialog_error(owner, cue::WindowsFileDialogError::OwnerThreadViolation);
        });
    ownerWorker.join();
    if (!rejectedOwnerCreationThread || !ownerThreadWindow->destroy())
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::Window>> invalidWindowResult = create_test_window(a_system);
    if (!invalidWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> invalidWindow = std::move(*invalidWindowResult.try_value());
    cue::Result<cue::FileDialogOwnerToken> invalidOwner =
        cue::create_windows_file_dialog_owner(*invalidWindow, a_context);
    if (!invalidOwner)
    {
        return false;
    }

    std::string embeddedNull("cu\0e", 4U);
    cue::FileDialogRequest invalidRequest(cue::FileDialogKind::SaveFile, {}, std::move(embeddedNull), {},
                                          std::move(*invalidOwner.try_value()));
    cue::Result<cue::FileDialogResult> invalidResult = service->show(invalidRequest);
    if (!has_dialog_error(invalidResult, cue::WindowsFileDialogError::InvalidRequest) || !invalidWindow->destroy())
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::Window>> utfWindowResult = create_test_window(a_system);
    if (!utfWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> utfWindow = std::move(*utfWindowResult.try_value());
    const std::string invalidUtf8("\xC3\x28", 2U);
    cue::Result<cue::FileDialogResult> invalidFilterName =
        show_request(*service, *utfWindow, cue::FileDialogKind::OpenFile, {{invalidUtf8, "*.*"}}, {}, {}, a_context);
    cue::Result<cue::FileDialogResult> invalidFilterPattern = show_request(
        *service, *utfWindow, cue::FileDialogKind::OpenFile, {{"All Files", invalidUtf8}}, {}, {}, a_context);
    cue::Result<cue::FileDialogResult> invalidExtension =
        show_request(*service, *utfWindow, cue::FileDialogKind::SaveFile, {}, invalidUtf8, {}, a_context);
    cue::Result<cue::FileDialogResult> invalidLocation =
        show_request(*service, *utfWindow, cue::FileDialogKind::SelectFolder, {}, {}, invalidUtf8, a_context);
    if (!has_dialog_error(invalidFilterName, cue::WindowsFileDialogError::InvalidRequest) ||
        !has_dialog_error(invalidFilterPattern, cue::WindowsFileDialogError::InvalidRequest) ||
        !has_dialog_error(invalidExtension, cue::WindowsFileDialogError::InvalidRequest) ||
        !has_dialog_error(invalidLocation, cue::WindowsFileDialogError::InvalidRequest) ||
        !has_utf8_conversion_cause(invalidFilterName) || !has_utf8_conversion_cause(invalidFilterPattern) ||
        !has_utf8_conversion_cause(invalidExtension) || !has_utf8_conversion_cause(invalidLocation) ||
        !utfWindow->destroy())
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::Window>> staleWindowResult = create_test_window(a_system);
    if (!staleWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> staleWindow = std::move(*staleWindowResult.try_value());
    cue::Result<cue::FileDialogOwnerToken> staleOwner = cue::create_windows_file_dialog_owner(*staleWindow, a_context);
    if (!staleOwner)
    {
        return false;
    }
    cue::FileDialogRequest staleRequest(cue::FileDialogKind::OpenFile, {}, {}, {}, std::move(*staleOwner.try_value()));
    if (!staleWindow->destroy())
    {
        return false;
    }
    cue::Result<cue::FileDialogResult> staleResult = service->show(staleRequest);
    if (!has_dialog_error(staleResult, cue::WindowsFileDialogError::OwnerUnavailable))
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::Window>> reusedWindowResult = create_test_window(a_system);
    if (!reusedWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> reusedWindow = std::move(*reusedWindowResult.try_value());
    cue::Result<cue::FileDialogOwnerToken> reusedOwner =
        cue::create_windows_file_dialog_owner(*reusedWindow, a_context);
    cue::Result<cue::NativeWindowView> reusedView = cue::get_native_window_view(*reusedWindow, a_context);
    if (!reusedOwner || !reusedView)
    {
        return false;
    }
    HWND reusedHandle = static_cast<HWND>(const_cast<void *>(reusedView.try_value()->value()));
    HANDLE originalGeneration = GetPropW(reusedHandle, L"CueEngine.DialogOwnerGeneration");
    HANDLE replacementGeneration = originalGeneration == reinterpret_cast<HANDLE>(1U) ? reinterpret_cast<HANDLE>(2U)
                                                                                      : reinterpret_cast<HANDLE>(1U);
    if (originalGeneration == nullptr ||
        SetPropW(reusedHandle, L"CueEngine.DialogOwnerGeneration", replacementGeneration) == FALSE)
    {
        return false;
    }
    cue::FileDialogRequest reusedRequest(cue::FileDialogKind::OpenFile, {}, {}, {},
                                         std::move(*reusedOwner.try_value()));
    cue::Result<cue::FileDialogResult> reusedResult = service->show(reusedRequest);
    if (SetPropW(reusedHandle, L"CueEngine.DialogOwnerGeneration", originalGeneration) == FALSE ||
        !has_dialog_error(reusedResult, cue::WindowsFileDialogError::OwnerUnavailable) || !reusedWindow->destroy())
    {
        return false;
    }

    cue::Result<std::unique_ptr<cue::Window>> threadWindowResult = create_test_window(a_system);
    if (!threadWindowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> threadWindow = std::move(*threadWindowResult.try_value());
    cue::Result<cue::FileDialogOwnerToken> threadOwner =
        cue::create_windows_file_dialog_owner(*threadWindow, a_context);
    if (!threadOwner)
    {
        return false;
    }

    cue::FileDialogRequest threadRequest(cue::FileDialogKind::SelectFolder, {}, {}, {},
                                         std::move(*threadOwner.try_value()));
    bool rejectedWrongThread = false;
    /// @brief Dialog要求を別Threadへ移してOwner Thread違反がModal表示前に拒否されることを検証する
    std::thread worker(
        [&service, &threadRequest, &rejectedWrongThread]() noexcept
        {
            cue::Result<cue::FileDialogResult> result = service->show(threadRequest);
            rejectedWrongThread = has_dialog_error(result, cue::WindowsFileDialogError::OwnerThreadViolation);
        });
    worker.join();

    return rejectedWrongThread && threadWindow->destroy().has_value();
}

/// @brief 指定KindのNative Dialogを手動検証用に一回表示する
[[nodiscard]] bool run_manual_dialog(std::string_view a_mode, cue::AssertContext &a_context,
                                     cue::WindowSystem &a_system) noexcept
{
    cue::Result<std::unique_ptr<cue::Window>> windowResult = create_test_window(a_system);
    if (!windowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> window = std::move(*windowResult.try_value());
    if (!window->show())
    {
        return false;
    }

    cue::Result<cue::FileDialogOwnerToken> owner = cue::create_windows_file_dialog_owner(*window, a_context);
    if (!owner)
    {
        return false;
    }

    cue::FileDialogKind kind = cue::FileDialogKind::OpenFile;
    std::vector<cue::FileDialogFilter> filters;
    std::string defaultExtension;
    if (a_mode == "save")
    {
        kind = cue::FileDialogKind::SaveFile;
        filters.push_back({"Cue Scene", "*.cuescene"});
        defaultExtension = "cuescene";
    }
    else if (a_mode == "folder")
    {
        kind = cue::FileDialogKind::SelectFolder;
    }
    else
    {
        filters.push_back({"Cue Project", "CueProject.json"});
        filters.push_back({"All Files", "*.*"});
    }

    cue::FileDialogRequest request(kind, std::move(filters), std::move(defaultExtension), {},
                                   std::move(*owner.try_value()));
    std::unique_ptr<cue::FileDialogService> service = cue::create_windows_file_dialog_service(a_context);
    cue::Result<cue::FileDialogResult> result = service->show(request);
    bool succeeded = false;
    if (!result)
    {
        const cue::Error *error = result.try_error();
        std::cerr << "Dialog failed: " << (error != nullptr ? error->summary() : std::string_view("unknown")) << '\n';
    }
    else if (result.try_value()->outcome() == cue::FileDialogOutcome::Cancelled)
    {
        std::cout << "Cancelled\n";
        succeeded = true;
    }
    else if (result.try_value()->selected_path().has_value())
    {
        std::cout << "Selected (unverified): " << *result.try_value()->selected_path() << '\n';
        succeeded = true;
    }

    cue::Result<void> destroyed = window->destroy();
    return succeeded && destroyed.has_value();
}
} // namespace

/// @brief Windows File Dialogの非Modal Integration Testまたは明示された手動Probeを実行する
int main(int a_argumentCount, char **a_arguments)
{
    TestFatalHandler handler;
    std::unique_ptr<cue::Logger> logger = create_logger(handler);
    cue::AssertContext context(*logger, handler);
    cue::Result<std::unique_ptr<cue::WindowSystem>> systemResult = cue::create_windows_window_system(context);
    if (!systemResult)
    {
        return 1;
    }
    std::unique_ptr<cue::WindowSystem> system = std::move(*systemResult.try_value());

    if (a_argumentCount == 1)
    {
        return run_automated_tests(context, *system) ? 0 : 1;
    }
    if (a_argumentCount == 2 &&
        (std::string_view(a_arguments[1]) == "open" || std::string_view(a_arguments[1]) == "save" ||
         std::string_view(a_arguments[1]) == "folder"))
    {
        return run_manual_dialog(a_arguments[1], context, *system) ? 0 : 1;
    }

    std::cerr << "Usage: CuePlatformWindowsFileDialogTests [open|save|folder]\n";
    return 2;
}
