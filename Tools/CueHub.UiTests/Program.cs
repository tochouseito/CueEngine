using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace Cue.Hub.UiTests;

internal static class Program
{
    [STAThread]
    private static int Main()
    {
        var root = Path.Combine(Path.GetTempPath(), "CueHubUiTests-" + Guid.NewGuid().ToString("N"));
        var trace = new BindingErrors();
        MainWindow? window = null;
        try
        {
            PresentationTraceSources.DataBindingSource.Listeners.Add(trace);
            PresentationTraceSources.DataBindingSource.Switch.Level = SourceLevels.Error;
            var application = new App();
            application.InitializeComponent();
            var store = new HubStateStore(root);
            // Hub の保存済み履歴は Engine 未導入でも一覧に復元する
            var remembered = new ProjectRegistration { Name = "RememberedProject", ProjectFile = Path.Combine(root, "Game.cueproj"), EngineVersion = "0.2.0" };
            store.update(state => state.Projects.Add(remembered));
            var model = new MainViewModel(store, new NoDialogs());
            window = new MainWindow(model) { Left = -10000, Top = -10000, ShowInTaskbar = false };
            window.Show();
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            var tabs = find<TabControl>(window) ?? throw new InvalidOperationException("Hub の Tab がありません");
            if (tabs.Items.Count != 3) throw new InvalidOperationException("主要画面が不足しています");
            if (tabs.TabStripPlacement != Dock.Left)
                throw new InvalidOperationException("Hub のナビゲーションが左側にありません");
            var headers = new[] { "プロジェクト", "インストール", "設定" };
            for (var index = 0; index < tabs.Items.Count; ++index)
            {
                var item = (TabItem)tabs.Items[index];
                if (!Equals(item.Header, headers[index]))
                    throw new InvalidOperationException("Hub のナビゲーション項目が不正です");
                tabs.SelectedIndex = index;
                window.UpdateLayout();
                Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
                var content = tabs.Template.FindName("PART_SelectedContentHost", tabs) as ContentPresenter
                    ?? throw new InvalidOperationException("Hub の選択画面がありません");
                if (content.ActualWidth <= 0 || content.TranslatePoint(new Point(), window).X <=
                    item.TranslatePoint(new Point(), window).X + item.ActualWidth)
                    throw new InvalidOperationException("Hub のナビゲーションと選択画面が重なっています");
            }
            if (model.CreateProject.CanExecute(null) || !model.RegisterEngine.CanExecute(null) || model.Cancel.CanExecute(null))
                throw new InvalidOperationException("起動直後の操作制限が不正です");
            tabs.SelectedIndex = 0;
            window.UpdateLayout();
            var projects = (ListView)window.FindName("ProjectList");
            var add = (Button)window.FindName("AddProjectButton");
            var create = (Button)window.FindName("NewProjectButton");
            var listPage = (Grid)window.FindName("ProjectListPage");
            var creationPage = (ProjectCreationView)window.FindName("ProjectCreationPage");
            var engineSelector = (ComboBox)creationPage.FindName("CreationEngineSelector");
            if (projects.Items.Count != 1 || !Equals(projects.Items[0], remembered) ||
                !Equals(add.Content, "追加") || !Equals(create.Content, "新しいプロジェクト") ||
                add.Command != model.RegisterProject || create.Command != model.ShowProjectCreation ||
                add.TranslatePoint(new Point(), window).X <= projects.TranslatePoint(new Point(), window).X ||
                create.TranslatePoint(new Point(), window).Y >= projects.TranslatePoint(new Point(), window).Y ||
                creationPage.Visibility != Visibility.Collapsed || listPage.Visibility != Visibility.Visible ||
                find<ComboBox>(listPage) != null)
                throw new InvalidOperationException("保存済み Project 一覧または右上の操作ボタンが不正です");
            create.Command.Execute(null);
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            if (creationPage.Visibility != Visibility.Visible || listPage.Visibility != Visibility.Collapsed || model.IsBusy)
                throw new InvalidOperationException("新しい Project の作成ページへ切り替わりません");
            model.HideProjectCreation.Execute(null);
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            if (creationPage.Visibility != Visibility.Collapsed || listPage.Visibility != Visibility.Visible ||
                projects.Items.Count != 1 || store.load().Projects.Count != 1)
                throw new InvalidOperationException("作成ページから一覧へ戻るときに Project 履歴が変化しました");
            // 未導入版は必要な版を案内し、一致する Engine の登録後だけ開ける
            var local = new EngineRegistration { Version = "0.1.0" };
            var required = new EngineRegistration { Version = "0.2.0" };
            model.Engines.Add(local);
            model.SelectedEngine = local;
            if (model.CreateProject.CanExecute(null))
                throw new InvalidOperationException("作成ページで Engine 未選択でも Project を作成できます");
            engineSelector.SelectedItem = local;
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            if (model.SelectedCreationEngine != local || !model.CreateProject.CanExecute(null))
                throw new InvalidOperationException("作成ページの Engine 選択が反映されません");
            model.SelectedProject = new ProjectRegistration { EngineVersion = required.Version };
            if (model.OpenProject.CanExecute(null) || !model.Status.Contains(required.Version, StringComparison.Ordinal))
                throw new InvalidOperationException("未導入 Engine を指定した Project の案内が不足しています");
            model.Engines.Add(required);
            model.SelectedProject = new ProjectRegistration { EngineVersion = required.Version };
            if (model.SelectedEngine != required || !model.OpenProject.CanExecute(null) || model.SelectedCreationEngine != local)
                throw new InvalidOperationException("Project に一致する Engine が自動選択されません");
            // 作成失敗時は作成ページに留まり、入力と Engine の選択を保つ
            model.ShowProjectCreation.Execute(null);
            var previousDirectory = model.ProjectDirectory;
            model.ProjectDirectory = "relative-project";
            model.CreateProject.Execute(null);
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            if (!model.IsProjectCreationOpen || !model.Status.StartsWith("失敗:", StringComparison.Ordinal) ||
                model.SelectedCreationEngine != local || model.ProjectName != "MyGame" ||
                model.ProjectDirectory != "relative-project" || model.IsBusy)
                throw new InvalidOperationException("作成失敗時に入力が保持されません");
            model.ProjectDirectory = previousDirectory;
            model.HideProjectCreation.Execute(null);
            test_installations(window, tabs, model, local);
            if (trace.Errors.Count != 0) throw new InvalidOperationException(string.Join(Environment.NewLine, trace.Errors));
            window.Close(); window = null;
            application.Shutdown();
            Console.WriteLine("CueHub WPF projects, creation, installed Engine cards, filters, management menus and install catalog passed");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        finally
        {
            window?.Close();
            PresentationTraceSources.DataBindingSource.Listeners.Remove(trace);
            if (Directory.Exists(root)) SafePaths.delete_owned_tree(root, Path.GetTempPath());
        }
    }

    private static void test_installations(MainWindow a_window, TabControl a_tabs, MainViewModel a_model, EngineRegistration a_local)
    {
        a_tabs.SelectedIndex = 1;
        var page = (InstallationsView)a_window.FindName("InstallationsPage");
        var installed = (ListBox)page.FindName("InstalledEngineList");
        var search = (TextBox)page.FindName("EngineSearchBox");
        var channels = (TabControl)page.FindName("InstalledChannelTabs");
        var preview = new EngineRegistration { Version = "0.3.0-preview.1" };
        a_model.Engines.Add(preview);
        a_window.UpdateLayout();
        if (installed.Items.Count != 3 || a_model.SelectedCreationEngine != a_local)
            throw new InvalidOperationException("インストール済み一覧が作成用 Engine 候補と分離されていません");
        channels.SelectedIndex = 1;
        a_window.UpdateLayout();
        if (installed.Items.Count != 2) throw new InvalidOperationException("正式リリースの絞り込みが不正です");
        channels.SelectedIndex = 2;
        a_window.UpdateLayout();
        if (installed.Items.Count != 1 || !Equals(installed.Items[0], preview))
            throw new InvalidOperationException("Preview の絞り込みが不正です");
        channels.SelectedIndex = 0;
        search.Text = "0.1.0";
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        a_window.UpdateLayout();
        if (installed.Items.Count != 1 || a_model.Engines.Count != 3 || a_model.SelectedCreationEngine != a_local)
            throw new InvalidOperationException("Engine 検索が他の Engine 選択に影響しています");
        var card = (ListBoxItem)installed.ItemContainerGenerator.ContainerFromIndex(0);
        var manage = find<Button>(card) ?? throw new InvalidOperationException("Engine の管理ボタンがありません");
        manage.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        if (a_model.SelectedEngine != a_local || manage.ContextMenu.DataContext != a_model ||
            ((MenuItem)manage.ContextMenu.Items[0]).Command != a_model.ShowEngineFolder ||
            ((MenuItem)manage.ContextMenu.Items[1]).Command != a_model.RepairEngine ||
            ((MenuItem)manage.ContextMenu.Items[3]).Command != a_model.RemoveEngine)
            throw new InvalidOperationException("管理メニューが押された Engine を操作対象にしていません");
        manage.ContextMenu.IsOpen = false;
        search.Text = "";
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        a_model.Releases.Add(new PublishedRelease { Version = "0.2.0" });
        a_model.Releases.Add(new PublishedRelease { Version = preview.Version, IsPreview = true });
        var dialog = new EngineInstallWindow(a_model) { Owner = a_window, Left = -10000, Top = -10000, WindowStartupLocation = WindowStartupLocation.Manual, ShowInTaskbar = false };
        try
        {
            // 公開 API を呼ばず、版の選択と表示 Binding だけを検証する
            dialog.Show();
            dialog.UpdateLayout();
            var releases = (ListBox)dialog.FindName("AvailableReleaseList");
            var catalogChannels = (TabControl)dialog.FindName("CatalogChannelTabs");
            if (releases.Items.Count != 1 || ((PublishedRelease)releases.Items[0]).IsPreview)
                throw new InvalidOperationException("正式リリースのインストール候補が不正です");
            catalogChannels.SelectedIndex = 1;
            dialog.UpdateLayout();
            if (releases.Items.Count != 1 || !((PublishedRelease)releases.Items[0]).IsPreview)
                throw new InvalidOperationException("Preview のインストール候補が不正です");
            releases.SelectedIndex = 0;
            if (a_model.SelectedRelease?.Version != preview.Version || !a_model.InstallEngine.CanExecute(null))
                throw new InvalidOperationException("選択した版をインストール対象にできません");
        }
        finally { dialog.Close(); }
    }

    private static T? find<T>(DependencyObject a_parent) where T : DependencyObject
    {
        if (a_parent is T result) return result;
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(a_parent); ++index)
        {
            var found = find<T>(VisualTreeHelper.GetChild(a_parent, index));
            if (found != null) return found;
        }
        return null;
    }

    private sealed class NoDialogs : IUserDialogs
    {
        public string? folder(string a_title) => null;
        public string? file(string a_title, string a_filter) => null;
        public bool confirm(string a_message) => false;
    }

    private sealed class BindingErrors : TraceListener
    {
        public List<string> Errors { get; } = [];
        public override void Write(string? message) { if (!string.IsNullOrWhiteSpace(message)) Errors.Add(message); }
        public override void WriteLine(string? message) => Write(message);
    }
}
