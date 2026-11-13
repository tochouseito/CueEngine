using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
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
            var dialogs = new NoDialogs();
            var model = new MainViewModel(store, dialogs);
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
            if (model.CreateProject.CanExecute(null) || !model.RegisterEngine.CanExecute(null) || model.Cancel.CanExecute(null) || model.ChangeProjectEngine.CanExecute(null))
                throw new InvalidOperationException("起動直後の操作制限が不正です");
            tabs.SelectedIndex = 0;
            window.UpdateLayout();
            var projects = (ListView)window.FindName("ProjectList");
            var add = (Button)window.FindName("AddProjectButton");
            var create = (Button)window.FindName("NewProjectButton");
            var changeEngine = (Button)window.FindName("ChangeProjectEngineButton");
            if (changeEngine.Command != model.ChangeProjectEngine)
                throw new InvalidOperationException("Engine バージョン変更ボタンが接続されていません");
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
            // 入力は親 Folder のままとし、表示する作成先だけを Project 名へ追従させる
            var initialParent = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            if (model.ProjectDirectory != initialParent) throw new InvalidOperationException("初期保存先に固定の Project 名が含まれています");
            var nameBox = (TextBox)creationPage.FindName("ProjectNameBox");
            var parentBox = (TextBox)creationPage.FindName("ProjectParentDirectoryBox");
            var destinationText = (TextBlock)creationPage.FindName("ProjectCreationPathText");
            nameBox.Text = "名前を変えたProject";
            parentBox.Text = Path.Combine(root, "ProjectParent");
            model.ApplicationName = "DifferentExecutable";
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            var expected = Path.Combine(parentBox.Text, nameBox.Text);
            if (model.ProjectCreationPath != expected || !destinationText.Text.Contains(expected, StringComparison.Ordinal))
                throw new InvalidOperationException("Project 名の変更が作成先へ反映されません");
            dialogs.SelectedFolder = Path.Combine(root, "ChosenParent");
            model.ChooseProjectDirectory.Execute(null);
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            expected = Path.Combine(dialogs.SelectedFolder, nameBox.Text);
            if (model.ProjectDirectory != dialogs.SelectedFolder || model.ProjectCreationPath != expected ||
                !destinationText.Text.Contains(expected, StringComparison.Ordinal))
                throw new InvalidOperationException("保存先の選択で Project 名以外の Folder を付加しています");
            nameBox.Text = "MyGame";
            parentBox.Text = initialParent;
            model.ApplicationName = "MyGame";
            dialogs.SelectedFolder = null;
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
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
            // 選択の取消では File 操作へ進まず、Project と他の Engine 選択を保つ
            var beforeProject = model.SelectedProject;
            var beforeStatus = model.Status;
            model.ChangeProjectEngine.Execute(null);
            if (model.SelectedProject != beforeProject || model.SelectedEngine != required || model.SelectedCreationEngine != local ||
                model.IsBusy || model.Status != beforeStatus || dialogs.EngineChoices?.Count != 2)
                throw new InvalidOperationException("Engine 変更の取消で状態が変化しました");
            test_project_engine_window(remembered, local, required);
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
            test_theme(window, tabs, model, store, dialogs);
            test_mouse_wheel(window, tabs, model);
            if (trace.Errors.Count != 0) throw new InvalidOperationException(string.Join(Environment.NewLine, trace.Errors));
            window.Close(); window = null;
            application.Shutdown();
            Console.WriteLine("CueHub WPF projects, creation, Engine management, install catalog, themes and mouse wheel passed");
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

    /// <summary>テーマの即時適用、独立した保存、再読込、ダイアログへの継承を検証する</summary>
    private static void test_theme(MainWindow a_window, TabControl a_tabs, MainViewModel a_model, HubStateStore a_store,
        IUserDialogs a_dialogs)
    {
        a_tabs.SelectedIndex = 2;
        a_window.UpdateLayout();
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        var selector = (ComboBox)a_window.FindName("ThemeSelector");
        var lightWindow = ((SolidColorBrush)a_window.Background).Color;
        var lightInput = ((SolidColorBrush)selector.Background).Color;
        if (!Equals(selector.SelectedValue, "light") || lightWindow != Color.FromRgb(243, 245, 248))
            throw new InvalidOperationException("既定のライトテーマが反映されません");

        var originalDirectory = a_store.load().EngineDirectory;
        a_model.EngineDirectory = "unsaved-directory";
        selector.SelectedValue = "dark";
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        var darkWindow = ((SolidColorBrush)a_window.Background).Color;
        var darkInput = ((SolidColorBrush)selector.Background).Color;
        var darkText = ((SolidColorBrush)a_window.Foreground).Color;
        var saved = a_store.load();
        if (darkWindow != Color.FromRgb(21, 27, 36) || darkText != Color.FromRgb(238, 243, 250) ||
            darkInput == lightInput || saved.Theme != "dark" || saved.EngineDirectory != originalDirectory ||
            a_model.EngineDirectory != "unsaved-directory")
            throw new InvalidOperationException("ダークテーマの即時適用または独立した保存が不正です");

        using var restored = new MainViewModel(a_store, a_dialogs);
        if (restored.Theme != "dark") throw new InvalidOperationException("保存済みテーマが復元されません");
        var dialog = new EngineInstallWindow(a_model);
        if (((SolidColorBrush)dialog.Background).Color != darkWindow)
            throw new InvalidOperationException("インストール画面がテーマを継承しません");
        dialog.Close();

        // 保存できないときは適用済みテーマと選択を保ち、ロック解放後に同じ変更を再試行できる
        using (var held = new FileStream(Path.Combine(a_store.DirectoryPath, "state.lock"), FileMode.Open,
                   FileAccess.ReadWrite, FileShare.None))
        {
            selector.SelectedValue = "light";
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            if (((SolidColorBrush)a_window.Background).Color != darkWindow || a_model.Theme != "dark" ||
                !Equals(selector.SelectedValue, "dark") || !a_model.Status.Contains("保存できません", StringComparison.Ordinal))
                throw new InvalidOperationException("テーマの保存失敗時に元の選択が保たれません");
        }
        selector.SelectedValue = "light";
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        if (((SolidColorBrush)a_window.Background).Color != lightWindow ||
            ((SolidColorBrush)selector.Background).Color != lightInput || a_store.load().Theme != "light")
            throw new InvalidOperationException("ライトテーマへの切り替えが反映されません");
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

    /// <summary>長い一覧と狭い設定画面で、子コントロール上のホイール操作が実際に一覧を動かすことを確認する</summary>
    private static void test_mouse_wheel(MainWindow a_window, TabControl a_tabs, MainViewModel a_model)
    {
        a_window.Height = a_window.MinHeight;
        for (var index = 0; index < 48; ++index)
        {
            a_model.Projects.Add(new ProjectRegistration { Name = $"Scroll project {index}" });
            a_model.Engines.Add(new EngineRegistration { Version = $"1.0.{index}" });
        }

        a_tabs.SelectedIndex = 0;
        a_window.UpdateLayout();
        var projects = (ListView)a_window.FindName("ProjectList");
        verify_mouse_wheel(projects, (ListViewItem)projects.ItemContainerGenerator.ContainerFromIndex(0), "プロジェクト一覧");

        a_tabs.SelectedIndex = 1;
        a_window.UpdateLayout();
        var installations = (InstallationsView)a_window.FindName("InstallationsPage");
        var installed = (ListBox)installations.FindName("InstalledEngineList");
        var card = (ListBoxItem)installed.ItemContainerGenerator.ContainerFromIndex(0);
        verify_mouse_wheel(installed, (UIElement?)find<Button>(card) ?? card, "インストール済み Engine 一覧");

        a_tabs.SelectedIndex = 2;
        a_window.UpdateLayout();
        var settings = (ScrollViewer)((TabItem)a_tabs.Items[2]).Content;
        var field = find<TextBox>(settings) ?? throw new InvalidOperationException("設定の入力欄がありません");
        verify_mouse_wheel(settings, field, "設定");
    }

    /// <summary>WPF のルーティングを通して上下のホイールイベントを送り、表示位置を検証する</summary>
    private static void verify_mouse_wheel(DependencyObject a_root, UIElement a_target, string a_page)
    {
        var scroll = find<ScrollViewer>(a_root) ?? throw new InvalidOperationException($"{a_page}にスクロール領域がありません");
        if (scroll.ScrollableHeight <= 0) throw new InvalidOperationException($"{a_page}にスクロール範囲がありません");
        a_target.RaiseEvent(new MouseWheelEventArgs(Mouse.PrimaryDevice, 0, -120) { RoutedEvent = Mouse.MouseWheelEvent });
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        if (scroll.VerticalOffset <= 0) throw new InvalidOperationException($"{a_page}をホイールで下に移動できません");
        a_target.RaiseEvent(new MouseWheelEventArgs(Mouse.PrimaryDevice, 0, 120) { RoutedEvent = Mouse.MouseWheelEvent });
        Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
        if (scroll.VerticalOffset != 0) throw new InvalidOperationException($"{a_page}をホイールで上に戻せません");
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

    private static void test_project_engine_window(ProjectRegistration a_project, EngineRegistration a_other, EngineRegistration a_current)
    {
        var dialog = new ProjectEngineWindow(a_project, [a_other, a_current])
        { Left = -10000, Top = -10000, WindowStartupLocation = WindowStartupLocation.Manual, ShowInTaskbar = false };
        try
        {
            dialog.Show();
            dialog.UpdateLayout();
            var selector = (ComboBox)dialog.FindName("ProjectEngineSelector");
            var apply = (Button)dialog.FindName("ApplyEngineButton");
            if (selector.Items.Count != 2 || selector.DisplayMemberPath != "Display" || dialog.SelectedEngine != a_current || apply.IsEnabled)
                throw new InvalidOperationException("変更先の候補または現在の Engine 表示が不正です");
            selector.SelectedItem = a_other;
            if (dialog.SelectedEngine != a_other || !apply.IsEnabled)
                throw new InvalidOperationException("異なる Engine を変更先に選べません");
            selector.SelectedItem = null;
            if (apply.IsEnabled) throw new InvalidOperationException("Engine 未選択でも変更できます");
        }
        finally { dialog.Close(); }
    }

    private sealed class NoDialogs : IUserDialogs
    {
        public string? SelectedFolder { get; set; }
        public IReadOnlyList<EngineRegistration>? EngineChoices { get; private set; }
        public string? folder(string a_title) => SelectedFolder;
        public string? file(string a_title, string a_filter) => null;
        public bool confirm(string a_message) => false;
        public EngineRegistration? project_engine(ProjectRegistration a_project, IReadOnlyList<EngineRegistration> a_engines)
        {
            EngineChoices = a_engines;
            return null;
        }
    }

    private sealed class BindingErrors : TraceListener
    {
        public List<string> Errors { get; } = [];
        public override void Write(string? message) { if (!string.IsNullOrWhiteSpace(message)) Errors.Add(message); }
        public override void WriteLine(string? message) => Write(message);
    }
}
