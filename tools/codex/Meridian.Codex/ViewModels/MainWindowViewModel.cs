using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Meridian.Codex.Services;

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
        new EditorItem("Pack", "pack.yaml — namespace, versions, dependencies, and build status"),
        new EditorItem("NPCs", "npc.schema.yaml — creatures, vendors, quest givers"),
        new EditorItem("Items", "item.schema.yaml — equipment, consumables, quest items"),
        new EditorItem("Abilities", "ability.schema.yaml — spells and combat abilities"),
        new EditorItem("Quests", "quest.schema.yaml — objectives and rewards"),
    ];

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ActiveEditor))]
    private EditorItem? _selectedEditor;

    [ObservableProperty]
    private string _statusText = "Ready.";

    /// <summary>The NPC editor (#128), created once and shown when the NPCs rail entry is active.</summary>
    public NpcEditorViewModel NpcEditor { get; }

    /// <summary>The item editor (#129), created once and shown when the Items rail entry is active.</summary>
    public ItemEditorViewModel ItemEditor { get; }

    /// <summary>The pack-oriented workspace shell and manifest editor (#667).</summary>
    public PackWorkspaceViewModel PackWorkspace { get; }

    /// <summary>
    /// The editor VM to host in the main content area for the current selection, or
    /// null when the selected editor has no implementation yet (Abilities/Quests
    /// arrive later). The ViewLocator maps this VM to its View automatically.
    /// </summary>
    public object? ActiveEditor => SelectedEditor?.Name switch
    {
        "Pack" => PackWorkspace,
        "NPCs" => NpcEditor,
        "Items" => ItemEditor,
        _ => null,
    };

    public MainWindowViewModel() : this(HeadlessContentDialogService.Instance) { }

    public MainWindowViewModel(IContentDialogService dialogs)
    {
        PackWorkspace = new PackWorkspaceViewModel(new RecentWorkspaceStore(), dialogs);
        NpcEditor = new NpcEditorViewModel(dialogs, () => PackWorkspace.WorkspacePath);
        ItemEditor = new ItemEditorViewModel(dialogs, () => PackWorkspace.WorkspacePath);
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
