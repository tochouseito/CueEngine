using System.Text;
using System.Text.Json;

namespace Cue.Hub;

/// <summary>署名済みパッケージ内の小さい Runtime Descriptor を検証する</summary>
public static class EngineDescriptor
{
    private static readonly HashSet<string> s_keys = new(StringComparer.Ordinal)
    { "formatVersion", "product", "version", "buildId", "platform", "architecture", "configuration", "sdkAbiVersion", "projectFormatMin", "projectFormatMax", "editor", "projectTool", "useLeaseId" };

    public static EngineRegistration load(string a_root)
    {
        var root = SafePaths.full(a_root);
        SafePaths.regular_file(Path.Combine(root, "CMakeLists.txt"));
        SafePaths.regular_file(Path.Combine(root, "CMakePresets.json"));
        var file = SafePaths.regular_file(Path.Combine(root, "engine.cueengine"));
        if (new FileInfo(file).Length > 65536) throw new InvalidDataException("Engine Manifest が大きすぎます");
        var text = new UTF8Encoding(false, true).GetString(File.ReadAllBytes(file));
        var fields = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in text.TrimStart('\uFEFF').Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            var equals = line.IndexOf('=');
            if (equals <= 0 || !s_keys.Contains(line[..equals].Trim()) || !fields.TryAdd(line[..equals].Trim(), line[(equals + 1)..].Trim()))
                throw new InvalidDataException("不正な Engine Manifest 項目です");
        }
        if (fields.Count != s_keys.Count || fields.Values.Any(value => value.Length == 0 || value.Any(char.IsControl)) ||
            fields["formatVersion"] != "1" || fields["product"] != "engine" || fields["platform"] != "Windows" || fields["architecture"] != "x64" ||
            fields["configuration"] != "Development" || fields["sdkAbiVersion"] != "1" || fields["projectFormatMin"] != "1" || fields["projectFormatMax"] != "2" ||
            !SafePaths.is_version(fields["version"]) || fields["buildId"].Length > 256 || fields["useLeaseId"] != $"engine-{fields["version"]}-windows-x64" ||
            fields["editor"] != "Bin/Editor/CueEditorHost.exe" || fields["projectTool"] != "Bin/Tools/CueProjectTool.exe")
            throw new InvalidDataException("未対応の Engine Manifest です");
        var editor = SafePaths.regular_file(SafePaths.child(root, fields["editor"]));
        var tool = SafePaths.regular_file(SafePaths.child(root, fields["projectTool"]));
        if (Path.GetFileName(editor) != "CueEditorHost.exe" || Path.GetFileName(tool) != "CueProjectTool.exe")
            throw new InvalidDataException("Engine 実行ファイルの種類が違います");
        return new EngineRegistration { Root = root, Version = fields["version"], BuildId = fields["buildId"], Editor = editor, ProjectTool = tool, LeaseId = fields["useLeaseId"] };
    }
}

/// <summary>Hub 外で起動した Editor とも共通 File の共有・排他で同期する</summary>
public static class EngineLease
{
    public static FileStream acquire(string a_leaseId, bool a_exclusive, string? a_directory = null)
    {
        if (a_leaseId.Length > 160 || a_leaseId.Length == 0 || a_leaseId.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '.' or '-' or '_' or '+')))
            throw new InvalidDataException("Engine Lease ID が不正です");
        var directory = a_directory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CueEngine", "Locks");
        SafePaths.no_links(directory);
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, a_leaseId + ".lock");
        SafePaths.no_links(path);
        return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, a_exclusive ? FileShare.None : FileShare.ReadWrite);
    }
}

public sealed class EngineRegistryService(HubStateStore a_store)
{
    private readonly HubStateStore m_store = a_store;

    public EngineRegistration register_local(string a_root)
    {
        var root = SafePaths.full(a_root);
        SafePaths.no_links(root);
        EngineRegistration engine;
        if (File.Exists(Path.Combine(root, "engine.cueengine"))) engine = EngineDescriptor.load(root);
        else
        {
            SafePaths.regular_file(Path.Combine(root, "CMakeLists.txt"));
            var versionFile = SafePaths.regular_file(Path.Combine(root, "Engine", "Version.json"));
            if (new FileInfo(versionFile).Length > 65536) throw new InvalidDataException("Version 定義が大きすぎます");
            using var version = JsonDocument.Parse(File.ReadAllBytes(versionFile));
            var number = version.RootElement.GetProperty("version").GetString() ?? "";
            if (!SafePaths.is_version(number)) throw new InvalidDataException("開発版の Version が不正です");
            var configurations = new[] { "Development", "Debug", "Release" };
            var outputs = configurations.Select(config => Path.Combine(root, "out", "build", "windows-vs2026", "bin", config));
            var output = outputs.FirstOrDefault(path => File.Exists(Path.Combine(path, "CueEditorHost.exe")) && File.Exists(Path.Combine(path, "CueProjectTool.exe")))
                ?? throw new FileNotFoundException("Editor と ProjectTool を先にビルドしてください");
            engine = new EngineRegistration { Root = root, Version = number, BuildId = "local-development", Editor = SafePaths.regular_file(Path.Combine(output, "CueEditorHost.exe")), ProjectTool = SafePaths.regular_file(Path.Combine(output, "CueProjectTool.exe")) };
        }
        // ローカル登録は配布版 Descriptor があっても削除権限を取得しない
        engine = engine with { IsManaged = false, ReceiptId = "" };
        m_store.update(state =>
        {
            if (state.Engines.Any(item => SafePaths.same(item.Root, root))) throw new InvalidOperationException("この Engine は登録済みです");
            state.Engines.Add(engine);
        });
        return engine;
    }

    public void unregister_local(EngineRegistration a_engine)
    {
        if (a_engine.IsManaged) throw new InvalidOperationException("導入版はアンインストールから操作してください");
        m_store.update(state => state.Engines.RemoveAll(item => item.Id == a_engine.Id && !item.IsManaged));
    }
}
