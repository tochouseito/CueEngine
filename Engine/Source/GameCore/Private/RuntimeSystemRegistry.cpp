#include <Cue/GameCore/RuntimeSystemRegistry.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/GameCore/Error.h>
#include <Cue/GameCore/RuntimeWorld.h>

#include <algorithm>
#include <exception>
#include <new>
#include <optional>
#include <utility>

namespace
{
/// @brief 下位System ErrorをGameCore境界へ再分類しSystem IDを診断へ追加する
[[nodiscard]] cue::Error reclassify_system_error(const cue::AssertContext &a_assertContext,
                                                 cue::game_core::GameCoreError a_code, std::string_view a_summary,
                                                 std::string_view a_systemId, cue::Error &&a_cause) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.GameCore", static_cast<std::int64_t>(a_code));
    cue::Error error =
        cue::Error::reclassify(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(a_cause));
    error.add_context(a_assertContext.fatal_handler(), a_systemId);
    return error;
}

/// @brief 最初のStop ErrorをPrimaryとして保持し後続Errorを順序付き診断へ追加する
void collect_stop_error(const cue::AssertContext &a_assertContext, std::optional<cue::Error> &a_primary,
                        cue::Error &&a_error) noexcept
{
    if (!a_primary.has_value())
    {
        a_primary.emplace(std::move(a_error));
        return;
    }

    a_primary->append_secondary_diagnostics(a_assertContext, a_error, "A later runtime system stop also failed",
                                            "Runtime system stop");
}
} // namespace

namespace cue::game_core
{
class RuntimeSystemRegistry::Entry final
{
  public:
    enum class State
    {
        Registered,
        Started,
        StopPending,
        Stopped
    };

    /// @brief 登録定義とSystem所有権を固定したEntryを構築する
    Entry(RuntimeSystemDescriptor &&a_descriptor, std::unique_ptr<RuntimeSystem> &&a_system,
          std::size_t a_registrationIndex) noexcept
        : descriptor(std::move(a_descriptor)), system(std::move(a_system)), registrationIndex(a_registrationIndex)
    {
    }

    RuntimeSystemDescriptor descriptor;
    std::unique_ptr<RuntimeSystem> system;
    std::size_t registrationIndex;
    State state = State::Registered;
};

RuntimeSystemRegistry::RuntimeSystemRegistry(const AssertContext &a_assertContext) noexcept
    : m_assertContext(&a_assertContext), m_ownerThread(std::this_thread::get_id())
{
}

RuntimeSystemRegistry::~RuntimeSystemRegistry() noexcept
{
    assert_owner_thread();
    const bool hasActiveSystems = count_active_systems() != 0U;
    CUE_ASSERT(*m_assertContext, !hasActiveSystems,
               "Cue.GameCore runtime system registry destruction requires all systems stopped");
    if (hasActiveSystems)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.GameCore runtime system registry destruction requires all systems stopped");
    }
}

Result<void> RuntimeSystemRegistry::register_system(RuntimeSystemDescriptor a_descriptor,
                                                    std::unique_ptr<RuntimeSystem> a_system) noexcept
{
    assert_owner_thread();
    if (m_state != RuntimeSystemRegistryState::Registering)
    {
        return Result<void>::failure(make_state_error("Runtime systems can only register before seal"));
    }
    if (a_descriptor.id.empty() || a_system == nullptr)
    {
        Error error = make_game_core_error(*m_assertContext, GameCoreError::InvalidSystemRegistration,
                                           "Runtime system registration requires a non-empty ID and owned system");
        if (!a_descriptor.id.empty())
        {
            error.add_context(m_assertContext->fatal_handler(), a_descriptor.id);
        }
        return Result<void>::failure(std::move(error));
    }

    for (const std::unique_ptr<Entry> &entry : m_entries)
    {
        if (entry->descriptor.id == a_descriptor.id)
        {
            Error error = make_game_core_error(*m_assertContext, GameCoreError::DuplicateSystem,
                                               "Runtime system ID is already registered");
            error.add_context(m_assertContext->fatal_handler(), a_descriptor.id);
            return Result<void>::failure(std::move(error));
        }
    }

    try
    {
        m_entries.push_back(std::make_unique<Entry>(std::move(a_descriptor), std::move(a_system), m_entries.size()));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }

    return Result<void>::success();
}

Result<void> RuntimeSystemRegistry::seal() noexcept
{
    assert_owner_thread();
    if (m_state != RuntimeSystemRegistryState::Registering)
    {
        return Result<void>::failure(make_state_error("Runtime system registry can only seal once"));
    }

    std::vector<std::size_t> candidateOrder;
    try
    {
        candidateOrder.reserve(m_entries.size());
        for (std::size_t index = 0; index < m_entries.size(); ++index)
        {
            candidateOrder.push_back(index);
        }

        std::stable_sort(candidateOrder.begin(), candidateOrder.end(),
                         [this](std::size_t a_left, std::size_t a_right) noexcept
                         {
                             const Entry &left = *m_entries[a_left];
                             const Entry &right = *m_entries[a_right];
                             if (left.descriptor.phase != right.descriptor.phase)
                             {
                                 return left.descriptor.phase < right.descriptor.phase;
                             }
                             if (left.descriptor.order != right.descriptor.order)
                             {
                                 return left.descriptor.order < right.descriptor.order;
                             }
                             return left.registrationIndex < right.registrationIndex;
                         });
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }

    for (std::size_t executionIndex = 0; executionIndex < candidateOrder.size(); ++executionIndex)
    {
        const Entry &entry = *m_entries[candidateOrder[executionIndex]];
        for (std::size_t dependencyIndex = 0; dependencyIndex < entry.descriptor.dependencies.size(); ++dependencyIndex)
        {
            const std::string &dependency = entry.descriptor.dependencies[dependencyIndex];
            if (dependency.empty())
            {
                Error error = make_game_core_error(*m_assertContext, GameCoreError::InvalidSystemDependency,
                                                   "Runtime system dependency ID must not be empty");
                error.add_context(m_assertContext->fatal_handler(), entry.descriptor.id);
                return Result<void>::failure(std::move(error));
            }

            for (std::size_t prior = 0; prior < dependencyIndex; ++prior)
            {
                if (entry.descriptor.dependencies[prior] == dependency)
                {
                    Error error = make_game_core_error(*m_assertContext, GameCoreError::InvalidSystemDependency,
                                                       "Runtime system dependency is duplicated");
                    error.add_context(m_assertContext->fatal_handler(), entry.descriptor.id);
                    error.add_context(m_assertContext->fatal_handler(), dependency);
                    return Result<void>::failure(std::move(error));
                }
            }

            std::optional<std::size_t> dependencyPosition;
            for (std::size_t candidateIndex = 0; candidateIndex < candidateOrder.size(); ++candidateIndex)
            {
                if (m_entries[candidateOrder[candidateIndex]]->descriptor.id == dependency)
                {
                    dependencyPosition = candidateIndex;
                    break;
                }
            }

            if (!dependencyPosition.has_value())
            {
                Error error = make_game_core_error(*m_assertContext, GameCoreError::MissingSystemDependency,
                                                   "Runtime system dependency is not registered");
                error.add_context(m_assertContext->fatal_handler(), entry.descriptor.id);
                error.add_context(m_assertContext->fatal_handler(), dependency);
                return Result<void>::failure(std::move(error));
            }
            if (dependencyPosition.value() >= executionIndex)
            {
                Error error = make_game_core_error(*m_assertContext, GameCoreError::InvalidSystemDependency,
                                                   "Runtime system dependency must precede the dependent system");
                error.add_context(m_assertContext->fatal_handler(), entry.descriptor.id);
                error.add_context(m_assertContext->fatal_handler(), dependency);
                return Result<void>::failure(std::move(error));
            }
        }
    }

    m_executionOrder.swap(candidateOrder);
    m_state = RuntimeSystemRegistryState::Sealed;
    return Result<void>::success();
}

Result<void> RuntimeSystemRegistry::start(RuntimeWorld &a_runtimeWorld) noexcept
{
    assert_owner_thread();
    if (m_isInvokingCallback)
    {
        return Result<void>::failure(
            make_state_error("Runtime system registry lifecycle API rejects callback reentry"));
    }
    if (m_state != RuntimeSystemRegistryState::Sealed)
    {
        return Result<void>::failure(make_state_error("Runtime system start requires a sealed registry"));
    }

    Result<void> validation = validate_runtime_world(a_runtimeWorld, true, nullptr);
    if (!validation)
    {
        return validation;
    }

    m_runtimeWorld = &a_runtimeWorld;
    RuntimeSystemContext context = {*a_runtimeWorld.try_world(), *a_runtimeWorld.try_command_buffer()};
    for (std::size_t entryIndex : m_executionOrder)
    {
        Entry &entry = *m_entries[entryIndex];
        m_isInvokingCallback = true;
        Result<void> result = entry.system->start(context);
        m_isInvokingCallback = false;
        if (!result)
        {
            Error primary = reclassify_system_error(*m_assertContext, GameCoreError::SystemStartFailed,
                                                    "Runtime system start failed", entry.descriptor.id,
                                                    std::move(*result.try_error()));
            Result<void> rollback = stop_started(context);
            if (!rollback)
            {
                primary.append_secondary_diagnostics(*m_assertContext, *rollback.try_error(),
                                                     "Runtime system start rollback was incomplete",
                                                     "Runtime system rollback");
            }
            return Result<void>::failure(std::move(primary));
        }
        entry.state = Entry::State::Started;
    }

    m_state = RuntimeSystemRegistryState::Started;
    return Result<void>::success();
}

Result<void> RuntimeSystemRegistry::update(RuntimeWorld &a_runtimeWorld, const UpdateContext &a_timing,
                                           const FrameInputSnapshot &a_input) noexcept
{
    assert_owner_thread();
    if (m_isInvokingCallback)
    {
        return Result<void>::failure(
            make_state_error("Runtime system registry lifecycle API rejects callback reentry"));
    }
    if (m_state != RuntimeSystemRegistryState::Started)
    {
        return Result<void>::failure(make_state_error("Runtime system update requires a started registry"));
    }

    Result<void> validation = validate_runtime_world(a_runtimeWorld, true, m_runtimeWorld);
    if (!validation)
    {
        return validation;
    }

    RuntimeSystemUpdateContext context = {*a_runtimeWorld.try_world(), *a_runtimeWorld.try_command_buffer(), a_timing,
                                          a_input};
    for (std::size_t entryIndex : m_executionOrder)
    {
        Entry &entry = *m_entries[entryIndex];
        m_isInvokingCallback = true;
        Result<void> result = entry.system->update(context);
        m_isInvokingCallback = false;
        if (!result)
        {
            Error error = reclassify_system_error(*m_assertContext, GameCoreError::SystemUpdateFailed,
                                                  "Runtime system update failed", entry.descriptor.id,
                                                  std::move(*result.try_error()));
            return Result<void>::failure(std::move(error));
        }
    }

    return Result<void>::success();
}

Result<void> RuntimeSystemRegistry::stop(RuntimeWorld &a_runtimeWorld) noexcept
{
    assert_owner_thread();
    if (m_isInvokingCallback)
    {
        return Result<void>::failure(
            make_state_error("Runtime system registry lifecycle API rejects callback reentry"));
    }
    if (m_state == RuntimeSystemRegistryState::Stopped)
    {
        return Result<void>::success();
    }
    if (m_state == RuntimeSystemRegistryState::Sealed)
    {
        m_state = RuntimeSystemRegistryState::Stopped;
        return Result<void>::success();
    }
    if (m_state != RuntimeSystemRegistryState::Started && m_state != RuntimeSystemRegistryState::StopPending)
    {
        return Result<void>::failure(make_state_error("Runtime system stop requires a sealed or started registry"));
    }

    Result<void> validation = validate_runtime_world(a_runtimeWorld, false, m_runtimeWorld);
    if (!validation)
    {
        return validation;
    }

    RuntimeSystemContext context = {*a_runtimeWorld.try_world(), *a_runtimeWorld.try_command_buffer()};
    return stop_started(context);
}

RuntimeSystemRegistryState RuntimeSystemRegistry::state() const noexcept
{
    assert_owner_thread();
    return m_state;
}

std::size_t RuntimeSystemRegistry::system_count() const noexcept
{
    assert_owner_thread();
    return m_entries.size();
}

std::size_t RuntimeSystemRegistry::active_system_count() const noexcept
{
    assert_owner_thread();
    return count_active_systems();
}

void RuntimeSystemRegistry::assert_owner_thread() const noexcept
{
    const bool isOwner = std::this_thread::get_id() == m_ownerThread;
    CUE_ASSERT(*m_assertContext, isOwner, "Cue.GameCore runtime system registry API requires its owner thread");
    if (!isOwner)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.GameCore runtime system registry API requires its owner thread");
    }
}

Result<void> RuntimeSystemRegistry::validate_runtime_world(RuntimeWorld &a_runtimeWorld, bool a_requiresRunning,
                                                           const RuntimeWorld *a_expectedRuntimeWorld) const noexcept
{
    if (a_expectedRuntimeWorld != nullptr && a_expectedRuntimeWorld != &a_runtimeWorld)
    {
        return Result<void>::failure(
            make_state_error("Runtime system registry requires the RuntimeWorld used for start"));
    }

    const RuntimeWorldState worldState = a_runtimeWorld.state();
    const bool isAllowed =
        a_requiresRunning ? worldState == RuntimeWorldState::Running
                          : (worldState == RuntimeWorldState::Running || worldState == RuntimeWorldState::Stopping);
    if (!isAllowed || a_runtimeWorld.try_world() == nullptr || a_runtimeWorld.try_command_buffer() == nullptr)
    {
        return Result<void>::failure(make_state_error("Runtime system registry requires an operational runtime world"));
    }
    return Result<void>::success();
}

Result<void> RuntimeSystemRegistry::stop_started(RuntimeSystemContext &a_context) noexcept
{
    std::optional<Error> primaryError;
    for (auto iterator = m_executionOrder.rbegin(); iterator != m_executionOrder.rend(); ++iterator)
    {
        Entry &entry = *m_entries[*iterator];
        if (entry.state != Entry::State::Started && entry.state != Entry::State::StopPending)
        {
            continue;
        }
        if (has_live_dependent(entry.descriptor.id))
        {
            continue;
        }

        entry.state = Entry::State::StopPending;
        m_isInvokingCallback = true;
        Result<void> result = entry.system->stop(a_context);
        m_isInvokingCallback = false;
        if (result)
        {
            entry.state = Entry::State::Stopped;
            continue;
        }

        Error error =
            reclassify_system_error(*m_assertContext, GameCoreError::SystemStopFailed, "Runtime system stop failed",
                                    entry.descriptor.id, std::move(*result.try_error()));
        collect_stop_error(*m_assertContext, primaryError, std::move(error));
    }

    if (count_active_systems() == 0U)
    {
        m_state = RuntimeSystemRegistryState::Stopped;
        m_runtimeWorld = nullptr;
        return Result<void>::success();
    }

    m_state = RuntimeSystemRegistryState::StopPending;
    if (primaryError.has_value())
    {
        return Result<void>::failure(std::move(primaryError.value()));
    }

    return Result<void>::failure(make_state_error("Runtime system stop could not make dependency-safe progress"));
}

bool RuntimeSystemRegistry::has_live_dependent(std::string_view a_systemId) const noexcept
{
    for (const std::unique_ptr<Entry> &entry : m_entries)
    {
        if (entry->state != Entry::State::Started && entry->state != Entry::State::StopPending)
        {
            continue;
        }
        for (const std::string &dependency : entry->descriptor.dependencies)
        {
            if (dependency == a_systemId)
            {
                return true;
            }
        }
    }
    return false;
}

std::size_t RuntimeSystemRegistry::count_active_systems() const noexcept
{
    std::size_t count = 0;
    for (const std::unique_ptr<Entry> &entry : m_entries)
    {
        if (entry->state == Entry::State::Started || entry->state == Entry::State::StopPending)
        {
            ++count;
        }
    }
    return count;
}

Error RuntimeSystemRegistry::make_state_error(std::string_view a_summary) const noexcept
{
    return make_game_core_error(*m_assertContext, GameCoreError::InvalidSystemRegistryState, a_summary);
}

[[noreturn]] void RuntimeSystemRegistry::terminate_allocation() const noexcept
{
    m_assertContext->fatal_handler().terminate("Cue.GameCore runtime system registry allocation failed");
}

[[noreturn]] void RuntimeSystemRegistry::terminate_exception() const noexcept
{
    m_assertContext->fatal_handler().terminate("Cue.GameCore runtime system registry caught an unexpected exception");
}
} // namespace cue::game_core
