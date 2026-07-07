using CommunityToolkit.Mvvm.ComponentModel;

namespace Meridian.Codex.ViewModels;

/// <summary>
/// Base for every Codex ViewModel. Inherits <see cref="ObservableObject"/> from
/// CommunityToolkit.Mvvm so derived VMs get <c>[ObservableProperty]</c> and
/// <c>[RelayCommand]</c> source-generation. Per-editor VMs (SAD §6.1) derive from
/// this.
/// </summary>
public abstract class ViewModelBase : ObservableObject
{
}
