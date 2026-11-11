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
    public string Theme { get; set; } = "light";
    public string TrustedPublisherKeyFile { get; set; } = Path.Combine(AppContext.BaseDirectory, "PublisherPublicKey.pem");
    public string LocalHubFeed { get; set; } = "";
}

/// <summary>署名付き配布一覧の所有値。認証情報を保持しない</summary>
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record PublishedRelease
{
    public string Version { get; init; } = "";
    public string Tag { get; init; } = "";
    public bool IsPreview { get; init; }
    public DateTimeOffset PublishedUtc { get; init; }
    public string PageUrl { get; init; } = "";
    /// <summary>Asset 名と自 Repository の通常 HTTPS ダウンロード URL の対応</summary>
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
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<ReleasePackagePart>? PackageParts { get; init; }
    [JsonIgnore] public IReadOnlyList<ReleasePackagePart> Parts => PackageParts ??
        [new() { FileName = PackageFileName, Sha256 = PackageSha256, Size = PackageSize }];
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record ReleasePackagePart
{
    public string FileName { get; init; } = "";
    public string Sha256 { get; init; } = "";
    public long Size { get; init; }
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record DistributionCatalog
{
    public int SchemaVersion { get; init; }
    public DateTimeOffset GeneratedAt { get; init; }
    public List<PublishedRelease> Engines { get; init; } = [];
    public List<PublishedRelease> Hubs { get; init; } = [];
}

/// <summary>受信・展開時の上限。巨大な入力による無制限確保を防ぐ</summary>
public static class DistributionLimits
{
    public const int CatalogBytes = 4 * 1024 * 1024;
    public const int ManifestBytes = 256 * 1024;
    public const int SignatureBytes = 4096;
    public const long PackageBytes = 1L << 40;
    public const long ExpandedBytes = 1L << 40;
    public const long PartBytes = (2L << 30) - 1;
    public const int Parts = 512;
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
