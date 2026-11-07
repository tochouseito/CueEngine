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
            var model = new MainViewModel(new HubStateStore(root), new NoDialogs());
            window = new MainWindow(model) { Left = -10000, Top = -10000, ShowInTaskbar = false };
            window.Show();
            Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            var tabs = find<TabControl>(window) ?? throw new InvalidOperationException("Hub の Tab がありません");
            if (tabs.Items.Count != 3) throw new InvalidOperationException("主要画面が不足しています");
            for (var index = 0; index < tabs.Items.Count; ++index)
            {
                tabs.SelectedIndex = index;
                window.UpdateLayout();
                Dispatcher.CurrentDispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            }
            if (model.CreateProject.CanExecute(null) || !model.RegisterEngine.CanExecute(null) || model.Cancel.CanExecute(null))
                throw new InvalidOperationException("起動直後の操作制限が不正です");
            // 未導入版は必要な版を案内し、一致する Engine の登録後だけ開ける
            var local = new EngineRegistration { Version = "0.1.0" };
            var required = new EngineRegistration { Version = "0.2.0" };
            model.Engines.Add(local);
            model.SelectedEngine = local;
            model.SelectedProject = new ProjectRegistration { EngineVersion = required.Version };
            if (model.OpenProject.CanExecute(null) || !model.Status.Contains(required.Version, StringComparison.Ordinal))
                throw new InvalidOperationException("未導入 Engine を指定した Project の案内が不足しています");
            model.Engines.Add(required);
            model.SelectedProject = new ProjectRegistration { EngineVersion = required.Version };
            if (model.SelectedEngine != required || !model.OpenProject.CanExecute(null))
                throw new InvalidOperationException("Project に一致する Engine が自動選択されません");
            if (trace.Errors.Count != 0) throw new InvalidOperationException(string.Join(Environment.NewLine, trace.Errors));
            window.Close(); window = null;
            application.Shutdown();
            Console.WriteLine("CueHub WPF startup, three tabs, bindings and initial commands passed");
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
