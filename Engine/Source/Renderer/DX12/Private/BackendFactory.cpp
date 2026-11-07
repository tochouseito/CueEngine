#include <RHI/BackendFactory.h>

#include <utility>

#include <DX12/DX12Backend.h>

namespace cue
{
/// @brief 現在の Windows Backend として DX12Backend を生成する
Result<std::unique_ptr<IBackend>> create_backend(IFileSystem *a_files, Clock *a_clock)
{
    // Platform で採用する DX12 Backend の生成と所有を抽象契約へ接続する
    auto backendResult = dx12::DX12Backend::create({}, a_files, a_clock);
    if (!backendResult.has_value())
    {
        // 生成失敗の Error は型だけを合わせて呼出側へ返す
        return Result<std::unique_ptr<IBackend>>::failure(*backendResult.try_error());
    }
    // 具体 Backend の一意所有権を IBackend の所有権へ移す
    return Result<std::unique_ptr<IBackend>>::success(std::move(backendResult.take_value()));
}
} // namespace cue
