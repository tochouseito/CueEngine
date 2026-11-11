using System.Windows;
using Velopack;

namespace Cue.Hub;

public partial class App : Application
{
    private string m_theme = "light";

    /// <summary>標準コントロールと Hub 固有の配色を同じ UI Thread で更新する</summary>
    internal void apply_theme(string a_theme)
    {
        if (m_theme == a_theme) return;
        var mode = a_theme == "dark" ? "Dark" : "Light";
        var controls = new ResourceDictionary
        {
            Source = new Uri($"pack://application:,,,/PresentationFramework.Fluent;component/Themes/Fluent.{mode}.xaml")
        };
        var palette = new ResourceDictionary
        {
            Source = new Uri($"/CueHub;component/Themes/Hub.{mode}.xaml", UriKind.Relative)
        };
        // App.xaml と同じ順序を保つと、既存の画面も DynamicResource の変更を受け取る
        Resources.MergedDictionaries[0] = controls;
        Resources.MergedDictionaries[1] = palette;
        m_theme = a_theme;
    }

    /// <summary>Updater の Hook は WPF と Window を生成する前に処理する</summary>
    [STAThread]
    public static int Main()
    {
        try
        {
            VelopackApp.Build().Run();
            var app = new App();
            app.InitializeComponent();
            return app.Run(new MainWindow());
        }
        catch (Exception error)
        {
            MessageBox.Show("Hub を開始できませんでした。\n" + error.Message, "CueEngine Hub", MessageBoxButton.OK, MessageBoxImage.Error);
            return 1;
        }
    }
}
