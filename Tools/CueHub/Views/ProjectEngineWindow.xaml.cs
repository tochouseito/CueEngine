using System.Windows;
using System.Windows.Controls;

namespace Cue.Hub;

/// <summary>導入済み Engine の選択だけを返す。File 更新と取消・失敗の処理は呼出元が所有する</summary>
public partial class ProjectEngineWindow : Window
{
    private readonly string m_currentVersion;
    public EngineRegistration? SelectedEngine => ProjectEngineSelector.SelectedItem as EngineRegistration;

    public ProjectEngineWindow(ProjectRegistration a_project, IReadOnlyList<EngineRegistration> a_engines)
    {
        m_currentVersion = a_project.EngineVersion;
        InitializeComponent();
        ProjectNameText.Text = a_project.Name;
        CurrentEngineText.Text = "現在の Engine: " + (m_currentVersion.Length == 0 ? "未指定（旧形式）" : m_currentVersion);
        ProjectEngineSelector.ItemsSource = a_engines;
        ProjectEngineSelector.SelectedItem = a_engines.FirstOrDefault(item => item.Version == m_currentVersion);
    }

    private void on_engine_selected(object a_sender, SelectionChangedEventArgs a_event)
    {
        if (ApplyEngineButton != null)
            ApplyEngineButton.IsEnabled = SelectedEngine != null && SelectedEngine.Version != m_currentVersion;
    }

    private void on_apply(object a_sender, RoutedEventArgs a_event)
    {
        if (SelectedEngine != null && SelectedEngine.Version != m_currentVersion) DialogResult = true;
    }
}
