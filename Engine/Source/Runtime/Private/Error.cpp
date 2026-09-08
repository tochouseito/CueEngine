#include <Cue/Runtime/Error.h>

#include <Cue/Foundation/Assert.h>

namespace cue::runtime
{
Error make_runtime_error(const AssertContext &a_assertContext, RuntimeError a_code, std::string_view a_summary) noexcept
{
    ErrorCode code =
        ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Runtime", static_cast<std::int64_t>(a_code));
    return Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}
} // namespace cue::runtime
