using System;
using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Meridian.Codex.Editing;
using Meridian.Codex.Services;

namespace Meridian.Codex.ViewModels;

/// <summary>
/// The item editor (TLS-04, issue #129). A schema-driven form over an
/// <see cref="ItemDocument"/> with fields grouped per Tools PRD §4.2 —
/// identity/class/slot/binding/stack, visual asset refs, primary-stat rows, weapon
/// and armor data, use/equip effects, and vendor price — that loads a
/// <c>content/core</c> item, edits it, and saves it back through the CST-preserving
/// layer so unrelated formatting and comments survive, or creates a new item and
/// writes canonical, schema-valid YAML. It is the item twin of
/// <see cref="NpcEditorViewModel"/> and plugs into the same shell/ViewLocator seam.
/// </summary>
public sealed partial class ItemEditorViewModel : ViewModelBase
{
    private ItemDocument _document;
    private readonly IContentDialogService _dialogs;
    private readonly Func<string?> _workspacePath;

    /// <summary>Selectable enum tokens for the identity pickers (schema enums).</summary>
    public static string[] ItemClasses =>
        ["weapon", "armor", "consumable", "quest", "trade_good", "container"];

    public static string[] Slots =>
    [
        "", "head", "shoulders", "back", "chest", "wrist", "hands", "waist", "legs", "feet",
        "neck", "finger", "trinket", "main_hand", "off_hand", "two_hand", "ranged", "bag",
    ];

    public static string[] Rarities => ["poor", "common", "uncommon", "rare", "epic", "legendary"];

    public static string[] Bindings => ["", "none", "on_pickup", "on_equip"];

    public static string[] Schools => ["", "physical", "fire", "frost", "nature", "shadow", "holy", "arcane"];

    public static string[] StatKeys => ["strength", "agility", "stamina", "intellect", "spirit"];

    /// <summary>Worn-contract enum tokens (meridian/skeleton@1 vocabulary, contract ①).</summary>
    public static string[] AttachSockets =>
        ["", "main_hand", "off_hand", "ranged", "back", "hip_l", "hip_r", "shield"];

    public static string[] Mirrors => ["", "none", "x"];

    public static string[] GeosetRegions =>
        ["head", "hands", "forearms", "torso", "waist", "hips_legs", "lower_legs", "feet"];

    public static string[] DyeChannels => ["primary", "secondary", "accent"];

    /// <summary>The item's primary-stat rows (ITM-01).</summary>
    public ObservableCollection<ItemStatRowViewModel> Stats { get; } = new();

    /// <summary>The on-equip ability rows (CMB-04 proc/effect refs, §4.2).</summary>
    public ObservableCollection<ItemOnEquipRowViewModel> OnEquip { get; } = new();

    /// <summary>The worn model rows (visual.worn.models, item@2 contract ①).</summary>
    public ObservableCollection<ItemWornModelRowViewModel> WornModels { get; } = new();

    /// <summary>The hidden-geoset rows (visual.worn.hides — armor only, contract ①).</summary>
    public ObservableCollection<ItemWornTokenRowViewModel> WornHides { get; } = new();

    /// <summary>The dye-channel rows (visual.worn.dye_channels, contract ①).</summary>
    public ObservableCollection<ItemWornTokenRowViewModel> WornDyeChannels { get; } = new();

    [ObservableProperty]
    private string _filePath = string.Empty;

    [ObservableProperty]
    private bool _isDirty;

    [ObservableProperty]
    private string _statusText = "New item.";

    [ObservableProperty]
    private bool _isValid;

    [ObservableProperty]
    private string _validationMessage = string.Empty;

    [ObservableProperty]
    private string _previewYaml = string.Empty;

    public string DisplayPath => PathPresentation.Compact(FilePath);
    public bool HasFilePath => !string.IsNullOrWhiteSpace(FilePath);

    public ItemEditorViewModel() : this(HeadlessContentDialogService.Instance) { }

    public ItemEditorViewModel(IContentDialogService dialogs, Func<string?>? workspacePath = null)
    {
        _dialogs = dialogs;
        _workspacePath = workspacePath ?? (() => null);
        _document = ItemDocument.NewItem();
        RebuildFromDocument();
    }

    partial void OnFilePathChanged(string value)
    {
        OnPropertyChanged(nameof(DisplayPath));
        OnPropertyChanged(nameof(HasFilePath));
        CopyFullPathCommand.NotifyCanExecuteChanged();
    }

    private ItemData Data => _document.Data;

    // ---- identity / classification (PRD §4.2) -------------------------------

    public string Id { get => Field("id"); set => SetField("id", value); }
    public string Name { get => Field("name"); set => SetField("name", value); }
    public string FlavorText { get => Field("flavor_text"); set => SetField("flavor_text", value); }
    public string ItemClass { get => Field("item_class"); set => SetField("item_class", value); }
    public string Subclass { get => Field("subclass"); set => SetField("subclass", value); }
    public string EquipType { get => Field("equip_type"); set => SetField("equip_type", value); }
    public string Slot { get => Field("slot"); set => SetField("slot", value); }
    public string Rarity { get => Field("rarity"); set => SetField("rarity", value); }
    public string RequiredLevel { get => Field("required_level"); set => SetField("required_level", value); }
    public string ItemLevel { get => Field("item_level"); set => SetField("item_level", value); }
    public string Binding { get => Field("binding"); set => SetField("binding", value); }
    public string StackSize { get => Field("stack_size"); set => SetField("stack_size", value); }

    /// <summary>Bind-on-pickup uniques etc.; absent = not unique (schema default false).</summary>
    public bool Unique
    {
        get => Field("unique") == "true";
        set
        {
            Data.Set("unique", value ? "true" : null);
            OnPropertyChanged();
            MarkDirty();
            Revalidate();
        }
    }

    // ---- visual (PRD §4.2) ---------------------------------------------------

    public string VisualIcon { get => Field("visual.icon"); set => SetField("visual.icon", value); }
    public string VisualModel { get => Field("visual.model"); set => SetField("visual.model", value); }

    // ---- worn / equipped render (item@2, contract ①) --------------------------

    public string WornAttachSocket
    {
        get => Field("visual.worn.attach.socket");
        set => SetField("visual.worn.attach.socket", value);
    }

    public string WornSheathSocket
    {
        get => Field("visual.worn.attach.sheath_socket");
        set => SetField("visual.worn.attach.sheath_socket", value);
    }

    // ---- weapon / armor (PRD §4.2) ------------------------------------------

    public string WeaponDamageMin { get => Field("weapon.damage.min"); set => SetField("weapon.damage.min", value); }
    public string WeaponDamageMax { get => Field("weapon.damage.max"); set => SetField("weapon.damage.max", value); }
    public string WeaponSpeedMs { get => Field("weapon.speed_ms"); set => SetField("weapon.speed_ms", value); }
    public string WeaponSchool { get => Field("weapon.school"); set => SetField("weapon.school", value); }
    public string Armor { get => Field("armor"); set => SetField("armor", value); }

    // ---- effects (PRD §4.2) --------------------------------------------------

    public string OnUse { get => Field("effects.on_use"); set => SetField("effects.on_use", value); }

    // ---- price / economy (ECO-01, PRD §4.2) ---------------------------------

    public string PriceSell { get => Field("price.sell"); set => SetField("price.sell", value); }
    public string PriceBuy { get => Field("price.buy"); set => SetField("price.buy", value); }

    // ---- commands ------------------------------------------------------------

    /// <summary>Start a fresh item with the schema's required fields pre-filled.</summary>
    [RelayCommand]
    private async Task NewAsync()
    {
        if (IsDirty && !await _dialogs.ConfirmDiscardChangesAsync(DisplayPath)) return;
        _document = ItemDocument.NewItem();
        FilePath = string.Empty;
        RebuildFromDocument();
        IsDirty = false;
        StatusText = "New item.";
    }

    /// <summary>Load the item at <see cref="FilePath"/> into the form.</summary>
    [RelayCommand]
    private async Task OpenAsync()
    {
        var selected = await _dialogs.PickEntityFileAsync(EntityFileKind.Item, FilePath, _workspacePath());
        if (selected is null)
        {
            if (_dialogs.IsNativePickerAvailable || string.IsNullOrWhiteSpace(FilePath)) return;
            selected = FilePath; // Explicit advanced/headless path entry.
        }

        try
        {
            var candidate = ItemDocument.Load(selected);
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
    private void Save()
    {
        if (string.IsNullOrWhiteSpace(FilePath))
        {
            StatusText = "Set a file path before saving.";
            return;
        }

        SyncStats();
        SyncOnEquip();
        SyncWornModels();
        SyncWornHides();
        SyncWornDyeChannels();
        if (!IsValid)
        {
            StatusText = $"Cannot save: {ValidationMessage}";
            return;
        }

        try
        {
            _document.Save(FilePath);
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
        await _dialogs.CopyPathAsync(FilePath);
        StatusText = "Full path copied to the clipboard.";
    }

    /// <summary>Append a blank primary-stat row.</summary>
    [RelayCommand]
    private void AddStat()
    {
        Stats.Add(NewStatRow("strength"));
        SyncStats();
    }

    /// <summary>Remove <paramref name="row"/> from the stat list.</summary>
    [RelayCommand]
    private void RemoveStat(ItemStatRowViewModel? row)
    {
        if (row is not null && Stats.Remove(row))
        {
            SyncStats();
        }
    }

    /// <summary>Append a blank on-equip ability row.</summary>
    [RelayCommand]
    private void AddOnEquip()
    {
        OnEquip.Add(NewOnEquipRow("ability.new_effect"));
        SyncOnEquip();
    }

    /// <summary>Remove <paramref name="row"/> from the on-equip list.</summary>
    [RelayCommand]
    private void RemoveOnEquip(ItemOnEquipRowViewModel? row)
    {
        if (row is not null && OnEquip.Remove(row))
        {
            SyncOnEquip();
        }
    }

    /// <summary>Append a blank worn-model row.</summary>
    [RelayCommand]
    private void AddWornModel()
    {
        WornModels.Add(NewWornModelRow("core:art.item.new_model"));
        SyncWornModels();
    }

    /// <summary>Remove <paramref name="row"/> from the worn-model list.</summary>
    [RelayCommand]
    private void RemoveWornModel(ItemWornModelRowViewModel? row)
    {
        if (row is not null && WornModels.Remove(row))
        {
            SyncWornModels();
        }
    }

    /// <summary>Append a blank hidden-geoset row.</summary>
    [RelayCommand]
    private void AddWornHide()
    {
        WornHides.Add(NewTokenRow("torso", SyncWornHides));
        SyncWornHides();
    }

    /// <summary>Remove <paramref name="row"/> from the hidden-geoset list.</summary>
    [RelayCommand]
    private void RemoveWornHide(ItemWornTokenRowViewModel? row)
    {
        if (row is not null && WornHides.Remove(row))
        {
            SyncWornHides();
        }
    }

    /// <summary>Append a blank dye-channel row.</summary>
    [RelayCommand]
    private void AddWornDyeChannel()
    {
        WornDyeChannels.Add(NewTokenRow("primary", SyncWornDyeChannels));
        SyncWornDyeChannels();
    }

    /// <summary>Remove <paramref name="row"/> from the dye-channel list.</summary>
    [RelayCommand]
    private void RemoveWornDyeChannel(ItemWornTokenRowViewModel? row)
    {
        if (row is not null && WornDyeChannels.Remove(row))
        {
            SyncWornDyeChannels();
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
        MarkDirty();
        Revalidate();
    }

    private void RebuildFromDocument()
    {
        Stats.Clear();
        for (int i = 0; i < Data.StatCount; i++)
        {
            Stats.Add(StatRowFromData(i));
        }

        OnEquip.Clear();
        for (int i = 0; i < Data.OnEquipCount; i++)
        {
            OnEquip.Add(OnEquipRowFromData(i));
        }

        WornModels.Clear();
        for (int i = 0; i < Data.WornModelCount; i++)
        {
            WornModels.Add(WornModelRowFromData(i));
        }

        WornHides.Clear();
        for (int i = 0; i < Data.WornHideCount; i++)
        {
            WornHides.Add(TokenRowFromData($"visual.worn.hides[{i}]", SyncWornHides));
        }

        WornDyeChannels.Clear();
        for (int i = 0; i < Data.WornDyeChannelCount; i++)
        {
            WornDyeChannels.Add(TokenRowFromData($"visual.worn.dye_channels[{i}]", SyncWornDyeChannels));
        }

        // Every bound field changed identity — refresh the whole form.
        OnPropertyChanged(string.Empty);
        Revalidate();
    }

    private ItemStatRowViewModel StatRowFromData(int index)
    {
        var row = new ItemStatRowViewModel
        {
            Stat = Data.Get($"stats[{index}].stat") ?? string.Empty,
            Amount = Data.Get($"stats[{index}].amount") ?? string.Empty,
        };
        row.Changed += SyncStats;
        return row;
    }

    private ItemStatRowViewModel NewStatRow(string stat)
    {
        var row = new ItemStatRowViewModel { Stat = stat };
        row.Changed += SyncStats;
        return row;
    }

    private ItemOnEquipRowViewModel OnEquipRowFromData(int index)
    {
        var row = new ItemOnEquipRowViewModel
        {
            Ability = Data.Get($"effects.on_equip[{index}]") ?? string.Empty,
        };
        row.Changed += SyncOnEquip;
        return row;
    }

    private ItemOnEquipRowViewModel NewOnEquipRow(string ability)
    {
        var row = new ItemOnEquipRowViewModel { Ability = ability };
        row.Changed += SyncOnEquip;
        return row;
    }

    private void SyncStats()
    {
        MarkDirty();
        Data.RemoveStats();
        for (int i = 0; i < Stats.Count; i++)
        {
            Data.Set($"stats[{i}].stat", Stats[i].Stat);
            Data.Set($"stats[{i}].amount", Stats[i].Amount);
        }

        Revalidate();
    }

    private void SyncOnEquip()
    {
        MarkDirty();
        Data.RemoveOnEquip();
        for (int i = 0; i < OnEquip.Count; i++)
        {
            Data.Set($"effects.on_equip[{i}]", OnEquip[i].Ability);
        }

        Revalidate();
    }

    private ItemWornModelRowViewModel WornModelRowFromData(int index)
    {
        var row = new ItemWornModelRowViewModel
        {
            Model = Data.Get($"visual.worn.models[{index}].model") ?? string.Empty,
            Mirror = Data.Get($"visual.worn.models[{index}].mirror") ?? string.Empty,
        };
        row.Changed += SyncWornModels;
        return row;
    }

    private ItemWornModelRowViewModel NewWornModelRow(string model)
    {
        var row = new ItemWornModelRowViewModel { Model = model };
        row.Changed += SyncWornModels;
        return row;
    }

    private ItemWornTokenRowViewModel TokenRowFromData(string path, Action sync)
    {
        var row = new ItemWornTokenRowViewModel { Token = Data.Get(path) ?? string.Empty };
        row.Changed += sync;
        return row;
    }

    private ItemWornTokenRowViewModel NewTokenRow(string token, Action sync)
    {
        var row = new ItemWornTokenRowViewModel { Token = token };
        row.Changed += sync;
        return row;
    }

    private void SyncWornModels()
    {
        MarkDirty();
        Data.RemoveWornModels();
        for (int i = 0; i < WornModels.Count; i++)
        {
            Data.Set($"visual.worn.models[{i}].model", WornModels[i].Model);
            Data.Set($"visual.worn.models[{i}].mirror", WornModels[i].Mirror);
        }

        Revalidate();
    }

    private void SyncWornHides()
    {
        MarkDirty();
        Data.RemoveWornHides();
        for (int i = 0; i < WornHides.Count; i++)
        {
            Data.Set($"visual.worn.hides[{i}]", WornHides[i].Token);
        }

        Revalidate();
    }

    private void SyncWornDyeChannels()
    {
        MarkDirty();
        Data.RemoveWornDyeChannels();
        for (int i = 0; i < WornDyeChannels.Count; i++)
        {
            Data.Set($"visual.worn.dye_channels[{i}]", WornDyeChannels[i].Token);
        }

        Revalidate();
    }

    private void Revalidate()
    {
        try
        {
            _ = _document.ToModel();
            IsValid = true;
            ValidationMessage = "Valid against item.schema.yaml.";
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

    private void MarkDirty()
    {
        IsDirty = true;
    }
}

/// <summary>One primary-stat row of an item (stat key + integer amount).</summary>
public sealed partial class ItemStatRowViewModel : ObservableObject
{
    /// <summary>Raised whenever any field changes, so the editor can resync the array to YAML.</summary>
    public event Action? Changed;

    [ObservableProperty]
    private string _stat = string.Empty;

    [ObservableProperty]
    private string _amount = string.Empty;

    partial void OnStatChanged(string value) => Changed?.Invoke();

    partial void OnAmountChanged(string value) => Changed?.Invoke();
}

/// <summary>One on-equip effect row of an item (a single ability ref).</summary>
public sealed partial class ItemOnEquipRowViewModel : ObservableObject
{
    /// <summary>Raised whenever the ability ref changes, so the editor can resync the array to YAML.</summary>
    public event Action? Changed;

    [ObservableProperty]
    private string _ability = string.Empty;

    partial void OnAbilityChanged(string value) => Changed?.Invoke();
}

/// <summary>One worn-model row of an item (art ref + optional mirror, visual.worn.models).</summary>
public sealed partial class ItemWornModelRowViewModel : ObservableObject
{
    /// <summary>Raised whenever any field changes, so the editor can resync the array to YAML.</summary>
    public event Action? Changed;

    [ObservableProperty]
    private string _model = string.Empty;

    [ObservableProperty]
    private string _mirror = string.Empty;

    partial void OnModelChanged(string value) => Changed?.Invoke();

    partial void OnMirrorChanged(string value) => Changed?.Invoke();
}

/// <summary>One enum-token row of a worn sequence (a geoset region or dye channel).</summary>
public sealed partial class ItemWornTokenRowViewModel : ObservableObject
{
    /// <summary>Raised whenever the token changes, so the editor can resync the array to YAML.</summary>
    public event Action? Changed;

    [ObservableProperty]
    private string _token = string.Empty;

    partial void OnTokenChanged(string value) => Changed?.Invoke();
}
