using Microsoft.Win32;
using System.Windows;

namespace Cue.Hub;

public interface IUserDialogs
{
    string? folder(string a_title);
    string? file(string a_title, string a_filter);
    bool confirm(string a_message);
}

/// <summary>OS の選択 UI を ViewModel と File 操作から分離する</summary>
public sealed class UserDialogs : IUserDialogs
{
    public string? folder(string a_title)
    {
        var dialog = new OpenFolderDialog { Title = a_title, Multiselect = false };
        return dialog.ShowDialog() == true ? dialog.FolderName : null;
    }
    public string? file(string a_title, string a_filter)
    {
        var dialog = new OpenFileDialog { Title = a_title, Filter = a_filter, CheckFileExists = true, Multiselect = false };
        return dialog.ShowDialog() == true ? dialog.FileName : null;
    }
    public bool confirm(string a_message) => MessageBox.Show(a_message, "CueEngine Hub", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes;
}
