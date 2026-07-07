using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Meridian.Codex.ViewModels;

namespace Meridian.Codex;

/// <summary>
/// Resolves a View for a ViewModel by naming convention: a VM whose type is
/// <c>Meridian.Codex.ViewModels.FooViewModel</c> is rendered by
/// <c>Meridian.Codex.Views.FooView</c>. This is the seam the per-editor VMs
/// (NpcEditorViewModel/#128, ItemEditorViewModel/#129) plug into — add a matching
/// View and it is located automatically, no registration needed.
/// </summary>
public class ViewLocator : IDataTemplate
{
    public Control? Build(object? param)
    {
        if (param is null)
        {
            return null;
        }

        var name = param.GetType().FullName!.Replace("ViewModel", "View", System.StringComparison.Ordinal);
        var type = System.Type.GetType(name);

        if (type != null)
        {
            return (Control)System.Activator.CreateInstance(type)!;
        }

        return new TextBlock { Text = "Not Found: " + name };
    }

    public bool Match(object? data) => data is ViewModelBase;
}
