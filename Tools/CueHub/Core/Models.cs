using System.Text.Json.Serialization;

namespace Cue.Hub;

/// <summary>表示と永続化の所有値。File / Process Handle を保持しない</summary>
public sealed record EngineRegistration
{
    public string Id { get; init; } = Guid.NewGuid().ToString("D");
    public string Root { get; init; } = "";
    public string Version { get; init; } = "";
    public string BuildId { get; init; } = "";
    public string Editor { get; init; } = "";
    public string ProjectTool { get; init; } = "";
    public string LeaseId { get; init; } = "";
    public bool IsManaged { get; init; }
    public string ReceiptId { get; init; } = "";
    [JsonIgnore] public string Display => Version;
}

public sealed record ProjectRegistration
{
    public string ProjectFile { get; init; } = "";
    public string ProjectId { get; init; } = "";
    public string Name { get; init; } = "";
    public string EngineVersion { get; init; } = "";
    public DateTimeOffset LastOpenedUtc { get; init; }
    [JsonIgnore] public string Display => $"{Name}  [{(EngineVersion.Length == 0 ? "旧形式・Engine未指定" : EngineVersion)}]";
}

public sealed record HubState
{
    public int SchemaVersion { get; init; } = 1;
    public string EngineDirectory { get; set; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CueEngine", "Engines");
    public List<EngineRegistration> Engines { get; init; } = [];
    public List<ProjectRegistration> Projects { get; init; } = [];
    public bool IncludePreview { get; set; } = true;
    public string TrustedPublisherKeyFile { get; set; } = Path.Combine(AppContext.BaseDirectory, "PublisherPublicKey.pem");
    public string LocalHubFeed { get; set; } = "";
}

/// <summary>GitHub から取得した公開 Asset の所有値。任意の API Token を保持しない</summary>
public sealed record PublishedRelease
{
    public string Version { get; init; } = "";
    public string Tag { get; init; } = "";
    public bool IsPreview { get; init; }
    public DateTimeOffset PublishedUtc { get; init; }
    public string PageUrl { get; init; } = "";
    /// <summary>Asset 名と自 Repository の Asset API URL の対応。Browser 向け URL を取得に使わない</summary>
    public Dictionary<string, string> Assets { get; init; } = new(StringComparer.Ordinal);
    [JsonIgnore] public string Display => $"{Version}  {(IsPreview ? "Preview" : "正式版")}  {PublishedUtc.LocalDateTime:yyyy-MM-dd}";
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record ReleasePackageManifest
{
    public int SchemaVersion { get; init; }
    public string Product { get; init; } = "";
    public string Version { get; init; } = "";
    public string Tag { get; init; } = "";
    public string Platform { get; init; } = "";
    public string Architecture { get; init; } = "";
    public string Configuration { get; init; } = "";
    public string BuildId { get; init; } = "";
    public string PackageFileName { get; init; } = "";
    public string PackageSha256 { get; init; } = "";
    public long PackageSize { get; init; }
    public long ExpandedSize { get; init; }
}

public sealed record InstallProgress(string Message, double? Percent = null);

public sealed record ProjectToolResult
{
    public int SchemaVersion { get; init; }
    public bool Success { get; init; }
    public string ProjectFile { get; init; } = "";
    public string ProjectId { get; init; } = "";
    public string Name { get; init; } = "";
    public string EngineVersion { get; init; } = "";
    public string Message { get; init; } = "";
}
