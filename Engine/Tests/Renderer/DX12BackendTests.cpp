#include <memory>

#include <DX12/DX12RenderDevice.h>
#include <RHI/BackendFactory.h>

/// @brief 共通 Factory から DX12 Device を所有し、停止で借用を失効させる
int main()
{
    auto backendResult = cue::create_backend();
    if (!backendResult.has_value())
    {
        return 1;
    }
    std::unique_ptr<cue::IBackend> backend = backendResult.take_value();
    auto* device = dynamic_cast<cue::dx12::DX12RenderDevice*>(backend->get_render_device());
    if (!device || !device->device() || !device->factory() || !device->adapter())
    {
        return 2;
    }
    if (device->is_software_adapter() != device->is_warp())
    {
        return 3;
    }

    // 停止後に借用 Pointer を再取得できず、二重停止も安全に完了する
    if (!backend->shutdown().has_value() || backend->get_render_device() != nullptr ||
        !backend->shutdown().has_value())
    {
        return 4;
    }
    return 0;
}
