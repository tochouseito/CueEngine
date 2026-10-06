#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

#include <Foundation/Result.h>
#include <Platform/Window.h>
#include <Runtime/FrameController.h>

namespace cue
{
class IBackend;
class FrameGraphContext;

/// @brief Editor が所有する Layout 保存先と Font / Style の初期設定
struct ImGuiManagerConfig final
{
    // 空なら保存しない。相対 Path は実行時の Working Directory を基準とする
    std::string settingsFile = "out/editor/imgui.ini";
    float fontSize = 18.0f;
    bool isDockingEnabled = true;
    // Renderer 共有 Heap と分離した UI 専用 SRV Heap の容量
    std::uint32_t rendererDescriptorCapacity = 64;
};

/// @brief 具体 GPU 型を公開せず UI Backend の所有と利用状況を返す
struct ImGuiRendererInfo final
{
    std::uint32_t frameCount = 0;
    std::uint32_t descriptorCapacity = 0;
    std::uint32_t activeDescriptors = 0;
    std::uint64_t recordedFrames = 0;
};

/// @brief 最後に確定した UI Frame の入力 Capture と CPU 描画 Data の概要
struct ImGuiFrameInfo final
{
    std::uint64_t frames = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    bool wantsMouse = false;
    bool wantsKeyboard = false;
    bool wantsTextInput = false;
};

using editorUiCallback = std::function<Result<void>()>;

/// @brief CPU 描画 Snapshot の受渡しと回収数を所有値で返す
struct ImGuiTransferInfo final
{
    std::uint64_t publishedFrames = 0;
    std::uint64_t consumedFrames = 0;
    std::uint64_t discardedFrames = 0;
    std::uint32_t pendingFrames = 0;
    std::thread::id renderThreadId;
};

/// @brief ImGui Context、Font、Style、Layout と Win32 入力接続を一意所有する
///
/// EditorHost が所有し、Window は shutdown 完了まで生存させる
/// UI 構築と破棄は生成 Thread に固定する。render_frame 内の記録だけ RenderThread に許可する
/// Runtime の Worker と Graph を停止してから、この Manager と下位 Backend を停止する
class ImGuiManager final
{
    struct CreateToken final
    {
    };

public:
    /// @brief create 内部だけで未初期化の Owner を生成する
    explicit ImGuiManager(CreateToken) noexcept;

    /// @brief Context と入力接続を確保して Win32 Backend を作成する
    ///
    /// 途中失敗時は部分生成物を回収し、成功時だけ所有権を返す
    [[nodiscard]] static Result<std::unique_ptr<ImGuiManager>> create(Window& a_window,
                                                                    ImGuiManagerConfig a_config = {});

    /// @brief 明示停止がない場合も Handler を解除してから Context を解放する
    ~ImGuiManager();

    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    /// @brief SwapChain を持つ Backend を借用し、UI 専用 Heap と公式 GPU Backend を生成する
    ///
    /// Frame 数は CPU 描画枠数の 1 / 2 に合わせ、初回 begin_frame より前に接続する
    /// Backend は shutdown 完了まで生存させる。通常失敗時は CPU UI 基盤を維持する
    /// 公式 Init の途中配列生成例外は安全に復元できないため Fatal 停止する
    [[nodiscard]] Result<void> initialize_renderer(IBackend &a_backend, std::uint32_t a_frameCount);

    /// @brief Renderer の枠数に合わせた Snapshot 転送を初回 UI 構築前に有効にする
    [[nodiscard]] Result<void> enable_frame_transfer();

    /// @brief Owner Thread で閉じた UI Frame を複製し、単調増加する Frame ID で公開する
    ///
    /// Texture 更新は旧 Snapshot の回収まで取消可能に待つ。満杯 / 二重公開は状態を変えず拒否する
    [[nodiscard]] Result<void> publish_frame(std::uint64_t a_frame, std::stop_token a_token = {});

    /// @brief 固定 RenderThread で Graph の記録・提出を囲み、成功・取消・失敗でも Snapshot を回収する
    ///
    /// a_record は同期呼出し限定。Present は戻った後に行う。同一 Thread の構成も利用できる
    [[nodiscard]] Result<void> render_frame(std::uint64_t a_frame, std::stop_token a_token,
                                            const FrameCallback &a_record);

    /// @brief Owner Thread へ転送状態を返す
    [[nodiscard]] Result<ImGuiTransferInfo> transfer_info() const;

    /// @brief 確定済み UI 描画を Graphics Pass の記録中 Context へ一度だけ記録する
    ///
    /// Pass は Write / RenderTarget を宣言して対象 RTV を先に設定する
    /// 直接記録では次の UI Frame 前に提出 / 破棄する。転送時は render_frame 内で該当 Snapshot を借用する
    /// GPU 完了後に Manager を停止する
    /// 外部記録後に同じ Context で描画を続ける場合は Pipeline / Target / Viewport / Binding を再設定する
    [[nodiscard]] Result<void> record_draw_data(FrameGraphContext &a_context);

    /// @brief GPU Backend の描画枠と Descriptor の所有数を Owner Thread へ返す
    [[nodiscard]] Result<ImGuiRendererInfo> renderer_info() const;

    /// @brief Win32 入力を取り込み UI Frame を開始し、この Context を Current にする
    ///
    /// end_frame または shutdown まで他の Context に切り替えない。二重開始を拒否する
    [[nodiscard]] Result<void> begin_frame();

    /// @brief CPU 描画 Data を確定し、開始前の Current Context に戻す
    [[nodiscard]] Result<void> end_frame();

    /// @brief この Context 上で UI Callback を実行し、失敗時も開いた Frame を回収する
    ///
    /// Callback からの再入は拒否する。Callback の例外も Result へ変換する
    [[nodiscard]] Result<void> build_frame(const editorUiCallback& a_buildUi = {});

    /// @brief 最後に確定した Frame の所有値を返す。入力は常に ImGui へ送る
    ///
    /// Capture は Gameplay 側へ渡す入力の抑制に使い、Window Lifecycle は抑制しない
    [[nodiscard]] Result<ImGuiFrameInfo> frame_info() const;

    /// @brief Layout を指定先へ保存する。空の保存先は成功として何もしない
    [[nodiscard]] Result<void> save_settings();

    /// @brief Handler 解除、Frame 回収、設定保存、Win32 停止、Context 破棄の順に停止する
    ///
    /// 複数回呼べる。設定保存失敗時も Context を解放し、Error を返す
    [[nodiscard]] Result<void> shutdown();

private:
    class State;

    /// @brief Owner Thread と Context の生存状態を検証する
    [[nodiscard]] Result<void> validate(const char* a_operation) const;

    /// @brief 失敗した UI Callback が残した Frame を描画 Data にせず終了する
    void cancel_frame() noexcept;

    std::thread::id m_ownerId;
    std::unique_ptr<State> m_state;
};
} // namespace cue
