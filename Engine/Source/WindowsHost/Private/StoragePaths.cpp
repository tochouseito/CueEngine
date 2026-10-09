#include <WindowsHost/StoragePaths.h>

#include <array>
#include <new>
#include <string_view>

namespace cue
{
namespace
{
/// @brief 製品名が Directory 境界や Windows の特殊名を変えないか確認する
bool is_valid_name(const std::string &a_name)
{
    if (a_name.empty() || a_name == "." || a_name == ".." || a_name.back() == '.' || a_name.back() == ' ' ||
        a_name.find_first_of("/\\:<>\"|?*") != std::string::npos)
    {
        return false;
    }
    auto path = Path::create(a_name);
    if (!path.has_value())
    {
        return false;
    }
    for (const auto character : a_name)
    {
        if (static_cast<unsigned char>(character) < 32)
        {
            return false;
        }
    }
    auto base = a_name.substr(0, a_name.find('.'));
    for (auto &character : base)
    {
        if (character >= 'a' && character <= 'z')
        {
            character -= 'a' - 'A';
        }
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$")
    {
        return false;
    }
    const auto suffix = base.size() >= 3 ? std::string_view(base).substr(3) : std::string_view{};
    return !((base.starts_with("COM") || base.starts_with("LPT")) &&
             ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') || suffix == "¹" || suffix == "²" ||
              suffix == "³"));
}
/// @brief Windows で CWD に依存しない Drive / UNC の絶対保存先か確認する
bool is_absolute_storage(const Path &a_path) noexcept
{
    const auto &text = a_path.utf8();
    return a_path.is_absolute() && (text.starts_with("//") || (text.size() >= 3 && text[1] == ':'));
}
} // namespace

Result<StoragePaths> resolve_storage_paths(IFileSystem &a_files, const StoragePathsConfig &a_config)
{
    using pathsResult = Result<StoragePaths>;
    try
    {
        if (!is_valid_name(a_config.companyName) || !is_valid_name(a_config.applicationName))
        {
            return pathsResult::failure({ErrorCategory::InvalidArgument, "StoragePaths.application_identity"});
        }
        const std::array overrides{&a_config.dataRootOverride, &a_config.logsOverride, &a_config.shaderCacheOverride,
                                   &a_config.psoCacheOverride};
        for (const auto *path : overrides)
        {
            if (!path->is_empty() && !is_absolute_storage(*path))
            {
                return pathsResult::failure({ErrorCategory::InvalidArgument, "StoragePaths.override.absolute"});
            }
        }
        if (a_config.mode != StorageMode::Development && a_config.mode != StorageMode::Product &&
            a_config.mode != StorageMode::Portable)
        {
            return pathsResult::failure({ErrorCategory::InvalidArgument, "StoragePaths.mode"});
        }
        Path root;
        if (!a_config.dataRootOverride.is_empty())
        {
            root = a_config.dataRootOverride;
        }
        else if (a_config.mode == StorageMode::Development)
        {
            if (!is_absolute_storage(a_config.repositoryRoot))
            {
                return pathsResult::failure({ErrorCategory::InvalidArgument, "StoragePaths.repository_root"});
            }
            auto joined = a_config.repositoryRoot.join("out");
            if (!joined.has_value())
            {
                return pathsResult::failure(*joined.try_error());
            }
            root = joined.take_value();
        }
        else
        {
            auto directory =
                a_config.mode == StorageMode::Product ? a_files.local_data_directory() : a_files.executable_directory();
            if (!directory.has_value())
            {
                return pathsResult::failure(*directory.try_error());
            }
            root = directory.take_value();
            if (a_config.mode == StorageMode::Product)
            {
                auto product = root.join(a_config.companyName + '/' + a_config.applicationName);
                if (!product.has_value())
                {
                    return pathsResult::failure(*product.try_error());
                }
                root = product.take_value();
            }
        }
        if (!is_absolute_storage(root))
        {
            return pathsResult::failure({ErrorCategory::InvalidArgument, "StoragePaths.root.absolute"});
        }
        auto logs = a_config.logsOverride.is_empty() ? root.join("logs") : Result<Path>::success(a_config.logsOverride);
        auto shader = a_config.shaderCacheOverride.is_empty() ? root.join("cache/shaders")
                                                              : Result<Path>::success(a_config.shaderCacheOverride);
        auto pso = a_config.psoCacheOverride.is_empty() ? root.join("cache/pso")
                                                        : Result<Path>::success(a_config.psoCacheOverride);
        if (!logs.has_value() || !shader.has_value() || !pso.has_value())
        {
            return pathsResult::failure(!logs.has_value()     ? *logs.try_error()
                                        : !shader.has_value() ? *shader.try_error()
                                                              : *pso.try_error());
        }
        return pathsResult::success({std::move(root), logs.take_value(), shader.take_value(), pso.take_value()});
    }
    catch (const std::bad_alloc &)
    {
        return pathsResult::failure({ErrorCategory::PlatformFailure, "StoragePaths.allocation"});
    }
}

Result<void> create_storage_directories(IFileSystem &a_files, const StoragePaths &a_paths)
{
    for (const auto *path : {&a_paths.dataRoot, &a_paths.logs, &a_paths.shaderCache, &a_paths.psoCache})
    {
        if (!is_absolute_storage(*path))
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "StoragePaths.create.absolute"});
        }
    }
    for (const auto *path : {&a_paths.dataRoot, &a_paths.logs, &a_paths.shaderCache, &a_paths.psoCache})
    {
        auto created = a_files.create_directories(*path);
        if (!created.has_value())
        {
            return created;
        }
    }
    return Result<void>::success();
}
} // namespace cue
