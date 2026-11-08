using System.Windows;
using System.Windows.Controls;

namespace Cue.Hub;

/// <summary>公開版を選択する Modal。取得・導入の寿命は Hub 側の ViewModel が所有する</summary>
public partial class EngineInstallWindow : Window
{
    private readonly MainViewModel m_model;

    public EngineInstallWindow(MainViewModel a_model)
    {
        InitializeComponent();
        m_model = a_model;
        DataContext = a_model;
    }

    private void on_install_release(object a_sender, RoutedEventArgs a_event)
    {
        if (m_model.IsBusy) return;
        m_model.SelectedRelease = (PublishedRelease)((Button)a_sender).DataContext;
        if (!m_model.InstallEngine.CanExecute(null)) return;
        m_model.InstallEngine.Execute(null);
        Close();
    }
}
