#pragma once

namespace cue
{
/// @brief Project に依存しない Editor の表示言語
enum class EditorLanguage
{
    English,
    Japanese,
};

/// @brief 選択言語の静的な表示文字列を返す
template <class Char>
[[nodiscard]] constexpr const Char *editor_text(EditorLanguage a_language, const Char *a_english,
                                                const Char *a_japanese) noexcept
{
    return a_language == EditorLanguage::Japanese ? a_japanese : a_english;
}
} // namespace cue
