using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Meridian.Codex.ViewModels;

/// <summary>
/// ViewModel for the Codex shell window. M0 skeleton (#124): it presents the
/// editor navigation stubs that the NPC (#128) and Item (#129) editors will
/// replace with real per-editor VMs, and proves the generated content model
/// layer (<c>Meridian.Codex.Models</c>) is wired in by reporting the number of
/// generated content record types.
/// </summary>
public partial class MainWindowViewModel : ViewModelBase
{
    /// <summary>Window title shown in the OS title bar.</summary>
    public string Title => "Meridian Codex — Content Editor";

    /// <summary>Placeholder shell copy until real editor docks land.</summary>
    public string Greeting =>
        "Codex shell (M0 skeleton). Editor docks arrive with the NPC (#128) and Item (#129) editors.";

    /// <summary>Editor navigation entries — the shell's left rail. Stubs for now.</summary>
    public ObservableCollection<EditorItem> Editors { get; } =
    [
        new EditorItem("NPCs", "npc.schema.yaml — creatures, vendors, quest givers"),
        new EditorItem("Items", "item.schema.yaml — equipment, consumables, quest items"),
        new EditorItem("Abilities", "ability.schema.yaml — spells and combat abilities"),
        new EditorItem("Quests", "quest.schema.yaml — objectives and rewards"),
    ];

    [ObservableProperty]
    private EditorItem? _selectedEditor;

    [ObservableProperty]
    private string _statusText = "Ready.";

    public MainWindowViewModel()
    {
        SelectedEditor = Editors.FirstOrDefault();
    }

    /// <summary>
    /// Count of generated content record types (<c>Meridian.Codex.Models</c>),
    /// proving the schema-generated model layer is referenced and loadable. Uses a
    /// known model type as the assembly anchor.
    /// </summary>
    public int GeneratedModelTypeCount =>
        typeof(Meridian.Codex.Models.Item).Assembly
            .GetTypes()
            .Count(t => t.Namespace == "Meridian.Codex.Models"
                        && t.IsClass
                        && !t.IsAbstract);

    /// <summary>Demonstration command wired via CommunityToolkit source generation.</summary>
    [RelayCommand]
    private void ShowModelInfo()
    {
        StatusText = $"Loaded {GeneratedModelTypeCount} content model types from /schema/content.";
    }
}

/// <summary>A single entry in the editor navigation rail (skeleton placeholder).</summary>
public sealed record EditorItem(string Name, string Description);
