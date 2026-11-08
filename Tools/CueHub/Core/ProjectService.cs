using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace Cue.Hub;

/// <summary>検証済み Engine の CLI を所有して待ち、Editor は独立した Process として起動する</summary>
public sealed class ProjectService(HubStateStore a_store)
{
    private readonly HubStateStore m_store = a_store;

    public async Task<ProjectRegistration> create_async(EngineRegistration a_engine, string a_directory, string a_name, string a_applicationName, CancellationToken a_cancel)
    {
        // 公開済み ProjectTool の必須引数を満たし、Hub からは空 Project だけを作成する
        var result = await run_tool_async(a_engine, ["create", "--directory", SafePaths.full(a_directory), "--name", a_name, "--application-name", a_applicationName, "--template", "Empty3D", "--engine-version", a_engine.Version], a_cancel);
        return register_result(result, a_engine, true);
    }

    public async Task<ProjectRegistration> register_async(EngineRegistration a_engine, string a_projectFile, CancellationToken a_cancel)
    {
        var path = SafePaths.regular_file(a_projectFile);
        var result = await run_tool_async(a_engine, ["validate", "--project", path], a_cancel);
        if (!SafePaths.same(result.ProjectFile, path)) throw new InvalidDataException("検証した Project の配置が違います");
        return register_result(result, a_engine, false);
    }

    public async Task<int> open_async(EngineRegistration a_engine, ProjectRegistration a_project, CancellationToken a_cancel)
    {
        var validated = await register_async(a_engine, a_project.ProjectFile, a_cancel);
        if (validated.EngineVersion.Length != 0 && validated.EngineVersion != a_engine.Version)
            throw new InvalidOperationException($"Project が指定する Engine {validated.EngineVersion} を選択してください");
        var info = new ProcessStartInfo(SafePaths.regular_file(a_engine.Editor)) { UseShellExecute = false, WorkingDirectory = SafePaths.full(a_engine.Root) };
        info.ArgumentList.Add("--project");
        info.ArgumentList.Add(validated.ProjectFile);
        using var process = Process.Start(info) ?? throw new IOException("Editor を起動できませんでした");
        m_store.update(state =>
        {
            var index = state.Projects.FindIndex(item => SafePaths.same(item.ProjectFile, validated.ProjectFile));
            if (index >= 0) state.Projects[index] = state.Projects[index] with { LastOpenedUtc = DateTimeOffset.UtcNow };
        });
        return process.Id;
    }

    public void unregister(ProjectRegistration a_project) => m_store.update(state => state.Projects.RemoveAll(item => SafePaths.same(item.ProjectFile, a_project.ProjectFile)));

    private ProjectRegistration register_result(ProjectToolResult a_result, EngineRegistration a_engine, bool a_requireVersion)
    {
        if (a_requireVersion && a_result.EngineVersion.Length != 0 && a_result.EngineVersion != a_engine.Version)
            throw new InvalidOperationException($"Project は Engine {a_result.EngineVersion} を指定しています。対応版を選択してください");
        if (a_result.SchemaVersion != 1 || !a_result.Success || !Guid.TryParseExact(a_result.ProjectId, "D", out var id) || id == Guid.Empty || string.IsNullOrWhiteSpace(a_result.Name))
            throw new InvalidDataException("ProjectTool 応答が不正です");
        var path = SafePaths.regular_file(a_result.ProjectFile);
        if (!Path.GetExtension(path).Equals(".cueproj", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException(".cueproj を選択してください");
        var registration = new ProjectRegistration { ProjectFile = path, ProjectId = a_result.ProjectId, Name = a_result.Name, EngineVersion = a_result.EngineVersion };
        m_store.update(state =>
        {
            var existing = state.Projects.FindIndex(item => SafePaths.same(item.ProjectFile, path));
            if (existing >= 0) registration = registration with { LastOpenedUtc = state.Projects[existing].LastOpenedUtc };
            state.Projects.RemoveAll(item => SafePaths.same(item.ProjectFile, path));
            state.Projects.Add(registration);
        });
        return registration;
    }

    private static async Task<ProjectToolResult> run_tool_async(EngineRegistration a_engine, IReadOnlyList<string> a_arguments, CancellationToken a_cancel)
    {
        SafePaths.no_links(a_engine.Root);
        var info = new ProcessStartInfo(SafePaths.regular_file(a_engine.ProjectTool))
        {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = a_engine.Root,
            RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = new UTF8Encoding(false, true), StandardErrorEncoding = new UTF8Encoding(false, true)
        };
        foreach (var argument in a_arguments) info.ArgumentList.Add(argument);
        using var lease = a_engine.LeaseId.Length == 0 ? null : EngineLease.acquire(a_engine.LeaseId, false);
        using var process = Process.Start(info) ?? throw new IOException("ProjectTool を起動できませんでした");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(a_cancel);
        timeout.CancelAfter(TimeSpan.FromMinutes(2));
        try
        {
            // Pipe の双方を並行回収し、一方が詰まって子が停止する状態を避ける
            var output = read_text_async(process.StandardOutput, 128 * 1024, timeout.Token);
            var error = read_text_async(process.StandardError, 128 * 1024, timeout.Token);
            await Task.WhenAll(process.WaitForExitAsync(timeout.Token), output, error);
            var result = JsonSerializer.Deserialize<ProjectToolResult>(output.Result, HubStateStore.JsonOptions) ?? throw new InvalidDataException("ProjectTool が応答しませんでした");
            if (process.ExitCode != 0 || !result.Success) throw new InvalidOperationException(result.Message.Length > 0 ? result.Message : error.Result);
            return result;
        }
        finally
        {
            if (!process.HasExited)
            {
                process.Kill(true);
                await process.WaitForExitAsync(CancellationToken.None);
            }
        }
    }

    private static async Task<string> read_text_async(StreamReader a_reader, int a_limit, CancellationToken a_cancel)
    {
        var text = new StringBuilder();
        var buffer = new char[4096];
        for (int count; (count = await a_reader.ReadAsync(buffer, a_cancel)) != 0;)
        {
            if (text.Length + count > a_limit) throw new InvalidDataException("ProjectTool の出力が大きすぎます");
            text.Append(buffer, 0, count);
        }
        return text.ToString();
    }
}
