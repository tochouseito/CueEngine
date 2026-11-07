using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Cue.Hub;

/// <summary>画面状態を UI Thread で所有し、長い処理は Service の非同期 API に渡す</summary>
public sealed class MainViewModel : INotifyPropertyChanged, IDisposable
{
    private readonly HubStateStore m_store;
    private readonly IUserDialogs m_dialogs;
    private readonly HttpClient m_http = new() { Timeout = TimeSpan.FromMinutes(30) };
    private readonly List<HubCommand> m_commands = [];
    private CancellationTokenSource? m_operation;
    private EngineRegistration? m_selectedEngine;
    private ProjectRegistration? m_selectedProject;
    private PublishedRelease? m_selectedRelease;
    private string m_status = "ローカル Engine を登録するか、公開版の一覧を取得してください";
    private double m_progress;
    private bool m_isBusy;
    private string m_engineDirectory = "";
    private string m_keyFile = "";
    private string m_localFeed = "";
    private bool m_includePreview;
    public event PropertyChangedEventHandler? PropertyChanged;
    public string Version => Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0] ?? "development";
    public ObservableCollection<EngineRegistration> Engines { get; } = [];
    public ObservableCollection<ProjectRegistration> Projects { get; } = [];
    public ObservableCollection<PublishedRelease> Releases { get; } = [];
    public string[] Templates { get; } = ["Empty3D", "RotateCube"];
    public string Template { get; set; } = "Empty3D";
    public string ProjectName { get; set; } = "MyGame";
    public string ApplicationName { get; set; } = "MyGame";
    private string m_projectDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "CueProjects", "MyGame");
    public string ProjectDirectory { get => m_projectDirectory; set => set(ref m_projectDirectory, value); }
    public EngineRegistration? SelectedEngine { get => m_selectedEngine; set { set(ref m_selectedEngine, value); refresh_commands(); } }
    public ProjectRegistration? SelectedProject
    {
        get => m_selectedProject;
        set
        {
            set(ref m_selectedProject, value);
            if (value?.EngineVersion.Length > 0 && SelectedEngine?.Version != value.EngineVersion)
            {
                var compatible = Engines.FirstOrDefault(item => item.Version == value.EngineVersion);
                if (compatible != null) SelectedEngine = compatible;
                else Status = $"この Project が指定する Engine {value.EngineVersion} を導入・登録してください";
            }
            refresh_commands();
        }
    }
    public PublishedRelease? SelectedRelease { get => m_selectedRelease; set { set(ref m_selectedRelease, value); refresh_commands(); } }
    public string Status { get => m_status; private set => set(ref m_status, value); }
    public double Progress { get => m_progress; private set => set(ref m_progress, value); }
    public bool IsBusy { get => m_isBusy; private set { set(ref m_isBusy, value); refresh_commands(); } }
    public string EngineDirectory { get => m_engineDirectory; set => set(ref m_engineDirectory, value); }
    public string KeyFile { get => m_keyFile; set => set(ref m_keyFile, value); }
    public string LocalFeed { get => m_localFeed; set => set(ref m_localFeed, value); }
    public bool IncludePreview { get => m_includePreview; set => set(ref m_includePreview, value); }
    public HubCommand RegisterEngine { get; }
    public HubCommand RegisterProject { get; }
    public HubCommand CreateProject { get; }
    public HubCommand OpenProject { get; }
    public HubCommand RemoveProject { get; }
    public HubCommand RefreshCatalog { get; }
    public HubCommand InstallEngine { get; }
    public HubCommand ImportEngine { get; }
    public HubCommand RepairEngine { get; }
    public HubCommand RemoveEngine { get; }
    public HubCommand SaveSettings { get; }
    public HubCommand ChooseEngineDirectory { get; }
    public HubCommand ChooseProjectDirectory { get; }
    public HubCommand ChooseKeyFile { get; }
    public HubCommand ChooseLocalFeed { get; }
    public HubCommand UpdateHub { get; }
    public HubCommand OpenArchive { get; }
    public HubCommand Cancel { get; }

    public MainViewModel(HubStateStore a_store, IUserDialogs a_dialogs)
    {
        m_store = a_store;
        m_dialogs = a_dialogs;
        RegisterEngine = command(async _ =>
        {
            var folder = m_dialogs.folder("CueEngine のルートフォルダを選択");
            if (folder != null) { new EngineRegistryService(m_store).register_local(folder); reload(); Status = "ローカル Engine を登録しました"; }
            await Task.CompletedTask;
        });
        RegisterProject = command(async token =>
        {
            var file = m_dialogs.file("Project を登録", "CueEngine Project|*.cueproj");
            if (file != null) { await new ProjectService(m_store).register_async(require_engine(), file, token); reload(); Status = "Project を登録しました"; }
        }, () => SelectedEngine != null);
        CreateProject = command(async token =>
        {
            await new ProjectService(m_store).create_async(require_engine(), ProjectDirectory, ProjectName, ApplicationName, Template, token);
            reload(); Status = "Project を作成しました。一覧から Editor で開けます";
        }, () => SelectedEngine != null);
        OpenProject = command(async token =>
        {
            var pid = await new ProjectService(m_store).open_async(require_engine(), SelectedProject!, token);
            reload(); Status = "Editor を起動しました (PID " + pid + ")";
        }, () => SelectedEngine != null && SelectedProject != null && (SelectedProject.EngineVersion.Length == 0 || SelectedProject.EngineVersion == SelectedEngine.Version));
        RemoveProject = command(async _ => { new ProjectService(m_store).unregister(SelectedProject!); reload(); Status = "一覧から登録を外しました。Project のファイルは保持しています"; await Task.CompletedTask; }, () => SelectedProject != null);
        RefreshCatalog = command(async token =>
        {
            var service = new EngineCatalogService(m_http, m_store);
            var result = await service.refresh_async(token);
            Releases.Clear();
            foreach (var item in result.Where(item => IncludePreview || !item.IsPreview)) Releases.Add(item);
            Status = service.LastWarning ?? $"公開 Engine {Releases.Count} 件を取得しました（正式版・Preview 合計最大 10 世代）";
        });
        InstallEngine = command(async token =>
        {
            var service = installer(); var release = SelectedRelease!; var report = progress();
            await Task.Run(() => service.install_release_async(release, report, token), token); reload();
        }, () => SelectedRelease != null);
        ImportEngine = command(async token =>
        {
            var zip = m_dialogs.file("Engine パッケージを選択", "Engine Package|*.zip");
            if (zip == null) return;
            var directory = Path.GetDirectoryName(zip)!;
            var service = installer(); var report = progress();
            var manifest = read_small_file(Path.Combine(directory, "engine-release.json"), 65536);
            var signature = System.Text.Encoding.ASCII.GetString(read_small_file(Path.Combine(directory, "engine-release.json.sig"), 4096));
            var identity = verifier().verify(manifest, signature);
            var repair = m_store.load().Engines.FirstOrDefault(item => item.IsManaged && item.Version == identity.Version);
            if (repair != null && !m_dialogs.confirm($"Engine {repair.Version} を同じ配布パッケージで修復しますか？")) return;
            await Task.Run(() => service.install_local_async(zip, manifest, signature, report, token, repair), token);
            reload();
        });
        RepairEngine = command(async token =>
        {
            var selected = SelectedEngine!;
            var release = Releases.FirstOrDefault(item => item.Version == selected.Version) ?? throw new InvalidOperationException("同じ Version の一覧を取得してください。旧版は同じパッケージを手動で取得できます");
            var service = installer(); var report = progress();
            await Task.Run(() => service.install_release_async(release, report, token, selected), token);
            reload();
        }, () => SelectedEngine?.IsManaged == true);
        RemoveEngine = command(async token =>
        {
            var selected = SelectedEngine!;
            if (!selected.IsManaged) new EngineRegistryService(m_store).unregister_local(selected);
            else
            {
                var references = m_store.load().Projects.Count(item => item.EngineVersion == selected.Version);
                if (!m_dialogs.confirm($"Engine {selected.Version} をアンインストールしますか？\n登録済み Project {references} 件がこの Version を指定しています。Project のファイルは削除しません。")) return;
                var service = installer();
                await Task.Run(() => service.uninstall(selected), token);
            }
            reload(); Status = "Engine の登録を外しました"; await Task.CompletedTask;
        }, () => SelectedEngine != null);
        SaveSettings = command(async _ =>
        {
            var path = SafePaths.full(EngineDirectory);
            SafePaths.no_links(path);
            var state = m_store.load();
            if (state.Engines.Any(item => item.IsManaged) && !SafePaths.same(state.EngineDirectory, path)) throw new InvalidOperationException("導入版 Engine を削除してから保存先を変更してください");
            if (File.Exists(KeyFile)) verifier();
            if (LocalFeed.Length > 0) SafePaths.no_links(SafePaths.full(LocalFeed));
            m_store.update(current => { current.EngineDirectory = path; current.TrustedPublisherKeyFile = KeyFile; current.LocalHubFeed = LocalFeed; current.IncludePreview = IncludePreview; });
            Status = "設定を保存しました"; await Task.CompletedTask;
        });
        ChooseEngineDirectory = command(async _ => { var folder = m_dialogs.folder("Engine 保存先"); if (folder != null) EngineDirectory = folder; await Task.CompletedTask; });
        ChooseProjectDirectory = command(async _ => { var folder = m_dialogs.folder("新規 Project の親フォルダ"); if (folder != null) ProjectDirectory = Path.Combine(folder, ApplicationName); await Task.CompletedTask; });
        ChooseKeyFile = command(async _ => { var file = m_dialogs.file("信頼する発行者の公開鍵を選択", "PEM Public Key|*.pem"); if (file != null) KeyFile = file; await Task.CompletedTask; });
        ChooseLocalFeed = command(async _ => { var folder = m_dialogs.folder("署名済み Hub 更新 Feed"); if (folder != null) LocalFeed = folder; await Task.CompletedTask; });
        UpdateHub = command(async token =>
        {
            var updater = new HubUpdateService(m_http, m_store, verifier());
            var offer = LocalFeed.Length > 0 ? await updater.prepare_local_async(LocalFeed, progress(), token) : await updater.prepare_github_async(IncludePreview, progress(), token);
            if (offer == null) { Status = "新しい Hub 更新はありません"; return; }
            if (m_dialogs.confirm($"Hub {offer.Manifest.Version} を準備しました。Hub を終了して更新・再起動しますか？")) await updater.apply_and_restart_async(offer, token);
            else Status = "Hub 更新の適用を取り消しました";
        });
        OpenArchive = command(async _ => { Process.Start(new ProcessStartInfo(EngineCatalogService.RepositoryUrl + "/releases") { UseShellExecute = true }); await Task.CompletedTask; });
        Cancel = new HubCommand(() => { m_operation?.Cancel(); return Task.CompletedTask; }, () => IsBusy);
        m_commands.Add(Cancel);
        reload();
    }

    private EngineRegistration require_engine() => SelectedEngine ?? throw new InvalidOperationException("Engine を選択してください");
    private PackageVerifier verifier() => new(System.Text.Encoding.UTF8.GetString(read_small_file(KeyFile, 16384)));
    private static byte[] read_small_file(string a_file, int a_limit)
    {
        var file = SafePaths.regular_file(a_file);
        if (new FileInfo(file).Length > a_limit) throw new InvalidDataException("ファイルが大きすぎます");
        return File.ReadAllBytes(file);
    }
    private EngineInstallService installer() => new(m_store, m_http, verifier());
    private IProgress<InstallProgress> progress() => new Progress<InstallProgress>(item => { Status = item.Message; if (item.Percent.HasValue) Progress = item.Percent.Value; });

    private HubCommand command(Func<CancellationToken, Task> a_action, Func<bool>? a_condition = null)
    {
        var command = new HubCommand(() => run_async(a_action), () => !IsBusy && (a_condition?.Invoke() ?? true));
        m_commands.Add(command);
        return command;
    }

    private async Task run_async(Func<CancellationToken, Task> a_action)
    {
        if (IsBusy) return;
        IsBusy = true; Progress = 0;
        using var cancel = new CancellationTokenSource();
        m_operation = cancel;
        try { await a_action(cancel.Token); }
        catch (OperationCanceledException) { Status = "操作を取り消しました"; }
        catch (Exception error) { Status = "失敗: " + error.Message; }
        finally { m_operation = null; IsBusy = false; }
    }

    private void reload()
    {
        var selectedId = SelectedEngine?.Id;
        var selectedFile = SelectedProject?.ProjectFile;
        var state = m_store.load();
        EngineDirectory = state.EngineDirectory; KeyFile = state.TrustedPublisherKeyFile; LocalFeed = state.LocalHubFeed; IncludePreview = state.IncludePreview;
        Engines.Clear(); foreach (var item in state.Engines) Engines.Add(item);
        Projects.Clear(); foreach (var item in state.Projects.OrderByDescending(item => item.LastOpenedUtc)) Projects.Add(item);
        SelectedEngine = Engines.FirstOrDefault(item => item.Id == selectedId) ?? Engines.FirstOrDefault();
        SelectedProject = Projects.FirstOrDefault(item => item.ProjectFile == selectedFile) ?? Projects.FirstOrDefault();
    }

    private void refresh_commands() { foreach (var command in m_commands) command.refresh(); }
    private void set<T>(ref T a_field, T a_value, [CallerMemberName] string? a_name = null)
    {
        if (EqualityComparer<T>.Default.Equals(a_field, a_value)) return;
        a_field = a_value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(a_name));
    }
    public void Dispose() { m_operation?.Cancel(); m_http.Dispose(); }
}
