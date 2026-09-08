#include <Cue/Editor/ImGui/SessionLog.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <mutex>
#include <utility>

namespace
{
constexpr std::size_t k_maximumEntries = 2048U;

/// @brief ASCII文字だけをLocale非依存の小文字へ変換する
[[nodiscard]] char ascii_lower(char a_value) noexcept
{
    const unsigned char value = static_cast<unsigned char>(a_value);
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z'))
    {
        return static_cast<char>(value + static_cast<unsigned char>('a' - 'A'));
    }
    return static_cast<char>(value);
}

/// @brief 二つのASCII文字をLocale非依存でCase-insensitive比較する
[[nodiscard]] bool ascii_equal(char a_left, char a_right) noexcept
{
    return ascii_lower(a_left) == ascii_lower(a_right);
}

/// @brief UTF-8 Byte列を壊さずASCII部分だけCase-insensitive比較する
[[nodiscard]] bool contains_filter(std::string_view a_text, std::string_view a_filter) noexcept
{
    if (a_filter.empty())
    {
        return true;
    }
    return std::search(a_text.begin(), a_text.end(), a_filter.begin(), a_filter.end(), ascii_equal) != a_text.end();
}

/// @brief Log購読競合をEditor ImGui Domainの回復可能Errorへ変換する
[[nodiscard]] cue::Error make_subscription_error(const cue::AssertContext &a_assertContext) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Editor.ImGui", 1);
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code),
                              "Editor Session Log Router already has an active subscription");
}
} // namespace

namespace cue::editor
{
class EditorSessionLogBuffer final
{
  public:
    /// @brief Log RecordをSession-localなPointer非保持値へ変換して上限内へ追加する
    [[nodiscard]] bool append(const LogRecord &a_record) noexcept
    {
        try
        {
            EditorSessionLogEntry entry{a_record.level(), std::string(a_record.message())};
            if (const Error *error = a_record.try_error(); error != nullptr)
            {
                entry.message.append(" | ");
                entry.message.append(error->summary());
                entry.message.append(" [");
                entry.message.append(error->root_code().domain());
                entry.message.push_back(':');
                char value[32]{};
                const auto converted = std::to_chars(value, value + sizeof(value), error->root_code().value());
                if (converted.ec == std::errc{})
                {
                    entry.message.append(value, converted.ptr);
                }
                entry.message.push_back(']');
            }

            std::scoped_lock lock(m_mutex);
            entry.sessionGeneration = m_sessionGeneration;
            if (m_entries.size() == k_maximumEntries)
            {
                m_entries.erase(m_entries.begin());
            }
            m_entries.push_back(std::move(entry));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    /// @brief Filterに一致する現在値をUI Thread用Snapshotへ複製する
    [[nodiscard]] std::vector<EditorSessionLogEntry> snapshot(std::string_view a_filter) const
    {
        std::scoped_lock lock(m_mutex);
        std::vector<EditorSessionLogEntry> result;
        result.reserve(m_entries.size());
        for (const EditorSessionLogEntry &entry : m_entries)
        {
            if (contains_filter(entry.message, a_filter))
            {
                result.push_back(entry);
            }
        }
        return result;
    }

    /// @brief Session-local Log値を空にする
    void clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_entries.clear();
    }

    /// @brief 以後に追加するLogへ付与するStable Session Generationを更新する
    void set_session_generation(std::uint64_t a_generation) noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_sessionGeneration = a_generation;
        for (EditorSessionLogEntry &entry : m_entries)
        {
            if (entry.sessionGeneration == 0U)
            {
                entry.sessionGeneration = a_generation;
            }
        }
    }

  private:
    mutable std::mutex m_mutex;
    std::vector<EditorSessionLogEntry> m_entries;
    std::uint64_t m_sessionGeneration = 0U;
};

class EditorSessionLogRouter::Impl final
{
  public:
    mutable std::mutex mutex;
    std::weak_ptr<EditorSessionLogBuffer> buffer;
    std::uint64_t generation = 0U;
};

EditorSessionLogSubscription::EditorSessionLogSubscription(ConstructionKey, EditorSessionLogRouter &a_router,
                                                           std::uint64_t a_generation,
                                                           std::shared_ptr<EditorSessionLogBuffer> a_buffer) noexcept
    : m_router(&a_router), m_buffer(std::move(a_buffer)), m_generation(a_generation)
{
}

EditorSessionLogSubscription::~EditorSessionLogSubscription() noexcept
{
    m_router->unsubscribe(m_generation);
    m_buffer.reset();
}

std::vector<EditorSessionLogEntry> EditorSessionLogSubscription::snapshot(std::string_view a_filter) const
{
    return m_buffer->snapshot(a_filter);
}

void EditorSessionLogSubscription::set_session_generation(std::uint64_t a_generation) noexcept
{
    m_buffer->set_session_generation(a_generation);
}

void EditorSessionLogSubscription::clear() noexcept
{
    m_buffer->clear();
}

EditorSessionLogRouter::EditorSessionLogRouter() noexcept : m_impl(std::make_unique<Impl>())
{
}

EditorSessionLogRouter::~EditorSessionLogRouter()
{
    if (has_active_subscription())
    {
        std::terminate();
    }
}

Result<std::unique_ptr<EditorSessionLogSubscription>> EditorSessionLogRouter::subscribe(
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        if (!m_impl->buffer.expired())
        {
            return Result<std::unique_ptr<EditorSessionLogSubscription>>::failure(
                make_subscription_error(a_assertContext));
        }
        std::shared_ptr<EditorSessionLogBuffer> buffer = std::make_shared<EditorSessionLogBuffer>();
        ++m_impl->generation;
        if (m_impl->generation == 0U)
        {
            m_impl->generation = 1U;
        }
        std::unique_ptr<EditorSessionLogSubscription> subscription = std::make_unique<EditorSessionLogSubscription>(
            EditorSessionLogSubscription::ConstructionKey{}, *this, m_impl->generation, buffer);
        m_impl->buffer = std::move(buffer);
        return Result<std::unique_ptr<EditorSessionLogSubscription>>::success(std::move(subscription));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Editor Session Log subscription allocation failed");
    }
    std::terminate();
}

bool EditorSessionLogRouter::has_active_subscription() const noexcept
{
    std::scoped_lock lock(m_impl->mutex);
    return !m_impl->buffer.expired();
}

bool EditorSessionLogRouter::write(const LogRecord &a_record) noexcept
{
    std::scoped_lock lock(m_impl->mutex);
    std::shared_ptr<EditorSessionLogBuffer> buffer = m_impl->buffer.lock();
    return buffer == nullptr || buffer->append(a_record);
}

bool EditorSessionLogRouter::flush() noexcept
{
    return true;
}

void EditorSessionLogRouter::unsubscribe(std::uint64_t a_generation) noexcept
{
    std::scoped_lock lock(m_impl->mutex);
    if (m_impl->generation == a_generation)
    {
        m_impl->buffer.reset();
    }
}
} // namespace cue::editor
