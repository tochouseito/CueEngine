using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Cue.Hub;

namespace Cue.Hub.Tests;

internal static class Program
{
    private static int s_checks;
    private static void check(bool a_condition, string a_message)
    {
        if (!a_condition) throw new InvalidOperationException("CHECK: " + a_message);
        ++s_checks;
    }
    private static async Task rejected(Func<Task> a_action, string a_message)
    {
        try { await a_action(); }
        catch (Exception error) when (error is IOException or InvalidDataException or CryptographicException or InvalidOperationException or FormatException or JsonException or OperationCanceledException)
        { ++s_checks; return; }
        throw new InvalidOperationException("Expected rejection: " + a_message);
    }

    public static async Task<int> Main(string[] a_arguments)
    {
        if (a_arguments.Length == 6 && a_arguments[0] == "--hub-update")
        {
            return await test_installed_hub_update(a_arguments[1], a_arguments[2], a_arguments[3], a_arguments[4], a_arguments[5]);
        }
        if (a_arguments.Length == 1 && a_arguments[0] == "--velopack-api")
        {
            foreach (var constructor in typeof(Velopack.Locators.TestVelopackLocator).GetConstructors())
                Console.WriteLine(string.Join(", ", constructor.GetParameters().Select(item => item.ParameterType.Name + " " + item.Name + (item.HasDefaultValue ? "=" + item.DefaultValue : ""))));
            return 0;
        }
        var root = Path.Combine(Path.GetTempPath(), "CueHubTests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            await test_packages(root);
            await test_catalog(root);
            await test_hub_feed_validation(root);
            test_state_and_paths(root);
            if (a_arguments.Length == 2 && a_arguments[0] == "--engine-root") await test_real_project(a_arguments[1], root);
            Console.WriteLine($"CueHub checks passed: {s_checks}");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        finally { SafePaths.delete_owned_tree(root, Path.GetTempPath()); }
    }

    /// <summary>実インストールの配置に公開 Test Locator を対応させ、製品と同じ署名・更新 Service を使う</summary>
    private static async Task<int> test_installed_hub_update(string a_root, string a_feed, string a_key, string a_currentVersion, string a_record)
    {
        try
        {
            var root = SafePaths.full(a_root);
            SafePaths.no_links(root);
            var current = Path.Combine(root, "current");
            var updater = SafePaths.regular_file(Path.Combine(root, "Update.exe"));
            var executable = SafePaths.regular_file(Path.Combine(current, "CueHub.exe"));
            using var client = new HttpClient();
            var verifier = new PackageVerifier(File.ReadAllText(SafePaths.regular_file(a_key)));
            var state = new HubStateStore(Path.Combine(Directory.GetParent(root)!.FullName, "UpdaterTestSettings"));
            var locator = new Velopack.Locators.WindowsVelopackLocator(new InstalledHubTestProcess(executable, Path.Combine(root, "HubUpdater.log")), new Velopack.Logging.NullVelopackLogger());
            check(locator.CurrentlyInstalledVersion?.ToString() == a_currentVersion, "installed version detected by production locator");
            var service = new HubUpdateService(client, state, verifier, locator);
            var offer = await service.prepare_local_async(a_feed, null, CancellationToken.None);
            check(offer != null, "actual Hub update offer");
            HubStateStore.atomic_write(SafePaths.full(a_record), JsonSerializer.SerializeToUtf8Bytes(new { phase = "verified-before-apply", version = offer!.Manifest.Version }, HubStateStore.JsonOptions));
            Console.WriteLine("Verified Hub update: " + offer.Manifest.Version);
            await service.apply_and_restart_async(offer, CancellationToken.None);
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    /// <summary>実際のインストール配置を指定し、実 PID の終了待ちと再起動を使う専用 Test Driver</summary>
    private sealed class InstalledHubTestProcess(string a_executable, string a_log) : Velopack.Locators.IProcessImpl
    {
        public string GetCurrentProcessPath() => a_executable;
        public uint GetCurrentProcessId() => checked((uint)Environment.ProcessId);
        public void StartProcess(string exePath, IEnumerable<string> args, string workDir, bool showWindow)
        {
            var start = new System.Diagnostics.ProcessStartInfo(exePath) { UseShellExecute = false, WorkingDirectory = workDir, CreateNoWindow = !showWindow };
            start.ArgumentList.Add("--log"); start.ArgumentList.Add(a_log);
            foreach (var argument in args) start.ArgumentList.Add(argument);
            using var process = System.Diagnostics.Process.Start(start) ?? throw new IOException("Updater を起動できません");
        }
        public void Exit(int exitCode) => Environment.Exit(exitCode);
    }

    private static async Task test_packages(string a_root)
    {
        using var signing = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var verifier = new PackageVerifier(signing.ExportSubjectPublicKeyInfoPem());
        using var other = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        using var http = new HttpClient(new FixtureHandler([]));
        var store = new HubStateStore(Path.Combine(a_root, "Settings"));
        var engineRoot = Path.Combine(a_root, "Engines");
        store.update(state => state.EngineDirectory = engineRoot);
        var leases = Path.Combine(a_root, "Locks");
        var installer = new EngineInstallService(store, http, verifier, leases);
        var package = make_package(a_root, "0.1.0-preview.1");
        var manifest = make_manifest(package, "0.1.0-preview.1");
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        var signature = sign(signing, bytes);
        check(verifier.verify(bytes, signature).Version == manifest.Version, "signature accepted");
        await rejected(() => Task.FromResult(new PackageVerifier(other.ExportSubjectPublicKeyInfoPem()).verify(bytes, signature)), "wrong publisher");
        var changed = bytes.ToArray(); changed[^2] ^= 1;
        await rejected(() => Task.FromResult(verifier.verify(changed, signature)), "mutated signed bytes");
        await rejected(() => Task.FromResult(new PackageVerifier(signing.ExportECPrivateKeyPem())), "private key not accepted as trust");
        var installed = await installer.install_local_async(package, bytes, signature, null, CancellationToken.None);
        check(installed.IsManaged && File.Exists(installed.Editor) && store.load().Engines.Count == 1, "publish registered verified engine");
        await rejected(() => installer.install_local_async(package, bytes, signature, null, CancellationToken.None), "no overwrite");
        using (EngineLease.acquire(installed.LeaseId, false, leases))
        {
            await rejected(() => { installer.uninstall(installed); return Task.CompletedTask; }, "in use uninstall");
            await rejected(() => installer.install_local_async(package, bytes, signature, null, CancellationToken.None, installed), "in use repair");
        }
        check(File.Exists(installed.Editor), "in use engine preserved");
        var badManifest = manifest with { PackageSha256 = new string('0', 64) };
        var badBytes = JsonSerializer.SerializeToUtf8Bytes(badManifest, HubStateStore.JsonOptions);
        await rejected(() => installer.install_local_async(package, badBytes, sign(signing, badBytes), null, CancellationToken.None, installed), "bad hash repair");
        check(File.Exists(installed.Editor), "old version survives verification failure");
        var anotherBuild = manifest with { BuildId = "different-commit" };
        var anotherBytes = JsonSerializer.SerializeToUtf8Bytes(anotherBuild, HubStateStore.JsonOptions);
        await rejected(() => installer.install_local_async(package, anotherBytes, sign(signing, anotherBytes), null, CancellationToken.None, installed), "same version different build repair");
        File.Delete(installed.ProjectTool);
        var repaired = await installer.install_local_async(package, bytes, signature, null, CancellationToken.None, installed);
        check(File.Exists(repaired.ProjectTool) && repaired.Id == installed.Id, "repair missing payload with same identity");
        await rejected(() => { installer.uninstall(repaired with { IsManaged = false }); return Task.CompletedTask; }, "local registration cannot delete");
        await rejected(() => { installer.uninstall(repaired with { ReceiptId = Guid.NewGuid().ToString("D") }); return Task.CompletedTask; }, "forged receipt rejected");
        var second = make_package(a_root, "0.2.0-preview.1");
        var secondManifest = make_manifest(second, "0.2.0-preview.1");
        var secondBytes = JsonSerializer.SerializeToUtf8Bytes(secondManifest, HubStateStore.JsonOptions);
        using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
        await rejected(() => installer.install_local_async(second, secondBytes, sign(signing, secondBytes), null, cancelled.Token), "cancelled install");
        var secondInstalled = await installer.install_local_async(second, secondBytes, sign(signing, secondBytes), null, CancellationToken.None);
        check(store.load().Engines.Count == 2 && File.Exists(repaired.Editor), "parallel versions coexist");
        installer.uninstall(secondInstalled);
        installer.uninstall(repaired);
        check(store.load().Engines.Count == 0 && !Directory.Exists(repaired.Root), "managed uninstall");
        foreach (var evil in new[] { "../outside.txt", "Bin/Editor/../../../../outside.txt", "C:/absolute.txt", "a\\b.txt", "CON.txt", "safe/duplicate.txt" })
        {
            var path = Path.Combine(a_root, "evil-" + Guid.NewGuid().ToString("N") + ".zip");
            using (var zip = ZipFile.Open(path, ZipArchiveMode.Create))
            {
                zip.CreateEntry(evil);
                if (evil == "safe/duplicate.txt") zip.CreateEntry("SAFE/DUPLICATE.TXT");
            }
            var entryManifest = make_manifest(path, "0.9.0-preview.1") with { ExpandedSize = 1 };
            var entryBytes = JsonSerializer.SerializeToUtf8Bytes(entryManifest, HubStateStore.JsonOptions);
            await rejected(() => installer.install_local_async(path, entryBytes, sign(signing, entryBytes), null, CancellationToken.None), "unsafe zip: " + evil);
        }
        check(!File.Exists(Path.Combine(a_root, "outside.txt")), "Zip did not escape staging");
    }

    private static async Task test_catalog(string a_root)
    {
        var first = new List<object>();
        // Hub や Draft が 1 ページ目を埋めても、次ページの Engine を取得する
        for (var index = 0; index < 100; ++index) first.Add(release($"hub-v0.0.{index}", false, false, index));
        var second = new List<object>();
        for (var index = 0; index < 14; ++index) second.Add(release($"engine-v0.1.{index}", false, index == 13, index));
        second.Add(release("engine-v0.2.0-preview.1", true, false, 20));
        second.Add(release("engine-v0.4.0", false, false, 30, true));
        var handler = new FixtureHandler([JsonSerializer.Serialize(first), JsonSerializer.Serialize(second)]);
        using var http = new HttpClient(handler);
        var store = new HubStateStore(Path.Combine(a_root, "Catalog"));
        var service = new EngineCatalogService(http, store);
        var list = await service.refresh_async(CancellationToken.None);
        check(handler.Calls == 2 && list.Count == 10, "catalog pagination and total ten");
        check(list[0].Version == "0.2.0-preview.1" && list[0].IsPreview, "preview classification and publish ordering");
        check(list.All(item => item.Tag.StartsWith("engine-v", StringComparison.Ordinal) && item.Version != "0.1.13" && item.Version != "0.4.0"), "exclude hub draft incomplete");
        using var offline = new HttpClient(new FixtureHandler([]));
        var cachedService = new EngineCatalogService(offline, store);
        var cached = await cachedService.refresh_async(CancellationToken.None);
        check(cached.Count == 10 && cachedService.LastWarning != null, "offline catalog retained");
        check(!EngineCatalogService.is_asset_url("https://evil.example/package.zip", list[0].Tag), "foreign URL rejected");
        check(!EngineCatalogService.is_asset_url("http://github.com/tochouseito/CueEngine/releases/download/" + list[0].Tag + "/a", list[0].Tag), "http URL rejected");
        check(!EngineCatalogService.is_asset_url("https://github.com/other/repo/releases/download/" + list[0].Tag + "/a", list[0].Tag), "foreign repository rejected");
    }

    private static void test_state_and_paths(string a_root)
    {
        var store = new HubStateStore(Path.Combine(a_root, "State"));
        store.update(state => state.IncludePreview = false);
        check(!new HubStateStore(store.DirectoryPath).load().IncludePreview, "state survives reload");
        check(SafePaths.is_version("0.1.0-preview.1") && !SafePaths.is_version("01.0.0") && !SafePaths.is_version("1.0.0-preview.01"), "semver strict");
        using var shared = EngineLease.acquire("engine-1.0.0-windows-x64", false, Path.Combine(a_root, "StateLocks"));
        using var second = EngineLease.acquire("engine-1.0.0-windows-x64", false, Path.Combine(a_root, "StateLocks"));
        check(shared.CanRead && second.CanRead, "shared engine users coexist");
    }

    private static async Task test_hub_feed_validation(string a_root)
    {
        var feedRoot = Path.Combine(a_root, "HubFeed");
        Directory.CreateDirectory(feedRoot);
        var packageName = "CueEngine.Hub-0.2.0-preview.1-full.nupkg";
        var packageBytes = Encoding.UTF8.GetBytes("fixture package");
        File.WriteAllBytes(Path.Combine(feedRoot, packageName), packageBytes);
        var feed = JsonSerializer.SerializeToUtf8Bytes(new { Assets = new[] { new { FileName = packageName, Type = "Full" } } });
        File.WriteAllBytes(Path.Combine(feedRoot, "releases.win.json"), feed);
        var manifest = new HubReleaseManifest { SchemaVersion = 1, Product = "hub", Version = "0.2.0-preview.1", Tag = "hub-v0.2.0-preview.1", PackageFileName = packageName, PackageSha256 = Convert.ToHexString(SHA256.HashData(packageBytes)), PackageSize = packageBytes.Length, FeedFileName = "releases.win.json", FeedSha256 = Convert.ToHexString(SHA256.HashData(feed)), FeedSize = feed.Length };
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        using var otherKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        using var http = new HttpClient();
        var store = new HubStateStore(Path.Combine(a_root, "HubFeedSettings"));
        var service = new HubUpdateService(http, store, new PackageVerifier(key.ExportSubjectPublicKeyInfoPem()));
        void write(HubReleaseManifest value, ECDsa signer)
        {
            var bytes = JsonSerializer.SerializeToUtf8Bytes(value, HubStateStore.JsonOptions);
            File.WriteAllBytes(Path.Combine(feedRoot, "hub-release.json"), bytes);
            File.WriteAllText(Path.Combine(feedRoot, "hub-release.json.sig"), sign(signer, bytes));
        }
        write(manifest, otherKey);
        await rejected(() => service.prepare_local_async(feedRoot, null, CancellationToken.None), "Hub wrong publisher");
        write(manifest with { FeedSha256 = new string('0', 64) }, key);
        await rejected(() => service.prepare_local_async(feedRoot, null, CancellationToken.None), "Hub tampered feed");
        write(manifest with { PackageSha256 = new string('0', 64) }, key);
        await rejected(() => service.prepare_local_async(feedRoot, null, CancellationToken.None), "Hub tampered package");
        write(manifest with { Product = "engine" }, key);
        await rejected(() => service.prepare_local_async(feedRoot, null, CancellationToken.None), "Engine cannot replace Hub");
        write(manifest with { PackageFileName = "../outside-full.nupkg" }, key);
        await rejected(() => service.prepare_local_async(feedRoot, null, CancellationToken.None), "Hub feed path traversal");
        var unsafeFeed = JsonSerializer.SerializeToUtf8Bytes(new { Assets = new[] { new { FileName = packageName, Type = "Full" }, new { FileName = "unsigned-delta.nupkg", Type = "Delta" } } });
        File.WriteAllBytes(Path.Combine(feedRoot, "releases.win.json"), unsafeFeed);
        write(manifest with { FeedSha256 = Convert.ToHexString(SHA256.HashData(unsafeFeed)), FeedSize = unsafeFeed.Length }, key);
        await rejected(() => service.prepare_local_async(feedRoot, null, CancellationToken.None), "Hub unsigned delta excluded");
    }

    private static async Task test_real_project(string a_engineRoot, string a_testRoot)
    {
        var store = new HubStateStore(Path.Combine(a_testRoot, "RealProject"));
        var engine = new EngineRegistryService(store).register_local(SafePaths.full(a_engineRoot));
        var service = new ProjectService(store);
        var created = await service.create_async(engine, Path.Combine(a_testRoot, "NewProject"), "NewProject", "NewProject", CancellationToken.None);
        var loaded = await service.register_async(engine, created.ProjectFile, CancellationToken.None);
        check(loaded.EngineVersion == engine.Version && loaded.ProjectId == created.ProjectId, "real project create validate without template selection");
        var scene = File.ReadAllText(Path.Combine(Path.GetDirectoryName(created.ProjectFile)!, "Content", "Scenes", "Main.cuescene"));
        check(scene.Contains("camera.projection=perspective", StringComparison.Ordinal) && !scene.Contains("mesh.", StringComparison.Ordinal), "new project contains camera without sample meshes");
        await rejected(() => service.create_async(engine, Path.GetDirectoryName(created.ProjectFile)!, "NewProject", "NewProject", CancellationToken.None), "existing project preserved");
        var bytes = File.ReadAllBytes(created.ProjectFile);
        service.unregister(created);
        check(File.ReadAllBytes(created.ProjectFile).SequenceEqual(bytes), "unregister preserved project files");
        // 未導入版の Project も登録できるが、別版の Editor は起動させない
        var futureVersion = "999.0.0-preview.1";
        File.WriteAllText(created.ProjectFile, Encoding.UTF8.GetString(bytes).Replace("engineVersion=" + engine.Version, "engineVersion=" + futureVersion, StringComparison.Ordinal), new UTF8Encoding(false));
        var unavailable = await service.register_async(engine, created.ProjectFile, CancellationToken.None);
        check(unavailable.EngineVersion == futureVersion && store.load().Projects.Any(item => item.ProjectId == created.ProjectId), "register project with unavailable engine version");
        await rejected(() => service.open_async(engine, unavailable, CancellationToken.None), "do not open project in another engine version");
        service.unregister(unavailable);
        new EngineRegistryService(store).unregister_local(engine);
        check(File.Exists(engine.Editor), "unregister local engine preserved executable");
    }

    private static object release(string a_tag, bool a_preview, bool a_draft, int a_day, bool a_missing = false)
    {
        var version = a_tag[8..];
        var names = new[] { $"CueEngine-{version}-windows-x64.zip", "engine-release.json", "engine-release.json.sig" };
        return new { tag_name = a_tag, prerelease = a_preview, draft = a_draft, published_at = DateTimeOffset.UtcNow.AddDays(a_day).ToString("O"), assets = names.Take(a_missing ? 1 : 3).Select(name => new { name, browser_download_url = EngineCatalogService.RepositoryUrl + "/releases/download/" + a_tag + "/" + name }).ToArray() };
    }

    private static string sign(ECDsa a_key, byte[] a_bytes) => Convert.ToBase64String(a_key.SignData(a_bytes, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation));

    private static string make_package(string a_root, string a_version)
    {
        var content = Path.Combine(a_root, "Payload-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(content, "Bin", "Editor"));
        Directory.CreateDirectory(Path.Combine(content, "Bin", "Tools"));
        File.WriteAllText(Path.Combine(content, "CMakeLists.txt"), "# fixture\n");
        File.WriteAllText(Path.Combine(content, "CMakePresets.json"), "{}\n");
        File.WriteAllText(Path.Combine(content, "Bin", "Editor", "CueEditorHost.exe"), "fixture");
        File.WriteAllText(Path.Combine(content, "Bin", "Tools", "CueProjectTool.exe"), "fixture");
        File.WriteAllText(Path.Combine(content, "engine.cueengine"), $"formatVersion=1\nproduct=engine\nversion={a_version}\nbuildId=test-commit\nplatform=Windows\narchitecture=x64\nconfiguration=Development\nsdkAbiVersion=1\nprojectFormatMin=1\nprojectFormatMax=2\neditor=Bin/Editor/CueEditorHost.exe\nprojectTool=Bin/Tools/CueProjectTool.exe\nuseLeaseId=engine-{a_version}-windows-x64\n");
        var zip = Path.Combine(a_root, "Package-" + Guid.NewGuid().ToString("N") + ".zip");
        ZipFile.CreateFromDirectory(content, zip);
        return zip;
    }

    private static ReleasePackageManifest make_manifest(string a_package, string a_version)
    {
        using var zip = ZipFile.OpenRead(a_package);
        var expanded = zip.Entries.Sum(entry => entry.Length);
        return new ReleasePackageManifest { SchemaVersion = 1, Product = "engine", Version = a_version, Tag = "engine-v" + a_version, Platform = "Windows", Architecture = "x64", Configuration = "Development", BuildId = "test-commit", PackageFileName = $"CueEngine-{a_version}-windows-x64.zip", PackageSize = new FileInfo(a_package).Length, ExpandedSize = expanded, PackageSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(a_package))) };
    }

    private sealed class FixtureHandler(IReadOnlyList<string> a_pages) : HttpMessageHandler
    {
        public int Calls { get; private set; }
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel)
        {
            a_cancel.ThrowIfCancellationRequested();
            if (Calls >= a_pages.Count) throw new HttpRequestException("fixture offline");
            var json = a_pages[Calls++];
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(json, Encoding.UTF8, "application/json") });
        }
    }
}
