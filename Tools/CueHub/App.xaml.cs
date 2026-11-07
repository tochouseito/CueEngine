using System.Windows;
using Velopack;

namespace Cue.Hub;

public partial class App : Application
{
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
