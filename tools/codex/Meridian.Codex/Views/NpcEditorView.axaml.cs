using Avalonia.Controls;

namespace Meridian.Codex.Views;

/// <summary>
/// Code-behind for the NPC editor (#128). Resolved automatically for
/// <see cref="ViewModels.NpcEditorViewModel"/> by the shell's ViewLocator; all logic
/// lives in the VM.
/// </summary>
public partial class NpcEditorView : UserControl
{
    public NpcEditorView()
    {
        InitializeComponent();
    }
}
