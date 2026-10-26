#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Foundation/Result.h>

namespace cue
{
/// @brief FileSystem に問い合わせず、正規化した UTF-8 Path を値として所有する
///
/// 空の既定値は未指定を表す。create は空、不正 UTF-8、NUL、Drive 相対 Path、Device Namespace を拒否する
/// 区切りは /。相対 .. は保持し、絶対 Root より上への移動は拒否する
/// Symlink の解決や Root 内への閉じ込めを保証しない。独立した値は複数 Thread で利用できる
/// Allocation 失敗は他の Foundation 値型と同様に例外として伝播する
class Path final
{
  public:
    /// @brief 保存先の未指定を表す空の値を作る
    Path() = default;

    /// @brief UTF-8 と Root を検証し、区切りと . / .. を字句的に正規化する
    [[nodiscard]] static Result<Path> create(std::string_view a_text)
    {
        // 空文字・NUL・不正な UTF-8 を正規化前に拒否する
        if (a_text.empty() || !is_valid_utf8(a_text))
        {
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "Path.create.text"});
        }
        // 呼出元の文字列を所有し、区切り文字を '/' に統一する
        std::string text(a_text);
        for (auto &character : text)
        {
            if (character == '\\')
            {
                character = '/';
            }
        }
        std::string root;
        std::size_t offset = 0;
        // UNC は Server と Share までを越えられない Root とする
        if (text.starts_with("//"))
        {
            const auto serverEnd = text.find('/', 2);
            const auto shareEnd = serverEnd == std::string::npos ? serverEnd : text.find('/', serverEnd + 1);
            const auto end = shareEnd == std::string::npos ? text.size() : shareEnd;
            if (serverEnd == std::string::npos || serverEnd == 2 || end == serverEnd + 1 ||
                text.substr(2, serverEnd - 2) == "?" || text.substr(2, serverEnd - 2) == "." ||
                text.substr(2, serverEnd - 2) == ".." || text.substr(serverEnd + 1, end - serverEnd - 1) == "." ||
                text.substr(serverEnd + 1, end - serverEnd - 1) == "..")
            {
                return Result<Path>::failure({ErrorCategory::InvalidArgument, "Path.create.unc"});
            }
            root = text.substr(0, end) + '/';
            offset = end;
        }
        // Drive 相対の曖昧な形式を拒否し、絶対 Drive Root だけを扱う
        else if (text.size() >= 2 && text[1] == ':')
        {
            if (!((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) || text.size() < 3 ||
                text[2] != '/')
            {
                return Result<Path>::failure({ErrorCategory::InvalidArgument, "Path.create.drive"});
            }
            root = text.substr(0, 3);
            offset = 3;
        }
        else if (text[0] == '/')
        {
            root = "/";
            offset = 1;
        }
        // Root より後ろの要素を分解し、親参照を順に解決する
        std::vector<std::string_view> parts;
        while (offset < text.size())
        {
            auto end = text.find('/', offset);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            const std::string_view part(text.data() + offset, end - offset);
            if (part == "..")
            {
                // 通常の直前要素があれば親参照で取り除く
                if (!parts.empty() && parts.back() != "..")
                {
                    parts.pop_back();
                }
                // 絶対 Path では Root の外へ出る親参照を拒否する
                else if (!root.empty())
                {
                    return Result<Path>::failure({ErrorCategory::InvalidArgument, "Path.create.above_root"});
                }
                else
                {
                    // 相対 Path の未解決の親参照はそのまま保持する
                    parts.push_back(part);
                }
            }
            // 空要素と現在 Directory の指定を取り除く
            else if (!part.empty() && part != ".")
            {
                parts.push_back(part);
            }
            offset = end + 1;
        }
        // 残った要素を Root に連結し、正規化した文字列を所有する
        std::string normalized = root;
        for (const auto part : parts)
        {
            if (!normalized.empty() && normalized.back() != '/')
            {
                normalized += '/';
            }
            normalized += part;
        }
        // 相対 Path の全要素が消えた場合は現在 Directory を表す
        if (normalized.empty())
        {
            normalized = ".";
        }
        return Result<Path>::success(Path(std::move(normalized), root.size()));
    }

    /// @brief 値の寿命まで正規化した UTF-8 文字列を借用する
    [[nodiscard]] const std::string &utf8() const noexcept
    {
        return m_text;
    }
    /// @brief 未指定の Path か返す
    [[nodiscard]] bool is_empty() const noexcept
    {
        return m_text.empty();
    }
    /// @brief Drive / UNC / POSIX Root を持つか返す
    [[nodiscard]] bool is_absolute() const noexcept
    {
        return m_rootSize != 0;
    }
    /// @brief Root はそのまま、相対単一要素は . として親を返す
    [[nodiscard]] Path parent() const
    {
        // 空 Path と Root 自体には、それ以上の親を作らない
        if (m_text.empty() || m_text.size() == m_rootSize)
        {
            return *this;
        }
        const auto last = m_text.rfind('/');
        if (last == std::string::npos)
        {
            return Path(".", 0);
        }
        return Path(m_text.substr(0, last < m_rootSize ? m_rootSize : last), m_rootSize);
    }
    /// @brief Root / 未指定は空、それ以外は最後の要素を返す
    [[nodiscard]] std::string filename() const
    {
        return m_text.size() == m_rootSize ? std::string{} : m_text.substr(m_text.rfind('/') + 1);
    }
    /// @brief 隠し File の先頭 Dot を除き、最後の Dot 以降を返す
    [[nodiscard]] std::string extension() const
    {
        const auto name = filename();
        // 先頭の '.' だけを拡張子の開始とは扱わない
        const auto dot = name.rfind('.');
        return dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
    }
    /// @brief 最後の拡張子を除いた File 名を返す
    [[nodiscard]] std::string stem() const
    {
        const auto name = filename();
        return name.substr(0, name.size() - extension().size());
    }
    /// @brief 絶対の右辺は置換し、相対の右辺は結合後に検証する
    [[nodiscard]] Result<Path> join(const Path &a_right) const
    {
        if (a_right.is_empty())
        {
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "Path.join.empty"});
        }
        // 右辺が絶対 Path なら置換し、相対なら連結後に再検証する
        return a_right.is_absolute() || is_empty() ? Result<Path>::success(a_right)
                                                   : create(m_text + '/' + a_right.m_text);
    }
    /// @brief 生文字列の右辺も検証してから結合する
    [[nodiscard]] Result<Path> join(std::string_view a_right) const
    {
        auto right = create(a_right);
        return right.has_value() ? join(*right.try_value()) : Result<Path>::failure(*right.try_error());
    }

  private:
    /// @brief 検証済み文字列と Root 長だけを内部で受け取る
    Path(std::string a_text, std::size_t a_rootSize) : m_text(std::move(a_text)), m_rootSize(a_rootSize)
    {
    }
    /// @brief Overlong、Surrogate、範囲外、NUL と途中終端を拒否する
    [[nodiscard]] static bool is_valid_utf8(std::string_view a_text) noexcept
    {
        for (std::size_t offset = 0; offset < a_text.size();)
        {
            const auto first = static_cast<unsigned char>(a_text[offset++]);
            // 埋込み NUL は OS API で文字列が途切れるため拒否する
            if (first == 0)
            {
                return false;
            }
            if (first < 0x80)
            {
                continue;
            }
            // 先頭 Byte から必要な継続 Byte 数を決める
            const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                                   : first >= 0xe0 && first <= 0xef ? 2
                                   : first >= 0xf0 && first <= 0xf4 ? 3
                                                                    : 0;
            if (count == 0 || count > a_text.size() - offset)
            {
                return false;
            }
            std::uint32_t code = first & (0x7f >> (count + 1));
            // 継続 Byte を検証しながら Unicode Code Point を復元する
            for (unsigned index = 0; index < count; ++index)
            {
                const auto next = static_cast<unsigned char>(a_text[offset++]);
                if ((next & 0xc0) != 0x80)
                {
                    return false;
                }
                code = (code << 6) | (next & 0x3f);
            }
            // 最短でない表現・代理符号・Unicode 範囲外を拒否する
            if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) || (count == 3 && code < 0x10000) ||
                code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            {
                return false;
            }
        }
        return true;
    }
    std::string m_text;
    std::size_t m_rootSize = 0;
};
} // namespace cue
