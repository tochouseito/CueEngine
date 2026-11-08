using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Velopack;
using Velopack.Locators;

namespace Cue.Hub;

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record HubReleaseManifest
{
    public int SchemaVersion { get; init; }
    public string Product { get; init; } = "";
    public string Version { get; init; } = "";
    public string Tag { get; init; } = "";
    public string PackageFileName { get; init; } = "";
    public string PackageSha256 { get; init; } = "";
    public long PackageSize { get; init; }
    public string FeedFileName { get; init; } = "";
    public string FeedSha256 { get; init; } = "";
    public long FeedSize { get; init; }
}

/// <summary>検証した Feed と Velopack の要求を所有する。再起動前に準備を完了する</summary>
public sealed record HubUpdateOffer(UpdateManager Manager, IVelopackLocator Locator, UpdateInfo Info, HubReleaseManifest Manifest);

/// <summary>Engine Release を除外し、署名した Hub 用 Feed だけを Updater へ渡す</summary>
public sealed class HubUpdateService(HttpClient a_http, HubStateStore a_store, PackageVerifier a_verifier, IVelopackLocator? a_locator = null)
{
    private readonly GitHubReleaseClient m_github = new(a_http);
    private readonly HubStateStore m_store = a_store;
    private readonly PackageVerifier m_verifier = a_verifier;
    private readonly IVelopackLocator? m_locator = a_locator;

    public async Task<HubUpdateOffer?> prepare_github_async(bool a_includePreview, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        var releases = new List<PublishedRelease>();
        var completed = false;
        for (var page = 1; page <= 100; ++page)
        {
            using var response = await m_github.get_releases_async(page, a_cancel);
            using var document = JsonDocument.Parse(await EngineCatalogService.read_bounded_async(response.Content, 4 * 1024 * 1024, a_cancel));
            foreach (var item in document.RootElement.EnumerateArray())
            {
                var release = EngineCatalogService.parse_release(item, "hub-v", false);
                if (release != null && (a_includePreview || !release.IsPreview) && release.Assets.ContainsKey("hub-release.json") && release.Assets.ContainsKey("hub-release.json.sig")) releases.Add(release);
            }
            if (document.RootElement.GetArrayLength() < 100) { completed = true; break; }
        }
        if (!completed) throw new InvalidDataException("Hub Release 一覧の上限を超えました");
        var selected = releases.OrderByDescending(item => item.PublishedUtc).FirstOrDefault();
        if (selected == null) return null;
        var bytes = await download_async(selected.Assets["hub-release.json"], 65536, a_cancel);
        var signature = Encoding.ASCII.GetString(await download_async(selected.Assets["hub-release.json.sig"], 4096, a_cancel));
        var manifest = read_manifest(bytes, signature);
        if (manifest.Tag != selected.Tag || manifest.Version != selected.Version || !selected.Assets.ContainsKey(manifest.PackageFileName))
            throw new InvalidDataException("Hub Release の内容が一致しません");
        var feed = await download_async(selected.Assets[manifest.FeedFileName], 1024 * 1024, a_cancel);
        verify_hash(feed, manifest.FeedSha256, manifest.FeedSize);
        var cache = Path.Combine(m_store.DirectoryPath, "HubUpdates", manifest.Version);
        SafePaths.no_links(cache);
        Directory.CreateDirectory(cache);
        HubStateStore.atomic_write(Path.Combine(cache, manifest.FeedFileName), feed);
        a_progress?.Report(new("Hub 更新パッケージを取得しています"));
        // Full Package だけを扱い、署名対象外 Delta を自動取得させない
        var package = await download_async(selected.Assets[manifest.PackageFileName], checked((int)Math.Min(manifest.PackageSize, int.MaxValue)), a_cancel);
        verify_hash(package, manifest.PackageSha256, manifest.PackageSize);
        HubStateStore.atomic_write(Path.Combine(cache, manifest.PackageFileName), package);
        return await prepare_verified_feed_async(cache, manifest, a_progress, a_cancel);
    }

    public async Task<HubUpdateOffer?> prepare_local_async(string a_directory, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        var root = SafePaths.full(a_directory);
        var file = SafePaths.regular_file(Path.Combine(root, "hub-release.json"));
        if (new FileInfo(file).Length > 65536) throw new InvalidDataException("Hub Manifest が大きすぎます");
        var signatureFile = SafePaths.regular_file(Path.Combine(root, "hub-release.json.sig"));
        if (new FileInfo(signatureFile).Length > 4096) throw new InvalidDataException("署名が大きすぎます");
        var manifest = read_manifest(File.ReadAllBytes(file), File.ReadAllText(signatureFile));
        return await prepare_verified_feed_async(root, manifest, a_progress, a_cancel);
    }

    private async Task<HubUpdateOffer?> prepare_verified_feed_async(string a_root, HubReleaseManifest a_manifest, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        await verify_file_async(SafePaths.child(a_root, a_manifest.FeedFileName), a_manifest.FeedSha256, a_manifest.FeedSize, a_cancel);
        await verify_file_async(SafePaths.child(a_root, a_manifest.PackageFileName), a_manifest.PackageSha256, a_manifest.PackageSize, a_cancel);
        // Feed は一つの署名済み Full Package だけを指すことを要求する
        using var feed = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(a_root, a_manifest.FeedFileName)));
        var assets = feed.RootElement.GetProperty("Assets");
        if (assets.GetArrayLength() != 1 || assets[0].GetProperty("FileName").GetString() != a_manifest.PackageFileName || assets[0].GetProperty("Type").GetString() != "Full" || assets[0].GetProperty("PackageId").GetString() != "CueEngine.Hub")
            throw new InvalidDataException("Hub Feed に署名対象外の更新があります");
        var locator = m_locator ?? VelopackLocator.Current;
        var manager = new UpdateManager(a_root, new UpdateOptions { ExplicitChannel = "win" }, locator);
        if (!manager.IsInstalled || manager.AppId != "CueEngine.Hub") throw new InvalidOperationException("Hub インストーラーで導入したアプリから更新してください");
        var info = await manager.CheckForUpdatesAsync();
        a_cancel.ThrowIfCancellationRequested();
        if (info == null) return null;
        if (info.TargetFullRelease.FileName != a_manifest.PackageFileName || info.TargetFullRelease.Version.ToString() != a_manifest.Version)
            throw new InvalidDataException("Hub 更新候補が Manifest と違います");
        await manager.DownloadUpdatesAsync(info, value => a_progress?.Report(new("Hub 更新を準備しています", value)), a_cancel);
        await verify_file_async(package_path(locator, info.TargetFullRelease), a_manifest.PackageSha256, a_manifest.PackageSize, a_cancel);
        return new HubUpdateOffer(manager, locator, info, a_manifest);
    }

    public async Task apply_and_restart_async(HubUpdateOffer a_offer, CancellationToken a_cancel)
    {
        var file = package_path(a_offer.Locator, a_offer.Info.TargetFullRelease);
        await verify_file_async(file, a_offer.Manifest.PackageSha256, a_offer.Manifest.PackageSize, a_cancel);
        a_offer.Manager.ApplyUpdatesAndRestart(a_offer.Info.TargetFullRelease);
    }

    private HubReleaseManifest read_manifest(byte[] a_bytes, string a_signature)
    {
        m_verifier.verify_signature(a_bytes, a_signature);
        using var document = JsonDocument.Parse(a_bytes);
        if (document.RootElement.EnumerateObject().Select(item => item.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != document.RootElement.EnumerateObject().Count()) throw new InvalidDataException("Hub Manifest が重複しています");
        var value = JsonSerializer.Deserialize<HubReleaseManifest>(a_bytes, HubStateStore.JsonOptions) ?? throw new InvalidDataException("Hub Manifest が空です");
        if (new[] { value.Product, value.Version, value.Tag, value.PackageFileName, value.PackageSha256, value.FeedFileName, value.FeedSha256 }.Any(string.IsNullOrWhiteSpace))
            throw new InvalidDataException("Hub Manifest の必須文字列がありません");
        if (value.SchemaVersion != 1 || value.Product != "hub" || !SafePaths.is_version(value.Version) || value.Tag != "hub-v" + value.Version ||
            value.PackageFileName != $"CueEngine.Hub-{value.Version}-full.nupkg" || value.FeedFileName != "releases.win.json" ||
            value.PackageSize is <= 0 or > 1024L * 1024 * 1024 || value.FeedSize is <= 0 or > 1024 * 1024 ||
            !is_hash(value.PackageSha256) || !is_hash(value.FeedSha256)) throw new InvalidDataException("Hub Manifest が不正です");
        SafePaths.validate_relative(value.PackageFileName);
        return value;
    }

    private static bool is_hash(string a_hash) => a_hash.Length == 64 && a_hash.All(char.IsAsciiHexDigit);
    private static string package_path(IVelopackLocator a_locator, VelopackAsset a_asset) => SafePaths.child(a_locator.PackagesDir ?? throw new InvalidOperationException("Hub の更新先がありません"), a_asset.FileName);
    private static void verify_hash(byte[] a_bytes, string a_hash, long a_size)
    {
        if (a_bytes.LongLength != a_size || !Convert.ToHexString(SHA256.HashData(a_bytes)).Equals(a_hash, StringComparison.OrdinalIgnoreCase)) throw new CryptographicException("Hub 配布物の Hash が一致しません");
    }

    private static async Task verify_file_async(string a_file, string a_hash, long a_size, CancellationToken a_cancel)
    {
        await using var file = new FileStream(SafePaths.regular_file(a_file), FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length != a_size || !Convert.ToHexString(await SHA256.HashDataAsync(file, a_cancel)).Equals(a_hash, StringComparison.OrdinalIgnoreCase)) throw new CryptographicException("Hub 更新パッケージの検証に失敗しました");
    }

    private async Task<byte[]> download_async(string a_url, int a_limit, CancellationToken a_cancel)
    {
        using var response = await m_github.get_asset_async(a_url, a_cancel);
        return await EngineCatalogService.read_bounded_async(response.Content, a_limit, a_cancel);
    }
}
