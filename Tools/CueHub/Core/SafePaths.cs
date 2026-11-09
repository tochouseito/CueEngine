using System.Text.RegularExpressions;

namespace Cue.Hub;

/// <summary>外部の配置指定を通常 File に閉じ込め、Reparse Point を辿らない</summary>
public static partial class SafePaths
{
    [GeneratedRegex(@"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$", RegexOptions.CultureInvariant)]
    private static partial Regex version_pattern();

    public static bool is_version(string a_version)
    {
        if (a_version.Length is 0 or > 128 || !version_pattern().IsMatch(a_version)) return false;
        var core = a_version.Split('+')[0];
        var dash = core.IndexOf('-');
        return dash < 0 || core[(dash + 1)..].Split('.').All(part => !part.All(char.IsAsciiDigit) || part.Length == 1 || part[0] != '0');
    }

    public static string full(string a_path)
    {
        if (!Path.IsPathFullyQualified(a_path)) throw new InvalidDataException("絶対パスを指定してください");
        var result = Path.GetFullPath(a_path);
        if (result.StartsWith(@"\\", StringComparison.Ordinal) || result.StartsWith(@"\\?\", StringComparison.Ordinal))
            throw new InvalidDataException("ローカルの通常パスが必要です");
        return Path.TrimEndingDirectorySeparator(result);
    }

    public static bool same(string a_left, string a_right) => StringComparer.OrdinalIgnoreCase.Equals(full(a_left), full(a_right));

    public static bool within(string a_root, string a_path)
    {
        var root = full(a_root);
        var path = full(a_path);
        return path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    }

    public static string child(string a_root, string a_relative)
    {
        validate_relative(a_relative);
        var result = Path.GetFullPath(Path.Combine(full(a_root), a_relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!within(a_root, result)) throw new InvalidDataException("配置がルート外を指しています");
        return result;
    }

    public static void validate_relative(string a_path)
    {
        if (string.IsNullOrWhiteSpace(a_path) || a_path.Length > 30000 || Path.IsPathRooted(a_path) || a_path.Contains(':') || a_path.Contains('\\') || a_path.Any(char.IsControl))
            throw new InvalidDataException("無効な相対パスです");
        foreach (var part in a_path.TrimEnd('/').Split('/'))
        {
            if (part.Length == 0 || part is "." or ".." || part.EndsWith(' ') || part.EndsWith('.') || part.IndexOfAny(['<', '>', '"', '|', '?', '*']) >= 0)
                throw new InvalidDataException("無効なパス要素です");
            var stem = part.Split('.')[0].ToUpperInvariant();
            if (stem is "CON" or "PRN" or "AUX" or "NUL" or "CLOCK$" || Regex.IsMatch(stem, @"^(COM|LPT)[1-9]$", RegexOptions.CultureInvariant))
                throw new InvalidDataException("予約されたパス名です");
        }
    }

    public static void no_links(string a_path)
    {
        var current = full(a_path);
        while (true)
        {
            // Exists は参照先のない Link を見落とすため、配置要素自体の属性を確認する
            try
            {
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException("リンクを含む配置は使用できません: " + current);
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
            var parent = Directory.GetParent(current)?.FullName;
            if (parent == null || StringComparer.OrdinalIgnoreCase.Equals(parent, current)) break;
            current = parent;
        }
    }

    public static string regular_file(string a_path)
    {
        var result = full(a_path);
        no_links(result);
        if (!File.Exists(result) || (File.GetAttributes(result) & FileAttributes.Directory) != 0)
            throw new FileNotFoundException("必要なファイルがありません", result);
        return result;
    }

    public static void delete_owned_tree(string a_path, string a_expectedParent)
    {
        var target = full(a_path);
        if (!same(Directory.GetParent(target)?.FullName ?? "", a_expectedParent))
            throw new InvalidDataException("所有する直下フォルダだけを回収できます");
        no_links(target);
        if (!Directory.Exists(target)) return;
        // 全要素を先に検証し、Link の発見時に途中まで削除しない
        var directories = new List<string> { target };
        var files = new List<string>();
        for (var index = 0; index < directories.Count; ++index)
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(directories[index]))
            {
                no_links(entry);
                if ((File.GetAttributes(entry) & FileAttributes.Directory) != 0) directories.Add(entry);
                else files.Add(entry);
            }
        }
        foreach (var file in files) { File.SetAttributes(file, FileAttributes.Normal); File.Delete(file); }
        for (var index = directories.Count - 1; index >= 0; --index) Directory.Delete(directories[index], false);
    }
}
