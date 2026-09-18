#include <Cue/Renderer/Error.h>

#include <Cue/Foundation/Assert.h>

#include <utility>

namespace cue::renderer
{
Error make_renderer_error(const AssertContext &a_assertContext, RendererError a_code,
                          std::string_view a_summary) noexcept
{
    ErrorCode code =
        ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Renderer", static_cast<std::int64_t>(a_code));
    return Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}
} // namespace cue::renderer
