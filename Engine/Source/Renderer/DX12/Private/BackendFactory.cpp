#include <Cue/Renderer/RHI/BackendFactory.h>

#include <Cue/Renderer/DX12/DX12Backend.h>

#include <utility>

namespace cue
{
/// @brief Windows の Native Window を DX12 Backend の静的生成関数へ渡す
Result<std::unique_ptr<IBackend>> create_backend(void* a_nativeWindow, WindowSize a_clientSize)
{
    auto backendResult = DX12Backend::create(a_nativeWindow, a_clientSize);
    if (!backendResult.has_value())
    {
        return Result<std::unique_ptr<IBackend>>::failure(*backendResult.try_error());
    }
    return Result<std::unique_ptr<IBackend>>::success(std::move(backendResult.take_value()));
}
} // namespace cue
