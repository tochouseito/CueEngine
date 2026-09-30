#include <Cue/Platform/Clock.h>
#include <Cue/Platform/Diagnostics.h>
#include <Cue/Platform/Thread.h>
#include <Cue/Platform/Waiter.h>
#include <Cue/Platform/Window.h>
#include <Cue/Platform/WindowEvent.h>
#include <Cue/Platform/WindowSystem.h>
#include <Cue/Renderer/RHI/Backend.h>
#include <Cue/Renderer/RHI/BufferManager.h>
#include <Cue/Renderer/RHI/Command.h>
#include <Cue/Renderer/RHI/GpuExecution.h>
#include <Cue/Renderer/RHI/GpuCommands.h>
#include <Cue/Renderer/RHI/GpuPipelines.h>
#include <Cue/Renderer/RHI/GpuResources.h>
#include <Cue/Renderer/RHI/Queue.h>
#include <Cue/Renderer/RHI/PipelineManager.h>
#include <Cue/Renderer/RHI/RenderDevice.h>
#include <Cue/Renderer/RHI/TextureManager.h>
#include <Cue/Renderer/RHI/ViewManager.h>

#include <type_traits>

#ifdef _WINDOWS_
#error PlatformとRHIの公開HeaderはWindows SDKへ依存してはならない
#endif

/// @brief Platform公開HeaderがWin32型なしで単体Compileできることを確認する
int main()
{
    static_assert(std::is_abstract_v<cue::Clock>);
    static_assert(std::is_abstract_v<cue::Waiter>);
    static_assert(std::is_abstract_v<cue::Thread>);
    static_assert(std::is_abstract_v<cue::ThreadFactory>);
    static_assert(std::is_abstract_v<cue::IGpuExecution>);
    static_assert(std::is_abstract_v<cue::IBackend>);
    static_assert(std::is_abstract_v<cue::IQueueContext>);
    static_assert(std::is_abstract_v<cue::IQueuePool>);
    static_assert(std::is_abstract_v<cue::IGpuResources>);
    static_assert(std::is_abstract_v<cue::IGpuPipelines>);
    static_assert(std::is_abstract_v<cue::IGpuCommandRecorder>);
    static_assert(std::is_abstract_v<cue::IRenderDevice>);
    static_assert(std::is_abstract_v<cue::IBufferManager>);
    static_assert(std::is_abstract_v<cue::ITextureManager>);
    static_assert(std::is_abstract_v<cue::IViewManager>);
    static_assert(std::is_abstract_v<cue::IPipelineManager>);
    static_assert(std::is_abstract_v<cue::ICommandContext>);
    static_assert(std::is_abstract_v<cue::ICommandPool>);
    cue::WindowDescriptor descriptor{"CueEngine", {1280, 720}};
    return descriptor.clientSize.width == 1280 ? 0 : 1;
}
