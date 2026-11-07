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
        InitializeComponent();
        m_viewModel = a_viewModel;
        DataContext = m_viewModel;
    }
    private void on_closing(object? a_sender, CancelEventArgs a_event)
    {
        if (m_viewModel?.IsBusy == true) { m_viewModel.Cancel.Execute(null); a_event.Cancel = true; return; }
        m_viewModel?.Dispose();
    }
}
