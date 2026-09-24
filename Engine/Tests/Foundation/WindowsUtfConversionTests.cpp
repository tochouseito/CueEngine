#include <Cue/Foundation/Windows/UtfConversion.h>

#include <string>

/// @brief UTF変換の往復、埋め込みNUL、不正Sequenceの拒否を確認する
int main()
{
    // 日本語と補助平面の文字を含む文字列が往復しても変わらないことを確認する
    const std::string utf8 = "Cue日😀";
    auto utf16Result = cue::utf8_to_utf16(utf8);
    if (!utf16Result.has_value() || *utf16Result.try_value() != L"Cue日😀")
    {
        return 1;
    }

    auto roundTrip = cue::utf16_to_utf8(*utf16Result.try_value());
    if (!roundTrip.has_value() || *roundTrip.try_value() != utf8)
    {
        return 2;
    }

    // 明示した入力長に含まれる NUL を途中で切り捨てない
    const std::string embeddedNul{"A\0B", 3};
    auto embeddedWide = cue::utf8_to_utf16(embeddedNul);
    if (!embeddedWide.has_value() || embeddedWide.try_value()->size() != 3 ||
        (*embeddedWide.try_value())[1] != L'\0')
    {
        return 3;
    }
    auto embeddedRoundTrip = cue::utf16_to_utf8(*embeddedWide.try_value());
    if (!embeddedRoundTrip.has_value() || *embeddedRoundTrip.try_value() != embeddedNul)
    {
        return 4;
    }

    // 空入力は両方向とも空の成功値になる
    auto emptyWide = cue::utf8_to_utf16("");
    auto emptyUtf8 = cue::utf16_to_utf8(L"");
    if (!emptyWide.has_value() || !emptyWide.try_value()->empty() ||
        !emptyUtf8.has_value() || !emptyUtf8.try_value()->empty())
    {
        return 5;
    }

    // 代替文字へ置換せず不正な Sequence を Error にする
    const std::string invalidUtf8{"\xC0\xAF", 2};
    auto invalidWide = cue::utf8_to_utf16(invalidUtf8);
    if (invalidWide.has_value() || invalidWide.try_value() || !invalidWide.try_error() ||
        invalidWide.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 6;
    }

    // 対応する Pair がない UTF-16 Surrogate も Error にする
    const std::wstring invalidUtf16(1, static_cast<wchar_t>(0xD800));
    auto invalidBytes = cue::utf16_to_utf8(invalidUtf16);
    if (invalidBytes.has_value() || invalidBytes.try_value() || !invalidBytes.try_error() ||
        invalidBytes.try_error()->category != cue::ErrorCategory::InvalidArgument)
    {
        return 7;
    }
    return 0;
}
