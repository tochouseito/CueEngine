using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Cue.Hub;

namespace Cue.Hub.Tests;

internal static partial class Program
{
    /// <summary>ブラウザー向け URL に戻らず、署名付きの Engine と Hub を API から取得できることを確認する</summary>
    private static async Task test_github_downloads(string a_root)
    {
        test_asset_metadata();
        await test_legacy_catalog(a_root);
        await test_engine_api_download(a_root);
        await test_hub_api_download(a_root);
        using var http = new HttpClient(new GitHubFailureHandler());
        var client = new GitHubReleaseClient(http);
        try
        {
            using var response = await client.get_asset_async(GitHubReleaseClient.asset_url(1), CancellationToken.None);
            throw new InvalidOperationException("HTTP failure was not reported");
        }
        catch (HttpRequestException error)
        {
            check(error.StatusCode == HttpStatusCode.GatewayTimeout && error.Message.Contains("/releases/assets/1"),
                "HTTP failure retains status and asset API URL");
        }
    }

    /// <summary>API の ID と公開 Tag / File の対応を照合し、取得先のすり替えを拒否する</summary>
    private static void test_asset_metadata()
    {
        var metadata = JsonSerializer.SerializeToNode(release("engine-v0.3.0-preview.1", true, false, 0))!.AsObject();
        PublishedRelease? parse(JsonObject value) => EngineCatalogService.parse_release(JsonSerializer.SerializeToElement(value), "engine-v", true);
        check(parse(metadata) != null, "valid asset IDs and URLs accepted");
        foreach (var url in new[]
        {
            "https://evil.example/1", "http://api.github.com/repos/tochouseito/CueEngine/releases/assets/1",
            "https://api.github.com/repos/other/CueEngine/releases/assets/1",
            GitHubReleaseClient.asset_url(1000) + "?download=1", GitHubReleaseClient.asset_url(1000) + "#fragment",
            GitHubReleaseClient.asset_url(1000) + "/extra", GitHubReleaseClient.asset_url(1000) + "/../1000",
            "https://api.github.com:443/repos/tochouseito/CueEngine/releases/assets/1000",
            "https://user@api.github.com/repos/tochouseito/CueEngine/releases/assets/1000",
            "https://api.github.com/repos/tochouseito/CueEngine/releases/assets/01000",
            "https://api.github.com/repos/tochouseito/CueEngine/releases/assets/0",
            "https://api.github.com/repos/tochouseito/CueEngine/releases/assets/9223372036854775808"
        })
        {
            check(!GitHubReleaseClient.is_asset_url(url), "unsafe API URL rejected: " + url);
            var changed = metadata.DeepClone().AsObject();
            changed["assets"]![0]!["url"] = url;
            check(parse(changed) == null, "unsafe API metadata rejected");
        }
        check(!GitHubReleaseClient.is_asset_url(null), "null API URL rejected");
        foreach (var id in new JsonNode?[] { null, JsonValue.Create(0), JsonValue.Create(-1), JsonValue.Create("1000"), JsonValue.Create(1001) })
        {
            var changed = metadata.DeepClone().AsObject();
            changed["assets"]![0]!["id"] = id;
            check(parse(changed) == null, "missing invalid or mismatched ID rejected");
        }
        var wrongName = metadata.DeepClone().AsObject();
        wrongName["assets"]![0]!["browser_download_url"] = EngineCatalogService.RepositoryUrl + "/releases/download/engine-v0.3.0-preview.1/other.zip";
        check(parse(wrongName) == null, "asset filename binding checked");
        var wrongTag = metadata.DeepClone().AsObject();
        wrongTag["tag_name"] = "engine-v0.4.0-preview.1";
        check(parse(wrongTag) == null, "asset tag binding checked");
        var duplicate = metadata.DeepClone().AsObject();
        duplicate["assets"]![1]!["id"] = 1000;
        duplicate["assets"]![1]!["url"] = GitHubReleaseClient.asset_url(1000);
        check(parse(duplicate) == null, "duplicate asset ID rejected");
    }

    /// <summary>旧 Cache の URL を再利用せず、既存の設定と旧 File を保持する</summary>
    private static async Task test_legacy_catalog(string a_root)
    {
        var store = new HubStateStore(Path.Combine(a_root, "LegacyCatalog"));
        store.update(state => state.IncludePreview = false);
        var legacy = Path.Combine(store.DirectoryPath, "engine-catalog.json");
        File.WriteAllText(legacy, "[]");
        using var http = new HttpClient(new FixtureHandler([]));
        await rejected(() => new EngineCatalogService(http, store).refresh_async(CancellationToken.None), "legacy cache requires online refresh");
        check(File.ReadAllText(legacy) == "[]" && !store.load().IncludePreview, "legacy cache and Hub settings preserved");
    }

    /// <summary>API 本体から通常の署名・Hash 検証を通して導入し、破損時は登録しない</summary>
    private static async Task test_engine_api_download(string a_root)
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var package = make_package(a_root, "0.3.0-preview.1");
        var manifest = make_manifest(package, "0.3.0-preview.1");
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        var files = new Dictionary<string, byte[]>
        {
            [manifest.PackageFileName] = File.ReadAllBytes(package),
            ["engine-release.json"] = bytes,
            ["engine-release.json.sig"] = Encoding.ASCII.GetBytes(sign(key, bytes))
        };
        var metadata = github_metadata(manifest.Tag, files.Keys);
        using var handler = new GitHubFixtureHandler(metadata, files);
        using var http = new HttpClient(handler);
        var store = new HubStateStore(Path.Combine(a_root, "ApiInstallSettings"));
        store.update(state => state.EngineDirectory = Path.Combine(a_root, "ApiEngines"));
        var releases = await new EngineCatalogService(http, store).refresh_async(CancellationToken.None);
        var installer = new EngineInstallService(store, http, new PackageVerifier(key.ExportSubjectPublicKeyInfoPem()), Path.Combine(a_root, "ApiLocks"));
        var installed = await installer.install_release_async(releases.Single(), null, CancellationToken.None);
        check(handler.CatalogCalls == 1 && handler.AssetCalls == 3 && File.Exists(installed.Editor), "Engine installed entirely through asset API");
        check(http.DefaultRequestHeaders.Accept.Count == 0 && http.DefaultRequestHeaders.UserAgent.Count == 0, "shared HttpClient headers unchanged");
        installer.uninstall(installed);
        var invalid = releases.Single() with { Assets = new() { ["engine-release.json"] = "https://evil.example/manifest" } };
        var calls = handler.AssetCalls;
        await rejected(() => installer.install_release_async(invalid, null, CancellationToken.None), "untrusted API rejected before request");
        check(handler.AssetCalls == calls, "untrusted URL never requested");
        files[manifest.PackageFileName][^1] ^= 0xff;
        await rejected(() => installer.install_release_async(releases.Single(), null, CancellationToken.None), "online package hash mismatch rejected");
        check(store.load().Engines.Count == 0 && !Directory.EnumerateDirectories(store.load().EngineDirectory).Any(), "failed API download leaves no staging or registration");

        var cacheFile = Path.Combine(store.DirectoryPath, "engine-catalog-v2.json");
        var cache = JsonNode.Parse(File.ReadAllText(cacheFile))!;
        cache[0]!["assets"]!["engine-release.json"] = "https://evil.example/manifest";
        File.WriteAllText(cacheFile, cache.ToJsonString());
        using var offline = new HttpClient(new FixtureHandler([]));
        await rejected(() => new EngineCatalogService(offline, store).refresh_async(CancellationToken.None), "tampered API cache rejected");
    }

    /// <summary>Hub 用の 4 Asset を API 取得し、既存のローカル Feed 検証に接続する。更新適用は行わない</summary>
    private static async Task test_hub_api_download(string a_root)
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var packageName = "CueEngine.Hub-0.3.0-preview.1-full.nupkg";
        var package = Encoding.UTF8.GetBytes("signed fixture package");
        var feed = JsonSerializer.SerializeToUtf8Bytes(new { Assets = new[] { new { PackageId = "CueEngine.Hub", Version = "0.3.0-preview.1", Type = "Full", FileName = packageName, SHA1 = Convert.ToHexString(SHA1.HashData(package)), SHA256 = Convert.ToHexString(SHA256.HashData(package)), Size = package.Length } } });
        var manifest = new HubReleaseManifest { SchemaVersion = 1, Product = "hub", Version = "0.3.0-preview.1", Tag = "hub-v0.3.0-preview.1", PackageFileName = packageName, PackageSha256 = Convert.ToHexString(SHA256.HashData(package)), PackageSize = package.Length, FeedFileName = "releases.win.json", FeedSha256 = Convert.ToHexString(SHA256.HashData(feed)), FeedSize = feed.Length };
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        var files = new Dictionary<string, byte[]>
        {
            ["hub-release.json"] = bytes,
            ["hub-release.json.sig"] = Encoding.ASCII.GetBytes(sign(key, bytes)),
            ["releases.win.json"] = feed,
            [packageName] = package
        };
        using var handler = new GitHubFixtureHandler(github_metadata(manifest.Tag, files.Keys), files);
        using var http = new HttpClient(handler);
        var store = new HubStateStore(Path.Combine(a_root, "ApiHubSettings"));
        var packages = Path.Combine(a_root, "ApiHubPackages");
        Directory.CreateDirectory(packages);
        var locator = new Velopack.Locators.TestVelopackLocator("CueEngine.Hub", manifest.Version, packages);
        var service = new HubUpdateService(http, store, new PackageVerifier(key.ExportSubjectPublicKeyInfoPem()), locator);
        var offer = await service.prepare_github_async(true, null, CancellationToken.None);
        check(offer == null && handler.AssetCalls == 4 && handler.CatalogCalls == 1, "Hub API feed validated without applying an update");
        files[packageName][0] ^= 0xff;
        await rejected(() => service.prepare_github_async(true, null, CancellationToken.None), "Hub API package hash mismatch rejected");
    }

    private static JsonObject github_metadata(string a_tag, IEnumerable<string> a_names)
    {
        return JsonSerializer.SerializeToNode(new
        {
            tag_name = a_tag, prerelease = true, draft = false, published_at = DateTimeOffset.UtcNow.ToString("O"),
            assets = a_names.Select((name, index) => new { name, id = index + 1, url = GitHubReleaseClient.asset_url(index + 1), browser_download_url = EngineCatalogService.RepositoryUrl + "/releases/download/" + a_tag + "/" + name })
        })!.AsObject();
    }

    /// <summary>明示した専用 Root に公開 Engine を導入し、実 Handler の 302 追跡と Hub Feed の取得も検証する</summary>
    private static async Task<int> test_live_github_downloads(string a_keyFile, string a_root)
    {
        try
        {
            var root = SafePaths.full(a_root);
            if (Directory.Exists(root) || File.Exists(root)) throw new IOException("Live test root already exists");
            var verifier = new PackageVerifier(File.ReadAllText(SafePaths.regular_file(a_keyFile)));
            var store = new HubStateStore(Path.Combine(root, "Settings"));
            store.update(state => state.EngineDirectory = Path.Combine(root, "Engines"));
            using var trace = new GitHubApiTraceHandler();
            using var http = new HttpClient(trace) { Timeout = TimeSpan.FromMinutes(5) };
            var releases = await new EngineCatalogService(http, store).refresh_async(CancellationToken.None);
            var installed = await new EngineInstallService(store, http, verifier, Path.Combine(root, "Locks"))
                .install_release_async(releases.First(), null, CancellationToken.None);
            var packages = Path.Combine(root, "HubPackages");
            Directory.CreateDirectory(packages);
            // 高い版数の Test Locator で Feed 検証まで実行し、実ユーザーの Hub を更新・終了しない
            var locator = new Velopack.Locators.TestVelopackLocator("CueEngine.Hub", "9999.0.0", packages);
            var offer = await new HubUpdateService(http, store, verifier, locator).prepare_github_async(true, null, CancellationToken.None);
            check(offer == null && trace.Requests.Count >= 9, "live Engine install and Hub feed verified without applying update");
            HubStateStore.atomic_write(Path.Combine(root, "result.json"), JsonSerializer.SerializeToUtf8Bytes(new
            {
                engineVersion = installed.Version, engineBuildId = installed.BuildId, engineRoot = installed.Root,
                hubFeedVerified = true, updateApplied = false, apiRequests = trace.Requests
            }, HubStateStore.JsonOptions));
            Console.WriteLine("GitHub API live installation and Hub feed validation passed: " + root);
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    /// <summary>実通信の入口が API に限定されることを記録し、署名付きの転送先 URL は保存しない</summary>
    private sealed class GitHubApiTraceHandler : DelegatingHandler
    {
        public List<string> Requests { get; } = [];
        public GitHubApiTraceHandler() : base(new HttpClientHandler()) { }

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel)
        {
            var url = a_request.RequestUri!.AbsoluteUri;
            var catalogPrefix = GitHubReleaseClient.RepositoryApiUrl + "/releases?per_page=100&page=";
            var isCatalog = url.StartsWith(catalogPrefix, StringComparison.Ordinal) &&
                int.TryParse(url.AsSpan(catalogPrefix.Length), out var page) && page > 0;
            check(GitHubReleaseClient.is_asset_url(url) || isCatalog,
                "live download starts at the release API");
            check(a_request.Headers.Authorization == null, "live public API needs no credentials");
            Requests.Add(url);
            return base.SendAsync(a_request, a_cancel);
        }
    }

    /// <summary>外部通信を行わず、API URL と要求 Header を検査して署名付きの Fixture を返す</summary>
    private sealed class GitHubFixtureHandler(JsonObject a_metadata, IReadOnlyDictionary<string, byte[]> a_files) : HttpMessageHandler
    {
        public int AssetCalls { get; private set; }
        public int CatalogCalls { get; private set; }

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel)
        {
            a_cancel.ThrowIfCancellationRequested();
            check(a_request.Headers.UserAgent.ToString() == "CueHub/0.1" && a_request.Headers.Authorization == null, "API User-Agent provided without credentials");
            var url = a_request.RequestUri!.AbsoluteUri;
            byte[] bytes;
            if (url == GitHubReleaseClient.RepositoryApiUrl + "/releases?per_page=100&page=1")
            {
                check(a_request.Headers.Accept.Single().MediaType == "application/vnd.github+json", "catalog requests JSON");
                ++CatalogCalls;
                bytes = JsonSerializer.SerializeToUtf8Bytes(new[] { a_metadata });
            }
            else
            {
                check(GitHubReleaseClient.is_asset_url(url) && a_request.Headers.Accept.Single().MediaType == "application/octet-stream", "asset requests API binary content");
                ++AssetCalls;
                var asset = a_metadata["assets"]!.AsArray().Single(item => item!["url"]!.GetValue<string>() == url)!;
                bytes = a_files[asset["name"]!.GetValue<string>()];
            }
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) });
        }
    }

    private sealed class GitHubFailureHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel) =>
            Task.FromResult(new HttpResponseMessage(HttpStatusCode.GatewayTimeout));
    }
}
