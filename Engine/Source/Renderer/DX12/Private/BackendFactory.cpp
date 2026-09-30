#include <RHI/BackendFactory.h>

#include <utility>

#include <DX12/DX12Backend.h>

namespace cue
{
/// @brief 現在の Windows Backend として DX12Backend を生成する
Result<std::unique_ptr<IBackend>> create_backend()
{
    auto backendResult = dx12::DX12Backend::create();
    if (!backendResult.has_value())
    {
        return Result<std::unique_ptr<IBackend>>::failure(*backendResult.try_error());
    }
    return Result<std::unique_ptr<IBackend>>::success(std::move(backendResult.take_value()));
}
} // namespace cue
