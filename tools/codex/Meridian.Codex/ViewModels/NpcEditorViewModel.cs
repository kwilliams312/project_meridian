using System;
using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Meridian.Codex.Editing;
using Meridian.Codex.Services;

namespace Meridian.Codex.ViewModels;

/// <summary>
/// The NPC/Mob editor (TLS-03, issue #128). A schema-driven form over an
/// <see cref="NpcDocument"/> with fields grouped per Tools PRD §4.1 —
/// identity/display, stats, AI params, interaction (roles), and loot — that loads a
/// <c>content/core</c> NPC, edits it, and saves it back through the CST-preserving
/// layer so unrelated formatting and comments survive, or creates a new NPC and
/// writes canonical, schema-valid YAML.
/// </summary>
public sealed partial class NpcEditorViewModel : ViewModelBase
{
    private NpcDocument _document;
    private readonly IContentDialogService _dialogs;
    private readonly Func<string?> _workspacePath;

    /// <summary>Selectable enum tokens for the identity/AI pickers (schema enums).</summary>
    public static string[] CreatureTypes =>
        ["humanoid", "beast", "undead", "elemental", "demon", "dragonkin", "mechanical", "critter"];

    public static string[] Ranks => ["", "normal", "elite", "rare", "boss"];

    public static string[] Factions => ["friendly", "neutral", "hostile"];

    public static string[] Behaviors => ["", "aggressive", "defensive", "passive"];

    /// <summary>The AI ability rotation rows (CMB-01/CMB-02).</summary>
    public ObservableCollection<NpcAbilityRowViewModel> Abilities { get; } = new();

    [ObservableProperty]
    private string _filePath = string.Empty;

    [ObservableProperty]
    private bool _isDirty;

    [ObservableProperty]
    private string _statusText = "New NPC.";

    [ObservableProperty]
    private bool _isValid;

    [ObservableProperty]
    private string _validationMessage = string.Empty;

    [ObservableProperty]
    private string _previewYaml = string.Empty;

    public string DisplayPath => PathPresentation.Compact(FilePath);
    public bool HasFilePath => !string.IsNullOrWhiteSpace(FilePath);
    public bool ShowManualFilePath => !_dialogs.CanOpenFiles || !_dialogs.CanSaveFiles;

    public NpcEditorViewModel() : this(HeadlessContentDialogService.Instance) { }

    public NpcEditorViewModel(IContentDialogService dialogs, Func<string?>? workspacePath = null)
    {
        _dialogs = dialogs;
        _workspacePath = workspacePath ?? (() => null);
        _document = NpcDocument.NewNpc();
        RebuildFromDocument();
    }

    partial void OnFilePathChanged(string value)
    {
        OnPropertyChanged(nameof(DisplayPath));
        OnPropertyChanged(nameof(HasFilePath));
        CopyFullPathCommand.NotifyCanExecuteChanged();
    }

    private NpcData Data => _document.Data;

    // ---- identity / display (PRD §4.1) --------------------------------------

    public string Id { get => Field("id"); set => SetField("id", value); }
    public string Name { get => Field("name"); set => SetField("name", value); }
    public string Subtitle { get => Field("subtitle"); set => SetField("subtitle", value); }
    public string LevelMin { get => Field("level.min"); set => SetField("level.min", value); }
    public string LevelMax { get => Field("level.max"); set => SetField("level.max", value); }
    public string CreatureType { get => Field("creature_type"); set => SetField("creature_type", value); }
    public string Rank { get => Field("rank"); set => SetField("rank", value); }
    public string Faction { get => Field("faction"); set => SetField("faction", value); }
    public string VisualModel { get => Field("visual.model"); set => SetField("visual.model", value); }
    public string VisualScale { get => Field("visual.scale"); set => SetField("visual.scale", value); }
    public string VisualSoundSet { get => Field("visual.sound_set"); set => SetField("visual.sound_set", value); }

    // ---- stats (PRD §4.1) ----------------------------------------------------

    public string Health { get => Field("stats.health"); set => SetField("stats.health", value); }
    public string Mana { get => Field("stats.mana"); set => SetField("stats.mana", value); }
    public string Armor { get => Field("stats.armor"); set => SetField("stats.armor", value); }
    public string DamageMin { get => Field("stats.damage.min"); set => SetField("stats.damage.min", value); }
    public string DamageMax { get => Field("stats.damage.max"); set => SetField("stats.damage.max", value); }
    public string AttackSpeedMs { get => Field("stats.attack_speed_ms"); set => SetField("stats.attack_speed_ms", value); }

    // ---- AI params (PRD §4.1) ------------------------------------------------

    public string Behavior { get => Field("ai.behavior"); set => SetField("ai.behavior", value); }
    public string AggroRadiusM { get => Field("ai.aggro_radius_m"); set => SetField("ai.aggro_radius_m", value); }
    public string LeashRadiusM { get => Field("ai.leash_radius_m"); set => SetField("ai.leash_radius_m", value); }
    public string CallForHelpRadiusM { get => Field("ai.call_for_help_radius_m"); set => SetField("ai.call_for_help_radius_m", value); }
    public string FleeAtHealthPct { get => Field("ai.flee_at_health_pct"); set => SetField("ai.flee_at_health_pct", value); }
    public string WalkSpeedMps { get => Field("movement.walk_speed_mps"); set => SetField("movement.walk_speed_mps", value); }
    public string RunSpeedMps { get => Field("movement.run_speed_mps"); set => SetField("movement.run_speed_mps", value); }

    // ---- interaction / roles (PRD §4.1) -------------------------------------

    public string GossipText { get => Field("interaction.gossip_text"); set => SetField("interaction.gossip_text", value); }
    public string Vendor { get => Field("interaction.vendor"); set => SetField("interaction.vendor", value); }

    // ---- loot links (PRD §4.1) ----------------------------------------------

    public string LootTable { get => Field("loot.table"); set => SetField("loot.table", value); }
    public string MoneyMin { get => Field("loot.money.min"); set => SetField("loot.money.min", value); }
    public string MoneyMax { get => Field("loot.money.max"); set => SetField("loot.money.max", value); }

    // ---- commands ------------------------------------------------------------

    /// <summary>Start a fresh NPC with the schema's required fields pre-filled.</summary>
    [RelayCommand]
    private async Task NewAsync()
    {
        try
        {
            if (IsDirty && !await _dialogs.ConfirmDiscardChangesAsync(DisplayPath)) return;
            _document = NpcDocument.NewNpc();
            FilePath = string.Empty;
            RebuildFromDocument();
            IsDirty = false;
            StatusText = "New NPC.";
        }
        catch (Exception ex) { StatusText = $"New failed: {ex.Message}"; }
    }

    /// <summary>Load the NPC at <see cref="FilePath"/> into the form.</summary>
    [RelayCommand]
    private async Task OpenAsync()
    {
        try
        {
            var selected = _dialogs.CanOpenFiles
                ? await _dialogs.PickEntityFileAsync(EntityFileKind.Npc, FilePath, _workspacePath())
                : null;
            if (selected is null)
            {
                if (_dialogs.CanOpenFiles || string.IsNullOrWhiteSpace(FilePath)) return;
                selected = FilePath;
            }
            var candidate = NpcDocument.Load(selected);
            if (IsDirty && !await _dialogs.ConfirmDiscardChangesAsync(DisplayPath)) return;
            _document = candidate;
            FilePath = Path.GetFullPath(selected);
            RebuildFromDocument();
            IsDirty = false;
            StatusText = $"Loaded {System.IO.Path.GetFileName(FilePath)}.";
        }
        catch (Exception ex)
        {
            StatusText = $"Open failed: {ex.Message}";
        }
    }

    /// <summary>Save the form to <see cref="FilePath"/> (surgical for loaded files, canonical for new).</summary>
    [RelayCommand]
    private async Task SaveAsync()
    {
        if (!IsValid)
        {
            StatusText = $"Cannot save: {ValidationMessage}";
            return;
        }

        try
        {
            var selected = FilePath;
            if (string.IsNullOrWhiteSpace(selected))
            {
                if (!_dialogs.CanSaveFiles)
                {
                    StatusText = "Enter a file path below before saving on this platform.";
                    return;
                }
                selected = await _dialogs.PickEntitySaveFileAsync(EntityFileKind.Npc, FilePath, _workspacePath());
                if (selected is null) return;
            }
            SyncAbilities();
            if (!IsValid)
            {
                StatusText = $"Cannot save: {ValidationMessage}";
                return;
            }
            _document.Save(selected);
            FilePath = Path.GetFullPath(selected);
            IsDirty = false;
            StatusText = $"Saved {System.IO.Path.GetFileName(FilePath)}.";
        }
        catch (Exception ex)
        {
            StatusText = $"Save failed: {ex.Message}";
        }
    }

    [RelayCommand(CanExecute = nameof(HasFilePath))]
    private async Task CopyFullPathAsync()
    {
        try
        {
            await _dialogs.CopyPathAsync(FilePath);
            StatusText = "Full path copied to the clipboard.";
        }
        catch (Exception ex) { StatusText = $"Copy path failed: {ex.Message}"; }
    }

    /// <summary>Append a blank ability row to the rotation.</summary>
    [RelayCommand]
    private void AddAbility()
    {
        Abilities.Add(NewRow("ability.new_ability"));
        SyncAbilities();
    }

    /// <summary>Remove <paramref name="row"/> from the rotation.</summary>
    [RelayCommand]
    private void RemoveAbility(NpcAbilityRowViewModel? row)
    {
        if (row is not null && Abilities.Remove(row))
        {
            SyncAbilities();
        }
    }

    // ---- internals -----------------------------------------------------------

    private string Field(string path) => Data.Get(path) ?? string.Empty;

    private void SetField(string path, string value, [CallerMemberName] string? propertyName = null)
    {
        if (Field(path) == (value ?? string.Empty))
        {
            return;
        }

        Data.Set(path, value);
        OnPropertyChanged(propertyName);
        IsDirty = true;
        Revalidate();
    }

    private void RebuildFromDocument()
    {
        Abilities.Clear();
        for (int i = 0; i < Data.AbilityCount; i++)
        {
            Abilities.Add(RowFromData(i));
        }

        // Every bound field changed identity — refresh the whole form.
        OnPropertyChanged(string.Empty);
        Revalidate();
    }

    private NpcAbilityRowViewModel RowFromData(int index)
    {
        var row = new NpcAbilityRowViewModel
        {
            Ability = Data.Get($"ai.abilities[{index}].ability") ?? string.Empty,
            Priority = Data.Get($"ai.abilities[{index}].priority") ?? string.Empty,
            CooldownOverrideMs = Data.Get($"ai.abilities[{index}].cooldown_override_ms") ?? string.Empty,
            UseAtHealthBelowPct = Data.Get($"ai.abilities[{index}].use_at_health_below_pct") ?? string.Empty,
        };
        row.Changed += SyncAbilities;
        return row;
    }

    private NpcAbilityRowViewModel NewRow(string ability)
    {
        var row = new NpcAbilityRowViewModel { Ability = ability };
        row.Changed += SyncAbilities;
        return row;
    }

    private void SyncAbilities()
    {
        IsDirty = true;
        Data.RemoveAbilities();
        for (int i = 0; i < Abilities.Count; i++)
        {
            var row = Abilities[i];
            Data.Set($"ai.abilities[{i}].ability", row.Ability);
            Data.Set($"ai.abilities[{i}].priority", row.Priority);
            Data.Set($"ai.abilities[{i}].cooldown_override_ms", row.CooldownOverrideMs);
            Data.Set($"ai.abilities[{i}].use_at_health_below_pct", row.UseAtHealthBelowPct);
        }

        Revalidate();
    }

    private void Revalidate()
    {
        try
        {
            _ = _document.ToModel();
            IsValid = true;
            ValidationMessage = "Valid against npc.schema.yaml.";
        }
        catch (Exception ex)
        {
            IsValid = false;
            ValidationMessage = ex.Message;
        }

        try
        {
            PreviewYaml = _document.ToYaml();
        }
        catch (Exception ex)
        {
            PreviewYaml = $"# preview unavailable: {ex.Message}";
        }
    }
}

/// <summary>One row of the NPC AI ability rotation (ability ref + priority/cooldown/HP-threshold).</summary>
public sealed partial class NpcAbilityRowViewModel : ObservableObject
{
    /// <summary>Raised whenever any field changes, so the editor can resync the array to YAML.</summary>
    public event Action? Changed;

    [ObservableProperty]
    private string _ability = string.Empty;

    [ObservableProperty]
    private string _priority = string.Empty;

    [ObservableProperty]
    private string _cooldownOverrideMs = string.Empty;

    [ObservableProperty]
    private string _useAtHealthBelowPct = string.Empty;

    partial void OnAbilityChanged(string value) => Changed?.Invoke();

    partial void OnPriorityChanged(string value) => Changed?.Invoke();

    partial void OnCooldownOverrideMsChanged(string value) => Changed?.Invoke();

    partial void OnUseAtHealthBelowPctChanged(string value) => Changed?.Invoke();
}
