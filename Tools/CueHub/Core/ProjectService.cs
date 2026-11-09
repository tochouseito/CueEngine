using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace Cue.Hub;

/// <summary>検証済み Engine の CLI を所有して待ち、Editor は独立した Process として起動する</summary>
public sealed class ProjectService(HubStateStore a_store)
{
    private readonly HubStateStore m_store = a_store;

    /// <summary>選択した親 Directory の直下へ Project 名の Folder を作る。表示名を Path として解釈しない</summary>
    public static string project_directory(string a_parentDirectory, string a_projectName)
    {
        if (string.IsNullOrWhiteSpace(a_projectName) || a_projectName != a_projectName.Trim() ||
            a_projectName.Contains('/') || a_projectName.Contains('\\'))
            throw new InvalidDataException("プロジェクト名にはフォルダ名として使える名前を指定してください");
        SafePaths.validate_relative(a_projectName);
        return SafePaths.child(a_parentDirectory, a_projectName);
    }

    public async Task<ProjectRegistration> create_async(EngineRegistration a_engine, string a_directory, string a_name, string a_applicationName, CancellationToken a_cancel)
    {
        a_cancel.ThrowIfCancellationRequested();
        var directory = SafePaths.full(a_directory);
        var parent = Directory.GetParent(directory)?.FullName ?? throw new InvalidDataException("プロジェクトの保存先に親フォルダがありません");
        SafePaths.regular_file(a_engine.ProjectTool);
        SafePaths.no_links(directory);
        // ProjectTool が新規作成する Project Folder は空けたまま、未作成の親階層だけを用意する
        // 親 Folder は他の Project と共有され得るため、失敗や取消でも削除しない
        Directory.CreateDirectory(parent);
        SafePaths.no_links(parent);
        a_cancel.ThrowIfCancellationRequested();
        // 公開済み ProjectTool の必須引数を満たし、Hub からは空 Project だけを作成する
        var result = await run_tool_async(a_engine, ["create", "--directory", directory, "--name", a_name, "--application-name", a_applicationName, "--template", "Empty3D", "--engine-version", a_engine.Version], a_cancel);
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

    /// <summary>UI Thread から呼び、v2 の Engine 指定だけを置換する。取消は保存前まで、保存失敗は元の File へ戻す</summary>
    public async Task<ProjectRegistration> change_engine_async(EngineRegistration a_engine, ProjectRegistration a_project, CancellationToken a_cancel)
    {
        a_cancel.ThrowIfCancellationRequested();
        var state = m_store.load();
        if (!state.Engines.Contains(a_engine) || !SafePaths.is_version(a_engine.Version))
            throw new InvalidOperationException("変更先の Engine をインストール・登録してください");
        SafePaths.regular_file(a_engine.Editor);
        var path = SafePaths.regular_file(a_project.ProjectFile);
        var original = read_project_bytes(path);
        // Target の ProjectTool は形式の検証だけに使い、互換性判定やデータ変換は行わない
        var result = await run_tool_async(a_engine, ["validate", "--project", path], a_cancel);
        var validated = registration(result);
        if (!SafePaths.same(validated.ProjectFile, path) || validated.ProjectId != a_project.ProjectId ||
            validated.EngineVersion != a_project.EngineVersion)
            throw new InvalidOperationException("Project の情報が変わっています。一覧へ再登録してから変更してください");
        var updated = replace_engine_version(original, validated.EngineVersion, a_engine.Version);
        var changed = validated with { EngineVersion = a_engine.Version };
        var wasReplaced = false;
        try
        {
            // Hub の設定を排他取得してから File と登録情報を更新し、同じ Hub の操作との競合を避ける
            m_store.update(current =>
            {
                var index = current.Projects.FindIndex(item => SafePaths.same(item.ProjectFile, path));
                if (!current.Engines.Contains(a_engine) || index < 0 || current.Projects[index] != a_project)
                    throw new InvalidOperationException("Hub の登録情報が変わっています。選択し直してください");
                if (!read_project_bytes(path).SequenceEqual(original))
                    throw new IOException("Project が他の処理で変更されました。保存せずに中止します");
                a_cancel.ThrowIfCancellationRequested();
                changed = changed with { LastOpenedUtc = current.Projects[index].LastOpenedUtc };
                // 保存開始後は取消を挟まず、両方の更新または File の復旧を完了させる
                if (!original.SequenceEqual(updated))
                {
                    HubStateStore.atomic_write(path, updated);
                    wasReplaced = true;
                }
                current.Projects[index] = changed;
            });
        }
        catch (Exception error)
        {
            if (wasReplaced)
            {
                try
                {
                    if (!read_project_bytes(path).SequenceEqual(updated))
                        throw new IOException("保存後に Project が他の処理で変更されました");
                    HubStateStore.atomic_write(path, original);
                }
                catch (Exception rollback)
                {
                    throw new AggregateException("Engine 指定の復旧に失敗しました。.cueproj を確認して Hub へ再登録してください", error, rollback);
                }
            }
            throw;
        }
        return changed;
    }

    /// <summary>正本の UTF-8、BOM、改行、コメントを保ち、既存の engineVersion の値だけを差し替える</summary>
    private static byte[] replace_engine_version(byte[] a_original, string a_previous, string a_version)
    {
        if (a_previous.Length == 0)
            throw new InvalidDataException("この Project は Engine 未指定の旧形式です。形式変換はまだ対応していません");
        var utf8 = new UTF8Encoding(false, true);
        var text = utf8.GetString(a_original);
        for (var offset = 0; offset < text.Length;)
        {
            var end = text.IndexOf('\n', offset);
            if (end < 0) end = text.Length;
            var start = offset == 0 && text[0] == '\uFEFF' ? 1 : offset;
            var line = text[start..end];
            var equals = line.IndexOf('=');
            if (equals >= 0 && line[..equals].Trim(' ', '\t', '\r') == "engineVersion")
            {
                var valueStart = start + equals + 1;
                while (valueStart < end && " \t\r".Contains(text[valueStart])) ++valueStart;
                var valueEnd = end;
                while (valueEnd > valueStart && " \t\r".Contains(text[valueEnd - 1])) --valueEnd;
                if (text[valueStart..valueEnd] != a_previous) break;
                var updated = utf8.GetBytes(text[..valueStart] + a_version + text[valueEnd..]);
                if (updated.Length > 65536) throw new InvalidDataException("Project File が大きすぎます");
                return updated;
            }
            offset = end + 1;
        }
        throw new InvalidDataException("Project の Engine 指定を読み取れませんでした");
    }

    private static byte[] read_project_bytes(string a_path)
    {
        using var file = new FileStream(SafePaths.regular_file(a_path), FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length > 65536) throw new InvalidDataException("Project File が大きすぎます");
        var bytes = new byte[(int)file.Length];
        file.ReadExactly(bytes);
        return bytes;
    }

    private ProjectRegistration register_result(ProjectToolResult a_result, EngineRegistration a_engine, bool a_requireVersion)
    {
        if (a_requireVersion && a_result.EngineVersion.Length != 0 && a_result.EngineVersion != a_engine.Version)
            throw new InvalidOperationException($"Project は Engine {a_result.EngineVersion} を指定しています。対応版を選択してください");
        var registration = ProjectService.registration(a_result);
        var path = registration.ProjectFile;
        m_store.update(state =>
        {
            var existing = state.Projects.FindIndex(item => SafePaths.same(item.ProjectFile, path));
            if (existing >= 0) registration = registration with { LastOpenedUtc = state.Projects[existing].LastOpenedUtc };
            state.Projects.RemoveAll(item => SafePaths.same(item.ProjectFile, path));
            state.Projects.Add(registration);
        });
        return registration;
    }

    private static ProjectRegistration registration(ProjectToolResult a_result)
    {
        if (a_result.SchemaVersion != 1 || !a_result.Success || !Guid.TryParseExact(a_result.ProjectId, "D", out var id) || id == Guid.Empty || string.IsNullOrWhiteSpace(a_result.Name))
            throw new InvalidDataException("ProjectTool 応答が不正です");
        var path = SafePaths.regular_file(a_result.ProjectFile);
        if (!Path.GetExtension(path).Equals(".cueproj", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException(".cueproj を選択してください");
        return new ProjectRegistration { ProjectFile = path, ProjectId = a_result.ProjectId, Name = a_result.Name, EngineVersion = a_result.EngineVersion };
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
