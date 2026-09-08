#include <Cue/Editor/ImGui/PlaySessionPresenter.h>
#include <Cue/Editor/ImGui/SessionLog.h>

#include <Cue/EditorCore/EditorController.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/World.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

namespace
{
/// @brief Test中の回復不能状態をProcess失敗へ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固有Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Message付きFatalを固有Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

/// @brief F5 Start失敗を任意の一Sampleだけへ注入できる単調Clock
class TestClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief 16ms刻みの初期時刻を生成する
    TestClock() noexcept = default;

    /// @brief 次回Sampleだけを回復可能失敗にする
    void fail_next_sample() noexcept
    {
        m_shouldFail = true;
    }

    /// @brief 16ms刻みの時刻または予約済み失敗を返す
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        if (m_shouldFail)
        {
            m_shouldFail = false;
            cue::ErrorCode code =
                cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Editor.ImGui.PlayTest", 1);
            return cue::Result<cue::game_core::MonotonicClockSample>::failure(
                cue::Error::create(a_assertContext.fatal_handler(), std::move(code), "Injected UI Play failure"));
        }
        cue::game_core::MonotonicClockSample sample{m_nanoseconds};
        m_nanoseconds += 16'000'000;
        return cue::Result<cue::game_core::MonotonicClockSample>::success(std::move(sample));
    }

  private:
    std::int64_t m_nanoseconds = 0;
    bool m_shouldFail = false;
};

/// @brief Headless描画からRuntime Windowの利用者向け初期配置を保持する
struct RuntimeWindowLayout final
{
    ImVec2 position{};
    ImVec2 size{};
};

/// @brief 条件が偽なら失敗位置を保持したProcess終了へ変換する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::_Exit(static_cast<int>((a_location.line() % 200U) + 20U));
    }
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> [[nodiscard]] T take_value(cue::Result<T> a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief 固定IdentityからPlay UI Test用Project Descriptorを生成する
[[nodiscard]] cue::ProjectDescriptor make_project_descriptor(const cue::AssertContext &a_assertContext) noexcept
{
    cue::ProjectId projectId =
        take_value(cue::ProjectId::parse("00000000-0000-4000-8000-000000000215", a_assertContext));
    return take_value(cue::create_blank_project_descriptor(
        std::move(projectId), "Play UI Test", cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt},
        a_assertContext));
}

/// @brief Runtime Core Typeを持つSeal済みTest Registryを生成する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_runtime_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(cue::runtime::add_runtime_schema_types(builder, a_assertContext).has_value());
    return take_value(builder.seal());
}

/// @brief ImGuiへKey押下を一Frameだけ送りPresenterのShortcut経路を実行する
void press_shortcut(cue::editor::PlaySessionPresenter &a_presenter, ImGuiKey a_key, bool a_hasShift) noexcept
{
    ImGuiIO &input = ImGui::GetIO();
    if (a_hasShift)
    {
        input.AddKeyEvent(ImGuiMod_Shift, true);
    }
    input.AddKeyEvent(a_key, true);
    ImGui::NewFrame();
    a_presenter.process_shortcuts();
    a_presenter.draw();
    a_presenter.advance_runtime();
    ImGui::Render();
    input.AddKeyEvent(a_key, false);
    if (a_hasShift)
    {
        input.AddKeyEvent(ImGuiMod_Shift, false);
    }
}

/// @brief Key解放Eventを一Frame処理して次のShortcut検証から分離する
void release_shortcut(cue::editor::PlaySessionPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    a_presenter.process_shortcuts();
    a_presenter.draw();
    a_presenter.advance_runtime();
    ImGui::Render();
}

/// @brief Presenter初回描画後のRuntime Window位置とSizeをPublic ImGui APIから取得する
[[nodiscard]] RuntimeWindowLayout capture_runtime_window_layout(
    cue::editor::PlaySessionPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    a_presenter.draw();
    const bool isVisible = ImGui::Begin("Runtime");
    require(isVisible);
    RuntimeWindowLayout layout{ImGui::GetWindowPos(), ImGui::GetWindowSize()};
    ImGui::End();
    ImGui::Render();
    return layout;
}

/// @brief Filter、Clear、単一購読、Token破棄後配送停止を検証する
void test_session_log_subscription(cue::Logger &a_logger, cue::editor::EditorSessionLogRouter &a_router,
                                   const cue::AssertContext &a_assertContext) noexcept
{
    {
        std::unique_ptr<cue::editor::EditorSessionLogSubscription> subscription =
            take_value(a_router.subscribe(a_assertContext));
        require(a_router.has_active_subscription());
        subscription->set_session_generation(42U);
        require(a_logger.log(cue::LogLevel::Info, "Alpha Runtime message") == cue::LogResult::Success);
        require(a_logger.log(cue::LogLevel::Warning, "Beta Runtime message") == cue::LogResult::Success);
        require(subscription->snapshot("alpha").size() == 1U);
        require(subscription->snapshot("alpha").front().sessionGeneration == 42U);
        require(subscription->snapshot("RUNTIME").size() == 2U);
        require(!a_router.subscribe(a_assertContext).has_value());
        subscription->clear();
        require(subscription->snapshot({}).empty());
    }
    require(!a_router.has_active_subscription());
    require(a_logger.log(cue::LogLevel::Info, "Message after Session") == cue::LogResult::Success);
    std::unique_ptr<cue::editor::EditorSessionLogSubscription> next = take_value(a_router.subscribe(a_assertContext));
    require(next->snapshot({}).empty());
}

/// @brief F5／Shift+F5、無効操作抑止、終了Cancel、日本語失敗表示を検証する
void test_play_toolbar_and_shutdown(cue::Logger &a_logger, cue::editor::EditorSessionLogRouter &a_router,
                                    const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> schemaRegistry =
        make_runtime_registry(schemaIdentitySource, a_assertContext);
    cue::game_core::WorldIdentitySource worldIdentitySource;
    TestClock clock;
    std::unique_ptr<cue::editor_core::EditorController> editor =
        cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);

    cue::scene::SceneAssetId sceneId =
        take_value(cue::scene::SceneAssetId::parse("70000000-0000-4000-8000-000000000215", a_assertContext));
    cue::scene::SceneDocument scene = cue::scene::SceneDocument::create(std::move(sceneId), a_assertContext);
    cue::RelativePath locator = take_value(cue::RelativePath::parse("Scenes/PlayUi.cuescene", a_assertContext));
    const cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(std::move(scene), std::move(locator), true));
    cue::runtime::RuntimeSchemaTypeIds typeIds =
        take_value(cue::runtime::make_runtime_schema_type_ids(a_assertContext));
    std::unique_ptr<cue::editor_core::EditorPlaySessionController> controller =
        take_value(cue::editor_core::EditorPlaySessionController::create(
            editor->session(), worldIdentitySource, clock, *schemaRegistry, {}, std::move(typeIds.transform),
            std::move(typeIds.sceneObjectState), 1U, 100'000'000, a_assertContext));
    std::unique_ptr<cue::editor::PlaySessionPresenter> presenter =
        cue::editor::PlaySessionPresenter::create(*controller, a_logger, a_router, a_assertContext);

    require(!presenter->can_play());
    require(!presenter->submit(cue::editor::EditorPlaySessionCommand::Play));
    presenter->set_active_document(documentId);
    require(presenter->can_play());

    require(ImGui::CreateContext() != nullptr);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    static_cast<void>(input.Fonts->Build());

    const RuntimeWindowLayout layout = capture_runtime_window_layout(*presenter);
    require(layout.position.x > input.DisplaySize.x * 0.5F);
    require(layout.position.y >= 50.0F);
    require(layout.size.x >= 480.0F);
    require(layout.size.y >= 300.0F);

    release_shortcut(*presenter);
    press_shortcut(*presenter, ImGuiKey_F5, false);
    require(presenter->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Running);
    require(presenter->state_snapshot().frameCount == 1U);
    require(a_router.has_active_subscription());
    require(!presenter->can_play());
    const std::uint64_t generation = presenter->state_snapshot().generation;
    require(!presenter->submit(cue::editor::EditorPlaySessionCommand::Play));
    require(presenter->state_snapshot().generation == generation);
    release_shortcut(*presenter);

    presenter->advance_runtime();
    require(presenter->state_snapshot().frameCount == 3U);
    require(!presenter->begin_editor_shutdown());
    require(presenter->is_shutdown_confirmation_pending());
    require(!presenter->respond_to_editor_shutdown(cue::editor::EditorPlayShutdownDecision::Cancel));
    require(!presenter->is_shutdown_confirmation_pending());
    require(presenter->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Running);

    const std::uint64_t frameCountBeforeStop = presenter->state_snapshot().frameCount;
    press_shortcut(*presenter, ImGuiKey_F5, true);
    require(presenter->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Stopped);
    require(presenter->state_snapshot().frameCount == frameCountBeforeStop);
    require(!a_router.has_active_subscription());
    require(!presenter->can_stop());
    release_shortcut(*presenter);

    clock.fail_next_sample();
    require(presenter->submit(cue::editor::EditorPlaySessionCommand::Play));
    require(presenter->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Stopped);
    require(!a_router.has_active_subscription());
    require(presenter->has_error_message());
    require(presenter->message().find("失敗") != std::string_view::npos);
    require(presenter->message().find("Injected") == std::string_view::npos);
    require(presenter->begin_editor_shutdown());

    presenter.reset();
    require(!a_router.has_active_subscription());
    ImGui::DestroyContext();
}
} // namespace

/// @brief Play Session ImGuiの操作、診断、Session限定Log寿命をHeadless検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::unique_ptr<cue::editor::EditorSessionLogRouter> router =
        std::make_unique<cue::editor::EditorSessionLogRouter>();
    cue::editor::EditorSessionLogRouter *routerReference = router.get();
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    sinks.push_back(std::move(router));
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    test_session_log_subscription(logger, *routerReference, assertContext);
    require(!routerReference->has_active_subscription());
    test_play_toolbar_and_shutdown(logger, *routerReference, assertContext);
    require(!routerReference->has_active_subscription());
    return 0;
}
