using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;

namespace Cue.Hub;

/// <summary>インストール済み一覧と管理メニューを表示し、操作の実体は ViewModel に委ねる</summary>
public partial class InstallationsView : UserControl
{
    public InstallationsView()
    {
        InitializeComponent();
    }

    private void on_add_engine(object a_sender, RoutedEventArgs a_event)
    {
        open_menu((Button)a_sender);
    }

    private void on_manage_engine(object a_sender, RoutedEventArgs a_event)
    {
        if (DataContext is not MainViewModel model || model.IsBusy) return;
        var button = (Button)a_sender;
        // 選択行ではなく、押された管理ボタンが属する Engine を操作対象とする
        model.SelectedEngine = (EngineRegistration)button.DataContext;
        open_menu(button);
    }

    private void open_menu(Button a_button)
    {
        a_button.ContextMenu.DataContext = DataContext;
        a_button.ContextMenu.PlacementTarget = a_button;
        a_button.ContextMenu.Placement = PlacementMode.Bottom;
        a_button.ContextMenu.IsOpen = true;
    }

    private void on_install_engine(object a_sender, RoutedEventArgs a_event)
    {
        if (DataContext is not MainViewModel model || model.IsBusy) return;
        if (!model.IncludePreview) model.CatalogChannel = 0;
        model.RefreshCatalog.Execute(null);
        new EngineInstallWindow(model) { Owner = Window.GetWindow(this) }.ShowDialog();
    }
}
