#include <Cue/Foundation/Result.h>

#include <memory>
#include <utility>

/// @brief Move-only成功値と診断値が同時に公開されないことを確認する
int main()
{
    // 成功時は Move-only Value を一意に保持する
    auto ownedValue = std::make_unique<int>(42);
    auto success = cue::Result<std::unique_ptr<int>>::success(std::move(ownedValue));
    if (ownedValue || !success.has_value() || !success.try_value() || !*success.try_value() ||
        **success.try_value() != 42 || success.try_error())
    {
        return 1;
    }

    // take_value 後も Result は成功側のままになる
    auto extractedValue = success.take_value();
    if (!extractedValue || *extractedValue != 42 || !success.has_value() || *success.try_value())
    {
        return 4;
    }

    // 失敗時は Value を公開せず診断情報を保持する
    auto failure = cue::Result<std::unique_ptr<int>>::failure(
        {cue::ErrorCategory::PlatformFailure, "CreateWindowExW", 1407});
    if (failure.has_value() || failure.try_value() || !failure.try_error() ||
        failure.try_error()->category != cue::ErrorCategory::PlatformFailure ||
        failure.try_error()->operation != "CreateWindowExW" || failure.try_error()->nativeCode != 1407)
    {
        return 2;
    }

    // void 版も成功と失敗を排他的に表す
    auto emptySuccess = cue::Result<void>::success();
    auto emptyFailure = cue::Result<void>::failure({cue::ErrorCategory::InvalidArgument, "WindowDescriptor", 0});
    if (!emptySuccess.has_value() || emptySuccess.try_error() || emptyFailure.has_value() ||
        !emptyFailure.try_error())
    {
        return 3;
    }

    return 0;
}
