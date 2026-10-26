#include <Platform/FileSystem.h>

#include <array>
#include <new>

namespace cue
{
Result<bool> IFileSystem::exists(const Path &a_path)
{
    // 未存在と問い合わせ失敗を区別したまま真偽値へ変換する
    auto info = stat(a_path);
    return info.has_value() ? Result<bool>::success(info.try_value()->has_value())
                            : Result<bool>::failure(*info.try_error());
}

Result<std::vector<std::byte>> IFileSystem::read_all(const Path &a_path, std::size_t a_maxBytes)
{
    using bytesResult = Result<std::vector<std::byte>>;
    try
    {
        // 読み込む File を所有し、途中失敗でも Handle を回収する
        auto opened = open(a_path);
        if (!opened.has_value())
        {
            return bytesResult::failure(*opened.try_error());
        }
        auto file = opened.take_value();
        auto size = file->size();
        if (!size.has_value())
        {
            return bytesResult::failure(*size.try_error());
        }
        std::vector<std::byte> bytes;
        // File サイズが読込み上限と配列の上限を超えないか確認する
        if (*size.try_value() > a_maxBytes || *size.try_value() > bytes.max_size())
        {
            return bytesResult::failure({ErrorCategory::InvalidArgument, "FileSystem.read_all.limit"});
        }
        bytes.resize(static_cast<std::size_t>(*size.try_value()));
        std::size_t offset = 0;
        // 部分読込みに対応し、要求した Byte 数まで繰り返す
        while (offset < bytes.size())
        {
            auto read = file->read(std::span(bytes).subspan(offset));
            if (!read.has_value())
            {
                return bytesResult::failure(*read.try_error());
            }
            // 途中の短縮や読取りの進捗停止を File の変更として拒否する
            if (*read.try_value() == 0 || *read.try_value() > bytes.size() - offset)
            {
                return bytesResult::failure({ErrorCategory::PlatformFailure, "FileSystem.read_all.changed"});
            }
            offset += *read.try_value();
        }
        // 末尾の追加とサイズ変更を確認し、読込み中の変更を検出する
        std::array<std::byte, 1> extra;
        auto end = file->read(extra);
        auto finalSize = file->size();
        if (!end.has_value() || !finalSize.has_value())
        {
            return bytesResult::failure(!end.has_value() ? *end.try_error() : *finalSize.try_error());
        }
        if (*end.try_value() != 0 || *finalSize.try_value() != bytes.size())
        {
            return bytesResult::failure({ErrorCategory::PlatformFailure, "FileSystem.read_all.changed"});
        }
        // Close の成功まで確認してから読込み結果を公開する
        auto closed = file->close();
        return closed.has_value() ? bytesResult::success(std::move(bytes)) : bytesResult::failure(*closed.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return bytesResult::failure({ErrorCategory::PlatformFailure, "FileSystem.read_all.allocation"});
    }
}

Result<void> IFileSystem::write_all(const Path &a_path, std::span<const std::byte> a_data, bool a_createParents)
{
    // 要求された場合だけ親 Directory を作る
    if (a_createParents)
    {
        auto created = create_directories(a_path.parent());
        if (!created.has_value())
        {
            return created;
        }
    }
    // 書込み先を切り詰めて開き、今回の内容で置き換える
    auto opened = open(a_path, {FileAccess::Write, FileCreation::CreateAlways});
    if (!opened.has_value())
    {
        return Result<void>::failure(*opened.try_error());
    }
    auto file = opened.take_value();
    std::size_t offset = 0;
    // 短い書込みが返っても未処理部分を続けて書く
    while (offset < a_data.size())
    {
        auto written = file->write(a_data.subspan(offset));
        if (!written.has_value())
        {
            return Result<void>::failure(*written.try_error());
        }
        if (*written.try_value() == 0 || *written.try_value() > a_data.size() - offset)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "FileSystem.write_all.progress"});
        }
        offset += *written.try_value();
    }
    // 書込み後の Flush と Close の結果も呼出側へ返す
    auto flushed = file->flush();
    auto closed = file->close();
    return !flushed.has_value() ? std::move(flushed) : std::move(closed);
}
} // namespace cue
