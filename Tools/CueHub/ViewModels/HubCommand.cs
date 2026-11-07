using System.Windows.Input;

namespace Cue.Hub;

/// <summary>UI Thread の Command を所有し、再入判定と例外境界を ViewModel へ委ねる</summary>
public sealed class HubCommand(Func<Task> a_execute, Func<bool>? a_canExecute = null) : ICommand
{
    public event EventHandler? CanExecuteChanged;
    public bool CanExecute(object? parameter) => a_canExecute?.Invoke() ?? true;
    public async void Execute(object? parameter)
    {
        if (CanExecute(parameter)) await a_execute();
    }
    public void refresh() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
}
