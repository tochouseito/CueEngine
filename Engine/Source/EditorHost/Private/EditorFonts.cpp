#include "EditorFonts.h"

#include <cstdio>
#include <utility>

#include <imgui.h>

#include <Platform/FileSystem.h>

namespace cue
{
namespace
{
constexpr const char *k_fontNames[] = {"Inter-VariableFont_opsz,wght.ttf", "NotoSansJP-VariableFont_wght.ttf",
                                     "JetBrainsMono-VariableFont_wght.ttf",
                                     "MaterialSymbolsOutlined-VariableFont_FILL,GRAD,opsz,wght.ttf"};
constexpr ImWchar k_iconRanges[] = {0xe000, 0xf8ff, 0};

/// @brief 所有権を Atlas へ渡さず、固定された Font Data から一つの Source を登録する
ImFont *add_font(ImGuiIO &a_io, std::vector<std::byte> &a_data, float a_size, const char *a_name,
                 ImFontConfig a_config, const ImWchar *a_ranges)
{
    a_config.FontDataOwnedByAtlas = false;
    std::snprintf(a_config.Name, sizeof(a_config.Name), "%s", a_name);
    return a_io.Fonts->AddFontFromMemoryTTF(a_data.data(), static_cast<int>(a_data.size()), a_size, &a_config, a_ranges);
}
} // namespace

/// @brief Unicode Path の読込みを FileSystem に委ね、ImGui の stdio に依存しない
Result<EditorFonts> read_editor_fonts(IFileSystem &a_files, const std::string &a_directory)
{
    Path directory;
    if (a_directory.empty())
    {
        auto executable = a_files.executable_directory();
        if (!executable.has_value())
            return Result<EditorFonts>::failure(*executable.try_error());
        auto joined = executable.try_value()->join("EngineResources/Fonts");
        if (!joined.has_value())
            return Result<EditorFonts>::failure(*joined.try_error());
        directory = joined.take_value();
    }
    else
    {
        auto specified = Path::create(a_directory);
        if (!specified.has_value())
            return Result<EditorFonts>::failure(*specified.try_error());
        directory = specified.take_value();
    }
    EditorFonts fonts;
    for (std::size_t index = 0; index < fonts.data.size(); ++index)
    {
        auto path = directory.join(k_fontNames[index]);
        if (!path.has_value())
            return Result<EditorFonts>::failure(*path.try_error());
        auto data = a_files.read_all(*path.try_value(), 32 * 1024 * 1024);
        if (!data.has_value())
            return Result<EditorFonts>::failure(*data.try_error());
        if (data.try_value()->size() < 100)
            return Result<EditorFonts>::failure({ErrorCategory::InvalidArgument, "EditorFonts.data"});
        fonts.data[index] = data.take_value();
    }
    return Result<EditorFonts>::success(std::move(fonts));
}

/// @brief Inter と JetBrains Mono へ日本語を合成し、UI の私用領域は Icon Source に限定する
Result<void> initialize_editor_fonts(ImGuiIO &a_io, EditorFonts &a_fonts, float a_uiSize, float a_codeSize)
{
    ImFontConfig ui;
    ui.GlyphExcludeRanges = k_iconRanges;
    auto *uiFont = add_font(a_io, a_fonts.data[0], a_uiSize, "Inter", ui, a_io.Fonts->GetGlyphRangesDefault());
    if (!uiFont)
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorFonts.Inter"});

    ImFontConfig japanese;
    japanese.MergeMode = true;
    japanese.OversampleH = 2;
    japanese.OversampleV = 2;
    japanese.RasterizerMultiply = 2.5f;
    japanese.GlyphExcludeRanges = k_iconRanges;
    auto *uiJapanese = add_font(a_io, a_fonts.data[1], a_uiSize, "Noto Sans JP UI", japanese,
                               a_io.Fonts->GetGlyphRangesJapanese());
    if (!uiJapanese)
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorFonts.NotoSansJP.UI"});

    ImFontConfig icons;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = a_uiSize;
    auto *iconFont = add_font(a_io, a_fonts.data[3], a_uiSize, "Material Symbols Outlined", icons, k_iconRanges);
    if (!iconFont)
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorFonts.MaterialSymbols"});

    auto *codeFont = add_font(a_io, a_fonts.data[2], a_codeSize, "JetBrains Mono", {}, a_io.Fonts->GetGlyphRangesDefault());
    if (!codeFont)
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorFonts.JetBrainsMono"});
    japanese.GlyphExcludeRanges = nullptr;
    auto *codeJapanese = add_font(a_io, a_fonts.data[1], a_codeSize, "Noto Sans JP Code", japanese,
                                 a_io.Fonts->GetGlyphRangesJapanese());
    if (!codeJapanese)
        return Result<void>::failure({ErrorCategory::PlatformFailure, "EditorFonts.NotoSansJP.Code"});
    a_io.FontDefault = uiFont;
    return Result<void>::success();
}
} // namespace cue
