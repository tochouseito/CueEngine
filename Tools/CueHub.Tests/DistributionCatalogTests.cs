using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Cue.Hub;

namespace Cue.Hub.Tests;

internal static partial class Program
{
    /// <summary>静的一覧と通常 HTTPS Asset を Engine / Hub の既存検証へ接続する</summary>
    private static async Task test_distribution_downloads(string a_root)
    {
        await test_engine_distribution_download(a_root);
        await test_multipart_distribution_download(a_root);
        await test_hub_distribution_download(a_root);
    }

    private static async Task test_signed_distribution_catalog(string a_root)
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        using var otherKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var verifier = new PackageVerifier(key.ExportSubjectPublicKeyInfoPem());
        var generated = DateTimeOffset.UtcNow.AddMinutes(-1);
        var engines = Enumerable.Range(0, 10).Select(index => distribution_release(index == 9 ? "0.2.0-preview.1" : $"0.1.{index}", true,
            generated.AddDays(-10 + index))).ToList();
        var catalog = new DistributionCatalog { SchemaVersion = 1, GeneratedAt = generated, Engines = engines,
            Hubs = [distribution_release("0.3.0-preview.1", false, generated.AddDays(-1))] };
        var store = new HubStateStore(Path.Combine(a_root, "SignedCatalog"));
        store.update(state => state.IncludePreview = false);
        var legacy = Path.Combine(store.DirectoryPath, "engine-catalog-v2.json");
        File.WriteAllText(legacy, "[]");
        using var handler = new SignedDistributionFixtureHandler(key, catalog);
        using var http = new HttpClient(handler);
        var service = new EngineCatalogService(http, store, verifier);
        var list = await service.refresh_async(CancellationToken.None);
        check(list.Count == 10 && list[0].Version == "0.2.0-preview.1" && list[0].IsPreview, "signed latest ten engines preserve publish ordering and preview classification");
        check(list.All(item => item.Assets.All(asset => DistributionClient.is_asset_url(asset.Value, item.Tag, asset.Key))), "signed catalog contains canonical normal HTTPS assets");
        check(handler.CatalogCalls == 2 && !handler.HasApiRequest, "catalog uses public JSON and detached signature only");
        await service.refresh_async(CancellationToken.None);
        check(handler.CatalogCalls == 2, "verified catalog TTL avoids extra HTTP");
        await service.refresh_async(CancellationToken.None, true);
        check(handler.CatalogCalls == 4, "forced catalog refresh bypasses TTL");
        check(File.ReadAllText(legacy) == "[]" && !store.load().IncludePreview, "legacy API cache and Hub settings preserved");
        var direct = new DistributionCatalogService(http, store, verifier);
        check((await direct.load_async(CancellationToken.None)).Hubs.Count == 1, "shared catalog separates Hub and Engine releases");
        handler.IsOffline = true;
        var cached = await direct.load_async(CancellationToken.None, true);
        check(cached.GeneratedAt == generated && direct.LastWarning != null, "offline forced refresh warns and returns verified cache");
        using (var cancel = new CancellationTokenSource())
        {
            cancel.Cancel();
            await rejected(() => direct.load_async(cancel.Token, true), "catalog cancellation propagates instead of cache success");
        }
        handler.IsOffline = false;
        handler.Catalog = catalog with { GeneratedAt = generated.AddDays(-1) };
        check((await direct.load_async(CancellationToken.None, true)).GeneratedAt == generated && direct.LastWarning != null,
            "signed rollback cannot replace newer verified cache");
        handler.Catalog = catalog;
        var cacheFile = Path.Combine(store.DirectoryPath, "distribution-catalog-v1.cache.json");
        var original = File.ReadAllBytes(cacheFile);
        foreach (var field in new[] { "payload", "signature" })
        {
            var cache = JsonNode.Parse(original)!.AsObject();
            cache[field] = field == "payload" ? Convert.ToBase64String(Encoding.UTF8.GetBytes("{}")) : sign(otherKey, JsonSerializer.SerializeToUtf8Bytes(catalog, HubStateStore.JsonOptions));
            File.WriteAllText(cacheFile, cache.ToJsonString());
            handler.IsOffline = true;
            await rejected(() => new DistributionCatalogService(http, store, verifier).load_async(CancellationToken.None, true), "modified persisted catalog " + field + " rejected offline");
            File.WriteAllBytes(cacheFile, original);
            handler.IsOffline = false;
        }
        var stale = JsonNode.Parse(original)!.AsObject();
        stale["fetchedUtc"] = DateTimeOffset.UtcNow.AddHours(-1);
        File.WriteAllText(cacheFile, stale.ToJsonString());
        var calls = handler.CatalogCalls;
        await new DistributionCatalogService(http, store, verifier).load_async(CancellationToken.None);
        check(handler.CatalogCalls == calls + 2, "expired catalog TTL automatically refreshes");
        var raw = JsonSerializer.SerializeToUtf8Bytes(catalog, HubStateStore.JsonOptions);
        var mutations = new List<byte[]>();
        var invalidUrl = JsonNode.Parse(raw)!.AsObject();
        invalidUrl["engines"]![0]!["assets"]!["engine-release.json"] = "https://api.github.com/repos/tochouseito/CueEngine/releases/assets/1";
        mutations.Add(Encoding.UTF8.GetBytes(invalidUrl.ToJsonString()));
        var channel = JsonNode.Parse(raw)!.AsObject();
        channel["engines"]![0]!["isPreview"] = true;
        mutations.Add(Encoding.UTF8.GetBytes(channel.ToJsonString()));
        var schema = JsonNode.Parse(raw)!.AsObject();
        schema["schemaVersion"] = 2;
        mutations.Add(Encoding.UTF8.GetBytes(schema.ToJsonString()));
        var unknown = JsonNode.Parse(raw)!.AsObject();
        unknown["draft"] = true;
        mutations.Add(Encoding.UTF8.GetBytes(unknown.ToJsonString()));
        mutations.Add(Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(raw).Insert(1, "\"SchemaVersion\":1,")));
        foreach (var mutation in mutations)
        {
            using var invalidHandler = new SignedDistributionFixtureHandler(key, catalog) { RawCatalog = mutation };
            using var invalidHttp = new HttpClient(invalidHandler);
            await rejected(() => new DistributionCatalogService(invalidHttp, new HubStateStore(Path.Combine(a_root, "BadCatalog-" + Guid.NewGuid().ToString("N"))), verifier)
                .load_async(CancellationToken.None), "invalid signed schema / channel / URL / duplicate rejected");
        }
        using (var badSignature = new SignedDistributionFixtureHandler(otherKey, catalog))
        using (var invalidHttp = new HttpClient(badSignature))
            await rejected(() => new DistributionCatalogService(invalidHttp, new HubStateStore(Path.Combine(a_root, "WrongPublisherCatalog")), verifier)
                .load_async(CancellationToken.None), "wrong catalog publisher rejected");
        check(!handler.HasApiRequest && http.DefaultRequestHeaders.Accept.Count == 0 && http.DefaultRequestHeaders.UserAgent.Count == 0,
            "normal catalog leaves shared HttpClient headers unchanged");
    }

    private static async Task test_engine_distribution_download(string a_root)
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var package = make_package(a_root, "0.3.0-preview.1");
        var manifest = make_manifest(package, "0.3.0-preview.1");
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        var files = new Dictionary<string, byte[]>
        {
            [distribution_asset(manifest.Tag, manifest.PackageFileName)] = File.ReadAllBytes(package),
            [distribution_asset(manifest.Tag, "engine-release.json")] = bytes,
            [distribution_asset(manifest.Tag, "engine-release.json.sig")] = Encoding.ASCII.GetBytes(sign(key, bytes))
        };
        var release = distribution_release(manifest.Version, true, DateTimeOffset.UtcNow.AddHours(-1));
        var catalog = new DistributionCatalog { SchemaVersion = 1, GeneratedAt = DateTimeOffset.UtcNow, Engines = [release] };
        using var handler = new SignedDistributionFixtureHandler(key, catalog, files);
        using var http = new HttpClient(handler);
        var store = new HubStateStore(Path.Combine(a_root, "HttpsInstallSettings"));
        store.update(state => state.EngineDirectory = Path.Combine(a_root, "HttpsEngines"));
        var verifier = new PackageVerifier(key.ExportSubjectPublicKeyInfoPem());
        var releases = await new EngineCatalogService(http, store, verifier).refresh_async(CancellationToken.None);
        var installer = new EngineInstallService(store, http, verifier, Path.Combine(a_root, "HttpsLocks"));
        var installed = await installer.install_release_async(releases.Single(), null, CancellationToken.None);
        check(handler.CatalogCalls == 2 && handler.AssetCalls == 3 && File.Exists(installed.Editor) && !handler.HasApiRequest,
            "Engine installed from signed static catalog and normal HTTPS");
        installer.uninstall(installed);
        var invalid = release with { Assets = new() { ["engine-release.json"] = "https://evil.example/manifest" } };
        var calls = handler.AssetCalls;
        await rejected(() => installer.install_release_async(invalid, null, CancellationToken.None), "untrusted manifest URL rejected before request");
        check(handler.AssetCalls == calls, "untrusted release URL never requested");
        files[distribution_asset(manifest.Tag, manifest.PackageFileName)][^1] ^= 0xff;
        var corrupt = new HubStateStore(Path.Combine(a_root, "CorruptHttpsInstallSettings"));
        corrupt.update(state => state.EngineDirectory = Path.Combine(a_root, "CorruptHttpsEngines"));
        await rejected(() => new EngineInstallService(corrupt, http, verifier, Path.Combine(a_root, "CorruptHttpsLocks"))
            .install_release_async(release, null, CancellationToken.None), "online HTTPS package hash mismatch");
        check(corrupt.load().Engines.Count == 0 && (!Directory.Exists(corrupt.load().EngineDirectory) ||
            !Directory.EnumerateDirectories(corrupt.load().EngineDirectory).Any()), "bad online package leaves no staging or registration");
    }

    /// <summary>小さい Part で v2 の通常 HTTPS 取得、結合展開、再導入 Cache、欠損拒否を検証する</summary>
    private static async Task test_multipart_distribution_download(string a_root)
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var package = make_package(a_root, "0.4.0-preview.1");
        var original = make_manifest(package, "0.4.0-preview.1");
        var archive = File.ReadAllBytes(package);
        var parts = new List<ReleasePackagePart>();
        var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        for (var offset = 0; offset < archive.Length; offset += 127)
        {
            var bytes = archive[offset..Math.Min(offset + 127, archive.Length)];
            var name = original.PackageFileName + "." + (parts.Count + 1).ToString("D3", System.Globalization.CultureInfo.InvariantCulture);
            parts.Add(new ReleasePackagePart { FileName = name, Sha256 = transfer_hash(bytes), Size = bytes.Length });
            files[distribution_asset(original.Tag, name)] = bytes;
        }
        var manifest = original with { SchemaVersion = 2, PackageParts = parts };
        var manifestBytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        files[distribution_asset(manifest.Tag, "engine-release.json")] = manifestBytes;
        files[distribution_asset(manifest.Tag, "engine-release.json.sig")] = Encoding.ASCII.GetBytes(sign(key, manifestBytes));
        var release = distribution_release(manifest.Version, true, DateTimeOffset.UtcNow.AddHours(-1)) with
        {
            Assets = files.Keys.ToDictionary(url => Uri.UnescapeDataString(url[(url.LastIndexOf('/') + 1)..]), url => url, StringComparer.Ordinal)
        };
        var catalog = new DistributionCatalog { SchemaVersion = 1, GeneratedAt = DateTimeOffset.UtcNow, Engines = [release] };
        using var handler = new SignedDistributionFixtureHandler(key, catalog, files);
        using var http = new HttpClient(handler);
        var store = new HubStateStore(Path.Combine(a_root, "MultipartHttpsSettings"));
        store.update(state => state.EngineDirectory = Path.Combine(a_root, "MultipartHttpsEngines"));
        var verifier = new PackageVerifier(key.ExportSubjectPublicKeyInfoPem());
        var published = (await new EngineCatalogService(http, store, verifier).refresh_async(CancellationToken.None)).Single();
        var installer = new EngineInstallService(store, http, verifier, Path.Combine(a_root, "MultipartHttpsLocks"));
        var installed = await installer.install_release_async(published, null, CancellationToken.None);
        check(parts.Count > 1 && handler.AssetCalls == parts.Count + 2 && File.Exists(installed.Editor), "v2 online parts are verified and extracted as a combined ZIP");
        var cached = parts.Select(part => Path.Combine(store.DirectoryPath, "Downloads", "engine-" + manifest.Version, part.Sha256.ToLowerInvariant(), part.FileName)).ToArray();
        check(cached.All(File.Exists) && !Directory.EnumerateFiles(store.DirectoryPath, "*.zip", SearchOption.AllDirectories).Any(),
            "v2 cache keeps parts without generating a recombined archive");
        installer.uninstall(installed);
        foreach (var part in parts) files.Remove(distribution_asset(manifest.Tag, part.FileName));
        var calls = handler.AssetCalls;
        var reinstalled = await installer.install_release_async(published, null, CancellationToken.None);
        check(handler.AssetCalls == calls + 2 && File.Exists(reinstalled.Editor), "v2 reinstall reuses every verified cached part without fetching package data");
        installer.uninstall(reinstalled);
        File.Delete(SafePaths.regular_file(cached[0]));
        await rejected(() => installer.install_release_async(published, null, CancellationToken.None), "missing uncached v2 part fails HTTP before extraction");
        check(store.load().Engines.Count == 0 && !Directory.EnumerateDirectories(store.load().EngineDirectory).Any(),
            "missing remote part publishes no v2 engine or staging");
        var incomplete = published with { Assets = published.Assets.Where(pair => pair.Key != parts[^1].FileName)
            .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal) };
        calls = handler.AssetCalls;
        await rejected(() => installer.install_release_async(incomplete, null, CancellationToken.None), "signed part missing from release metadata rejected");
        check(handler.AssetCalls == calls + 2, "all signed part names are checked before requesting any package part");
    }

    private static async Task test_hub_distribution_download(string a_root)
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var packageName = "CueEngine.Hub-0.3.0-preview.1-full.nupkg";
        var package = Encoding.UTF8.GetBytes("signed fixture package");
        var feed = JsonSerializer.SerializeToUtf8Bytes(new { Assets = new[] { new { PackageId = "CueEngine.Hub", Version = "0.3.0-preview.1", Type = "Full", FileName = packageName,
            SHA1 = Convert.ToHexString(SHA1.HashData(package)), SHA256 = Convert.ToHexString(SHA256.HashData(package)), Size = package.Length } } });
        var manifest = new HubReleaseManifest { SchemaVersion = 1, Product = "hub", Version = "0.3.0-preview.1", Tag = "hub-v0.3.0-preview.1", PackageFileName = packageName,
            PackageSha256 = Convert.ToHexString(SHA256.HashData(package)), PackageSize = package.Length, FeedFileName = "releases.win.json", FeedSha256 = Convert.ToHexString(SHA256.HashData(feed)), FeedSize = feed.Length };
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        var files = new Dictionary<string, byte[]>
        {
            [distribution_asset(manifest.Tag, "hub-release.json")] = bytes,
            [distribution_asset(manifest.Tag, "hub-release.json.sig")] = Encoding.ASCII.GetBytes(sign(key, bytes)),
            [distribution_asset(manifest.Tag, "releases.win.json")] = feed,
            [distribution_asset(manifest.Tag, packageName)] = package
        };
        var catalog = new DistributionCatalog { SchemaVersion = 1, GeneratedAt = DateTimeOffset.UtcNow,
            Hubs = [distribution_release(manifest.Version, false, DateTimeOffset.UtcNow.AddHours(-1))] };
        using var handler = new SignedDistributionFixtureHandler(key, catalog, files);
        using var http = new HttpClient(handler);
        var store = new HubStateStore(Path.Combine(a_root, "HttpsHubSettings"));
        var packages = Path.Combine(a_root, "HttpsHubPackages");
        Directory.CreateDirectory(packages);
        var locator = new Velopack.Locators.TestVelopackLocator("CueEngine.Hub", manifest.Version, packages);
        var verifier = new PackageVerifier(key.ExportSubjectPublicKeyInfoPem());
        var offer = await new HubUpdateService(http, store, verifier, locator).prepare_github_async(true, null, CancellationToken.None);
        check(offer == null && handler.AssetCalls == 4 && handler.CatalogCalls == 2 && !handler.HasApiRequest,
            "Hub HTTPS feed validated without applying update");
        files[distribution_asset(manifest.Tag, packageName)][0] ^= 0xff;
        var corruptStore = new HubStateStore(Path.Combine(a_root, "CorruptHttpsHubSettings"));
        await rejected(() => new HubUpdateService(http, corruptStore, verifier, locator).prepare_github_async(true, null, CancellationToken.None),
            "Hub HTTPS package hash mismatch");
    }

    private static PublishedRelease distribution_release(string a_version, bool a_isEngine, DateTimeOffset a_published)
    {
        var tag = (a_isEngine ? "engine-v" : "hub-v") + a_version;
        string[] names = a_isEngine
            ? [$"CueEngine-{a_version}-windows-x64.zip", "engine-release.json", "engine-release.json.sig"]
            : [$"CueEngine.Hub-{a_version}-full.nupkg", "releases.win.json", "hub-release.json", "hub-release.json.sig", "CueEngine.Hub-win-Setup.exe"];
        return new PublishedRelease { Version = a_version, Tag = tag, IsPreview = a_version.Split('+')[0].Contains('-'), PublishedUtc = a_published,
            PageUrl = DistributionClient.RepositoryUrl + "/releases/tag/" + Uri.EscapeDataString(tag),
            Assets = names.ToDictionary(name => name, name => distribution_asset(tag, name), StringComparer.Ordinal) };
    }

    private static string distribution_asset(string a_tag, string a_name) =>
        DistributionClient.RepositoryUrl + "/releases/download/" + Uri.EscapeDataString(a_tag) + "/" + Uri.EscapeDataString(a_name);

    /// <summary>公開後に明示実行する検証。Redirect の署名 URL は記録せず、Hub 更新を適用しない</summary>
    private static async Task<int> test_live_distribution_downloads(string a_keyFile, string a_root)
    {
        try
        {
            var root = SafePaths.full(a_root);
            if (Directory.Exists(root) || File.Exists(root)) throw new IOException("Live test root already exists");
            var verifier = new PackageVerifier(File.ReadAllText(SafePaths.regular_file(a_keyFile)));
            var store = new HubStateStore(Path.Combine(root, "Settings"));
            store.update(state => state.EngineDirectory = Path.Combine(root, "Engines"));
            using var trace = new DistributionTraceHandler();
            using var http = new HttpClient(trace) { Timeout = TimeSpan.FromMinutes(5) };
            var releases = await new EngineCatalogService(http, store, verifier).refresh_async(CancellationToken.None, true);
            var installed = await new EngineInstallService(store, http, verifier, Path.Combine(root, "Locks"))
                .install_release_async(releases.First(), null, CancellationToken.None);
            var packages = Path.Combine(root, "HubPackages");
            Directory.CreateDirectory(packages);
            var locator = new Velopack.Locators.TestVelopackLocator("CueEngine.Hub", "9999.0.0", packages);
            var offer = await new HubUpdateService(http, store, verifier, locator).prepare_github_async(true, null, CancellationToken.None);
            check(offer == null && trace.Requests.Count >= 9, "live static catalog Engine install and Hub feed verified without update");
            HubStateStore.atomic_write(Path.Combine(root, "result.json"), JsonSerializer.SerializeToUtf8Bytes(new
            {
                engineVersion = installed.Version, engineBuildId = installed.BuildId, engineRoot = installed.Root,
                hubFeedVerified = true, updateApplied = false, distributionRequests = trace.Requests
            }, HubStateStore.JsonOptions));
            Console.WriteLine("Static catalog live installation and Hub feed validation passed: " + root);
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    private sealed class DistributionTraceHandler : DelegatingHandler
    {
        public List<string> Requests { get; } = [];
        public DistributionTraceHandler() : base(new HttpClientHandler()) { }
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel)
        {
            var url = a_request.RequestUri!.AbsoluteUri;
            check(url == DistributionClient.CatalogUrl || url == DistributionClient.CatalogUrl + ".sig" ||
                url.StartsWith(DistributionClient.RepositoryUrl + "/releases/download/", StringComparison.Ordinal), "live transport uses static catalog or normal download");
            check(a_request.Headers.Authorization == null && !url.Contains("api.github.com", StringComparison.Ordinal), "live public downloads need no API credentials");
            Requests.Add(url);
            return base.SendAsync(a_request, a_cancel);
        }
    }

    /// <summary>通常の公開 URL だけを受け、署名付き Catalog と Asset Fixture を返す</summary>
    private sealed class SignedDistributionFixtureHandler(ECDsa a_key, DistributionCatalog a_catalog, IReadOnlyDictionary<string, byte[]>? a_files = null) : HttpMessageHandler
    {
        public DistributionCatalog Catalog { get; set; } = a_catalog;
        public byte[]? RawCatalog { get; init; }
        public bool IsOffline { get; set; }
        public int CatalogCalls { get; private set; }
        public int AssetCalls { get; private set; }
        public bool HasApiRequest { get; private set; }
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel)
        {
            a_cancel.ThrowIfCancellationRequested();
            var url = a_request.RequestUri!.AbsoluteUri;
            HasApiRequest |= url.Contains("api.github.com", StringComparison.Ordinal);
            check(!HasApiRequest && a_request.Headers.Authorization == null && a_request.Headers.UserAgent.Any(), "distribution request has UA and no API credentials");
            if (IsOffline) throw new HttpRequestException("fixture offline");
            byte[] bytes;
            if (url == DistributionClient.CatalogUrl || url == DistributionClient.CatalogUrl + ".sig")
            {
                ++CatalogCalls;
                var raw = RawCatalog ?? JsonSerializer.SerializeToUtf8Bytes(Catalog, HubStateStore.JsonOptions);
                bytes = url == DistributionClient.CatalogUrl ? raw : Encoding.ASCII.GetBytes(sign(a_key, raw));
            }
            else
            {
                ++AssetCalls;
                check(url.StartsWith(DistributionClient.RepositoryUrl + "/releases/download/", StringComparison.Ordinal), "asset uses normal HTTPS URL");
                if (a_files == null || !a_files.TryGetValue(url, out bytes!))
                    return Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound));
            }
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) });
        }
    }
}
