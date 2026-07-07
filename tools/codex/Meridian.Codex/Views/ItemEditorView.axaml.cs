using Avalonia.Controls;

namespace Meridian.Codex.Views;

/// <summary>
/// Code-behind for the item editor (#129). Resolved automatically for
/// <see cref="ViewModels.ItemEditorViewModel"/> by the shell's ViewLocator; all logic
/// lives in the VM.
/// </summary>
public partial class ItemEditorView : UserControl
{
    public ItemEditorView()
    {
        InitializeComponent();
    }
}
