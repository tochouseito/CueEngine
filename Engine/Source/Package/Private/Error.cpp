#include <Cue/Package/Error.h>

#include <Cue/Foundation/Assert.h>

#include <utility>

namespace cue::package
{
Error make_package_error(const AssertContext &a_assertContext, PackageError a_error,
                         std::string_view a_summary) noexcept
{
    ErrorCode code =
        ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Package", static_cast<std::int64_t>(a_error));
    return Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}
} // namespace cue::package
