using System.Text.Json;
using Cue.Hub;

namespace Cue.Packaging.Validation;

/// <summary>製品と同じ Service を使い、実パッケージを専用 Root に導入して CLI まで接続する</summary>
internal static class Program
{
    private static async Task<int> Main(string[] a_arguments)
    {
        try
        {
            if (a_arguments.Length is < 3 or > 4)
                throw new ArgumentException("Release directory, publisher public key, new test root, and optional --open-editor are required");
            var releaseDirectory = SafePaths.full(a_arguments[0]);
            var publicKey = File.ReadAllText(SafePaths.regular_file(a_arguments[1]));
            var root = SafePaths.full(a_arguments[2]);
            if (File.Exists(root) || Directory.Exists(root)) throw new IOException("Test root already exists");
            var openEditor = a_arguments.Length == 4 && a_arguments[3] == "--open-editor";
            if (a_arguments.Length == 4 && !openEditor) throw new ArgumentException("Unknown test option");
            var store = new HubStateStore(Path.Combine(root, "HubState"));
            store.update(state => state.EngineDirectory = Path.Combine(root, "Engines"));
            using var http = new HttpClient();
            var installer = new EngineInstallService(store, http, new PackageVerifier(publicKey));
            var manifest = File.ReadAllBytes(Path.Combine(releaseDirectory, "engine-release.json"));
            var signature = File.ReadAllText(Path.Combine(releaseDirectory, "engine-release.json.sig"));
            var verified = new PackageVerifier(publicKey).verify(manifest, signature);
            var progress = new Progress<InstallProgress>(value => Console.WriteLine(value.Message));
            var engine = await installer.install_local_async(Path.Combine(releaseDirectory, verified.PackageFileName),
                manifest, signature, progress, CancellationToken.None);
            var projects = new ProjectService(store);
            var cube = await projects.create_async(engine, Path.Combine(root, "RotateCube"), "Package Cube", "PackageCube",
                "RotateCube", CancellationToken.None);
            var empty = await projects.create_async(engine, Path.Combine(root, "Empty3D"), "Empty Scene", "EmptyScene",
                "Empty3D", CancellationToken.None);
            var validated = await projects.register_async(engine, cube.ProjectFile, CancellationToken.None);
            if (validated.ProjectId != cube.ProjectId || cube.ProjectId == empty.ProjectId)
                throw new InvalidDataException("Project identities were not preserved or regenerated");
            // 実 Native Editor と同じ共有 Lease がある間、削除操作を拒否する
            using (var lease = EngineLease.acquire(engine.LeaseId, false))
            {
                var refused = false;
                try { installer.uninstall(engine); }
                catch (IOException) { refused = true; }
                if (!refused) throw new InvalidOperationException("An Engine in use was removed");
            }
            var editorPid = openEditor ? await projects.open_async(engine, cube, CancellationToken.None) : 0;
            var result = new
            {
                schemaVersion = 1, engineRoot = engine.Root, engineVersion = engine.Version,
                engineBuildId = engine.BuildId, editor = engine.Editor, projectTool = engine.ProjectTool,
                projectFile = cube.ProjectFile, emptyProjectFile = empty.ProjectFile, editorPid,
                createdAndValidated = true, sharedLeaseBlockedRemoval = true
            };
            HubStateStore.atomic_write(Path.Combine(root, "result.json"), JsonSerializer.SerializeToUtf8Bytes(result, HubStateStore.JsonOptions));
            Console.WriteLine("Engine package installation, two templates, ProjectTool validation and use lease checks passed");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
