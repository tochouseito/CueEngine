using System.Text.Json;

namespace Cue.Hub;

/// <summary>ユーザー設定を排他取得して保存する。返却値は Handle を借用しない</summary>
public sealed class HubStateStore
{
    public static JsonSerializerOptions JsonOptions { get; } = new(JsonSerializerDefaults.Web) { WriteIndented = true };
    public string DirectoryPath { get; }
    private readonly string m_file;

    public HubStateStore(string? a_directory = null)
    {
        DirectoryPath = SafePaths.full(a_directory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CueEngine", "Hub"));
        SafePaths.no_links(DirectoryPath);
        Directory.CreateDirectory(DirectoryPath);
        m_file = Path.Combine(DirectoryPath, "state.json");
    }

    public HubState load()
    {
        using var held = acquire();
        return read_unlocked();
    }

    public HubState update(Action<HubState> a_change)
    {
        using var held = acquire();
        var state = read_unlocked();
        a_change(state);
        validate(state);
        atomic_write(m_file, JsonSerializer.SerializeToUtf8Bytes(state, JsonOptions));
        return state;
    }

    private FileStream acquire()
    {
        SafePaths.no_links(DirectoryPath);
        var lockFile = Path.Combine(DirectoryPath, "state.lock");
        SafePaths.no_links(lockFile);
        return new FileStream(lockFile, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
    }

    private HubState read_unlocked()
    {
        if (!File.Exists(m_file)) return new HubState();
        SafePaths.regular_file(m_file);
        if (new FileInfo(m_file).Length > 4 * 1024 * 1024) throw new InvalidDataException("Hub 設定が大きすぎます");
        var result = JsonSerializer.Deserialize<HubState>(File.ReadAllBytes(m_file), JsonOptions) ?? throw new InvalidDataException("Hub 設定が空です");
        validate(result);
        return result;
    }

    private static void validate(HubState a_state)
    {
        if (a_state.SchemaVersion != 1 || a_state.Engines == null || a_state.Projects == null || a_state.Engines.Count > 1024 || a_state.Projects.Count > 4096)
            throw new InvalidDataException("未対応の Hub 設定です");
        SafePaths.full(a_state.EngineDirectory);
        if (a_state.Engines.Select(item => item.Id).Distinct(StringComparer.Ordinal).Count() != a_state.Engines.Count)
            throw new InvalidDataException("Engine 登録 ID が重複しています");
    }

    public static void atomic_write(string a_file, ReadOnlySpan<byte> a_bytes)
    {
        SafePaths.no_links(a_file);
        var parent = Path.GetDirectoryName(a_file) ?? throw new InvalidDataException("保存先に親がありません");
        Directory.CreateDirectory(parent);
        var temporary = Path.Combine(parent, ".cue-save-" + Guid.NewGuid().ToString("N"));
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            { file.Write(a_bytes); file.Flush(true); }
            File.Move(temporary, a_file, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
