using System.ComponentModel;
using System.Windows;

namespace Cue.Hub;

public partial class MainWindow : Window
{
    private readonly MainViewModel m_viewModel;
    public MainWindow() : this(new MainViewModel(new HubStateStore(), new UserDialogs()))
    {
    }
    public MainWindow(MainViewModel a_viewModel)
    {
        m_viewModel = a_viewModel;
        apply_theme();
        InitializeComponent();
        DataContext = m_viewModel;
        m_viewModel.PropertyChanged += on_view_model_changed;
    }
    /// <summary>開いている各画面が参照するアプリ全体の配色を切り替える</summary>
    private void apply_theme() => (Application.Current as App)?.apply_theme(m_viewModel.Theme);

    private void on_view_model_changed(object? a_sender, PropertyChangedEventArgs a_event)
    {
        if (a_event.PropertyName == nameof(MainViewModel.Theme)) apply_theme();
    }

    private void on_closing(object? a_sender, CancelEventArgs a_event)
    {
        if (m_viewModel.IsBusy) { m_viewModel.Cancel.Execute(null); a_event.Cancel = true; return; }
        m_viewModel.PropertyChanged -= on_view_model_changed;
        m_viewModel.Dispose();
    }
}
