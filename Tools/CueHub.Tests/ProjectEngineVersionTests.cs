using System.Text;
using Cue.Hub;

namespace Cue.Hub.Tests;

internal static partial class Program
{
    /// <summary>実 ProjectTool を使い、指定だけの置換と保存失敗時の正本保全を確認する</summary>
    private static async Task test_project_engine_change(HubStateStore a_store, EngineRegistration a_engine, ProjectRegistration a_project)
    {
        var service = new ProjectService(a_store);
        var file = a_project.ProjectFile;
        var original = File.ReadAllBytes(file);
        var utf8 = new UTF8Encoding(false, true);
        var source = utf8.GetString(original);
        var sceneFile = Path.Combine(Path.GetDirectoryName(file)!, "Content", "Scenes", "Main.cuescene");
        var scene = File.ReadAllBytes(sceneFile);
        var opened = DateTimeOffset.Parse("2026-01-02T03:04:05Z");
        a_store.update(state => state.Projects[0] = state.Projects[0] with { LastOpenedUtc = opened });

        foreach (var newline in new[] { "\n", "\r\n" })
        {
            // BOM・コメント・空白・末尾改行の有無を含め、値以外は Byte 単位で保つ
            var text = "\uFEFF# keep engineVersion=not-a-field" + newline + source.Replace("\r\n", "\n", StringComparison.Ordinal)
                .Replace("\n", newline, StringComparison.Ordinal).TrimEnd('\r', '\n')
                .Replace("engineVersion=" + a_project.EngineVersion, "\tengineVersion \t= \t" + a_project.EngineVersion + " \t", StringComparison.Ordinal);
            File.WriteAllBytes(file, utf8.GetBytes(text));
            var registered = await service.register_async(a_engine, file, CancellationToken.None);
            var changed = await service.change_engine_async(a_engine, registered, CancellationToken.None);
            var expected = utf8.GetBytes(text.Replace("\t" + a_project.EngineVersion + " \t", "\t" + a_engine.Version + " \t", StringComparison.Ordinal));
            check(File.ReadAllBytes(file).SequenceEqual(expected), "version replacement preserves all other descriptor bytes");
            var stored = a_store.load().Projects.Single();
            check(changed == stored && stored.EngineVersion == a_engine.Version && stored.ProjectId == a_project.ProjectId &&
                stored.Name == a_project.Name && stored.LastOpenedUtc == opened, "engine version and Hub history updated together");
            var validated = await service.register_async(a_engine, file, CancellationToken.None);
            check(validated.EngineVersion == a_engine.Version, "changed project remains valid for the target ProjectTool");
            await service.change_engine_async(a_engine, validated, CancellationToken.None);
            check(File.ReadAllBytes(file).SequenceEqual(expected), "selecting the same version does not rewrite the descriptor");
        }

        File.WriteAllBytes(file, original);
        var current = await service.register_async(a_engine, file, CancellationToken.None);
        var beforeState = File.ReadAllBytes(Path.Combine(a_store.DirectoryPath, "state.json"));
        using (var cancel = new CancellationTokenSource())
        {
            cancel.Cancel();
            await rejected(() => service.change_engine_async(a_engine, current, cancel.Token), "cancelled change preserves both originals");
        }
        await rejected(() => service.change_engine_async(a_engine with { Id = Guid.NewGuid().ToString("D") }, current, CancellationToken.None), "unregistered engine rejected");
        await rejected(() => service.change_engine_async(a_engine with { Version = "1.0.0\nname=bad" }, current, CancellationToken.None), "invalid engine version rejected");
        await rejected(() => service.change_engine_async(a_engine, current with { ProjectId = Guid.NewGuid().ToString("D") }, CancellationToken.None), "stale project identity rejected");
        await rejected(() => service.change_engine_async(a_engine, current with { EngineVersion = "123.0.0" }, CancellationToken.None), "stale project version rejected");
        check(File.ReadAllBytes(file).SequenceEqual(original) && File.ReadAllBytes(Path.Combine(a_store.DirectoryPath, "state.json")).SequenceEqual(beforeState), "validation failures preserve file and Hub state");

        File.SetAttributes(file, FileAttributes.ReadOnly);
        try { await rejected(() => service.change_engine_async(a_engine, current, CancellationToken.None), "read-only project cannot be replaced"); }
        finally { File.SetAttributes(file, FileAttributes.Normal); }
        check(File.ReadAllBytes(file).SequenceEqual(original) && a_store.load().Projects.Single() == current, "failed project save preserves registration");

        // Project の保存後に Hub 側が保存できなくても、Project の指定を旧版へ戻す
        var stateFile = Path.Combine(a_store.DirectoryPath, "state.json");
        File.SetAttributes(stateFile, FileAttributes.ReadOnly);
        try { await rejected(() => service.change_engine_async(a_engine, current, CancellationToken.None), "failed Hub save rolls back project replacement"); }
        finally { File.SetAttributes(stateFile, FileAttributes.Normal); }
        check(File.ReadAllBytes(file).SequenceEqual(original) && File.ReadAllBytes(stateFile).SequenceEqual(beforeState), "Hub save rollback preserves exact originals");
        check(!Directory.EnumerateFiles(Path.GetDirectoryName(file)!, ".cue-save-*").Any(), "temporary save files cleaned after failure");

        var legacy = string.Join('\n', source.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n')
            .Where(line => !line.StartsWith("engineVersion=", StringComparison.Ordinal))).Replace("formatVersion=2", "formatVersion=1", StringComparison.Ordinal);
        File.WriteAllBytes(file, utf8.GetBytes(legacy));
        var legacyRegistration = await service.register_async(a_engine, file, CancellationToken.None);
        await rejected(() => service.change_engine_async(a_engine, legacyRegistration, CancellationToken.None), "legacy project is not implicitly converted");
        check(File.ReadAllBytes(file).SequenceEqual(utf8.GetBytes(legacy)), "legacy format left unchanged");

        File.WriteAllBytes(file, utf8.GetBytes(source + "engineVersion=1.0.0\n"));
        var duplicate = File.ReadAllBytes(file);
        await rejected(() => service.change_engine_async(a_engine, current, CancellationToken.None), "duplicate engine version rejected by ProjectTool");
        check(File.ReadAllBytes(file).SequenceEqual(duplicate), "invalid descriptor is not partially changed");
        File.WriteAllBytes(file, [0xff, 0xfe, 0x00]);
        await rejected(() => service.change_engine_async(a_engine, current, CancellationToken.None), "invalid UTF-8 rejected");
        File.WriteAllBytes(file, new byte[65537]);
        await rejected(() => service.change_engine_async(a_engine, current, CancellationToken.None), "oversized descriptor rejected");

        File.WriteAllBytes(file, original);
        current = await service.register_async(a_engine, file, CancellationToken.None);
        service.unregister(current);
        await rejected(() => service.change_engine_async(a_engine, current, CancellationToken.None), "unregistered project cannot be changed");
        check(File.ReadAllBytes(file).SequenceEqual(original) && a_store.load().Projects.Count == 0, "unregistered project file preserved");
        await service.register_async(a_engine, file, CancellationToken.None);
        check(File.ReadAllBytes(sceneFile).SequenceEqual(scene), "scene data left unchanged by engine version changes");
    }
}
