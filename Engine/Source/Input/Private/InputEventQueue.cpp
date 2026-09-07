#include <Cue/Input/InputEventQueue.h>

#include <limits>

namespace cue
{
bool InputEventQueue::push(InputEvent a_event) noexcept
{
    if (m_count == k_capacity)
    {
        if (m_overflowCount != std::numeric_limits<std::uint64_t>::max())
        {
            ++m_overflowCount;
        }

        m_readIndex = 0;
        m_events[0] = {InputEventType::DeviceReset};
        m_count = 1;

        if (a_event.type == InputEventType::FocusGained || a_event.type == InputEventType::FocusLost)
        {
            m_events[1] = a_event;
            m_count = 2;
            m_isFocused = a_event.type == InputEventType::FocusGained;
        }
        else
        {
            m_events[1] = {m_isFocused ? InputEventType::FocusGained : InputEventType::FocusLost};
            m_count = 2;
            if (a_event.type != InputEventType::DeviceReset)
            {
                m_events[2] = a_event;
                m_count = 3;
            }
        }
        return false;
    }

    const std::size_t writeIndex = (m_readIndex + m_count) % k_capacity;
    m_events[writeIndex] = a_event;
    ++m_count;
    if (a_event.type == InputEventType::FocusGained || a_event.type == InputEventType::FocusLost)
    {
        m_isFocused = a_event.type == InputEventType::FocusGained;
    }
    return true;
}

bool InputEventQueue::try_pop(InputEvent &a_event) noexcept
{
    if (m_count == 0)
    {
        return false;
    }

    a_event = m_events[m_readIndex];
    m_readIndex = (m_readIndex + 1) % k_capacity;
    --m_count;

    if (m_count == 0)
    {
        m_readIndex = 0;
    }
    return true;
}

std::size_t InputEventQueue::size() const noexcept
{
    return m_count;
}

std::uint64_t InputEventQueue::overflow_count() const noexcept
{
    return m_overflowCount;
}
} // namespace cue
