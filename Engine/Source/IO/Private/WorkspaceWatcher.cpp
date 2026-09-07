#include <Cue/IO/WorkspaceWatcher.h>

#include <utility>

namespace cue
{
bool WorkspaceWatchLimits::is_valid() const noexcept
{
    return maxQueuedEvents != 0U && maxQueuedNameBytes != 0U && maxBatchChanges != 0U && debounceMilliseconds != 0U &&
           maximumBatchDelayMilliseconds >= debounceMilliseconds;
}

WorkspaceChangeHint::WorkspaceChangeHint(WorkspaceChangeHintKind a_kind, RelativePath a_locator,
                                         std::optional<RelativePath> a_previousLocator) noexcept
    : kind(a_kind), locator(std::move(a_locator)), previousLocator(std::move(a_previousLocator))
{
}
} // namespace cue
