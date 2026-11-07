#include <Editor/SceneDocument.h>

#include <utility>

namespace cue
{
SceneDocument::SceneDocument(ConstructionKey, Scene a_scene, std::string a_text, std::optional<Path> a_file,
                             bool a_hasSavedBaseline)
    : m_scene(std::move(a_scene)), m_text(std::move(a_text)), m_file(std::move(a_file)),
      m_savedText(a_hasSavedBaseline ? m_text : std::string{}), m_hasSavedBaseline(a_hasSavedBaseline)
{
    m_history.reserve(k_historyLimit);
}

const Scene &SceneDocument::scene() const noexcept
{
    return m_scene;
}
const Path *SceneDocument::file() const noexcept
{
    return m_file ? &*m_file : nullptr;
}
std::string_view SceneDocument::selected_object_id() const noexcept
{
    return m_selected;
}
bool SceneDocument::is_dirty() const noexcept
{
    return !m_hasSavedBaseline || m_text != m_savedText;
}
bool SceneDocument::can_undo() const noexcept
{
    return !m_edit && m_cursor != 0;
}
bool SceneDocument::can_redo() const noexcept
{
    return !m_edit && m_cursor < m_history.size();
}
bool SceneDocument::is_transform_editing() const noexcept
{
    return m_edit.has_value();
}
std::uint64_t SceneDocument::revision() const noexcept
{
    return m_revision;
}

void SceneDocument::append_command(Command a_command) noexcept
{
    m_history.erase(m_history.begin() + m_cursor, m_history.end());
    if (m_history.size() == k_historyLimit)
        m_history.erase(m_history.begin());
    m_history.push_back(std::move(a_command));
    m_cursor = m_history.size();
}
} // namespace cue
