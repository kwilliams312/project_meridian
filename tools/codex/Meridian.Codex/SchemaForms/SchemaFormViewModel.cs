using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json.Nodes;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Meridian.Codex.SchemaForms;

public sealed partial class SchemaFormViewModel : ObservableObject
{
    private IReadOnlyList<SchemaDiagnostic> _diagnostics = [];

    public SchemaFormViewModel(SchemaFormDocument document)
    {
        Document = document;
        Root = new SchemaFieldViewModel(document.Schema, document, Refresh);
        document.Changed += (_, _) => Root.NotifyRequirementStateChanged();
    }

    public SchemaFormDocument Document { get; }
    public SchemaFieldViewModel Root { get; private set; }

    public void SetDiagnostics(IReadOnlyList<SchemaDiagnostic> diagnostics)
    {
        _diagnostics = diagnostics;
        Root.SetDiagnostics(diagnostics);
    }

    private void Refresh()
    {
        Root = new SchemaFieldViewModel(Document.Schema, Document, Refresh);
        Root.SetDiagnostics(_diagnostics);
        OnPropertyChanged(nameof(Root));
    }
}

public sealed partial class SchemaFieldViewModel : ObservableObject
{
    private readonly SchemaFormDocument _document;
    private readonly Action _refresh;
    private readonly string? _arrayPath;
    private readonly int? _arrayIndex;
    private string? _pendingVariant;

    public SchemaFieldViewModel(SchemaField field, SchemaFormDocument document, Action refresh, string? concretePath = null, string? arrayPath = null, int? arrayIndex = null)
    {
        Field = field;
        _document = document;
        _refresh = refresh;
        Path = concretePath ?? field.Path;
        _arrayPath = arrayPath;
        _arrayIndex = arrayIndex;
        RebuildChildren();
    }

    public SchemaField Field { get; }
    public string Path { get; }
    public string FieldName => Field.Ui?.Label ?? Field.Title;
    public string Label => FieldName + (IsRequiredForForm ? " *" : string.Empty);
    public string RequirementText => Field.Constant is not null
        ? "Fixed by schema; read-only"
        : ActiveConditionalRequirement is { } activeRequirement
            ? $"Required because {activeRequirement.Description}"
            : Field.ConditionalRequirements.Count > 0
                ? $"Optional now; required when {ConditionalRequirementDescription}"
            : Field.IsRequired
                ? Field.AvailabilityCondition is { } availability
                    ? $"Required when {availability}"
                    : "Required"
                : Field.Default is not null
                    ? "Optional; a default is provided"
                    : "Optional";
    public string RequirementStateText => Field.Constant is not null
        ? "Fixed"
        : IsRequiredForForm
            ? "Required"
            : Field.Default is not null
                ? "Optional (default)"
                : "Optional";
    public string Help => Field.Ui?.Help ?? Field.Description ?? KindHelp;
    public string? ExampleText => Field.Ui?.Example is { } example ? $"Example: {Display(example)}" : null;
    public string? UnitText => Field.Ui?.Unit is { } unit ? $"Unit: {UnitName(unit)}" : null;
    public string? DefaultText => Field.Default is { } value && Field.Constant is null ? $"Default: {Display(value)}" : null;
    public string? ConstraintText => Field.Ui?.Constraint ?? DerivedConstraint;
    public string? DocumentationText => Field.Ui?.Documentation is { } documentation ? $"Learn more: {documentation}" : null;
    public string? Watermark => Field.Ui?.Example is { } example ? Display(example) : null;
    public bool HasExample => ExampleText is not null;
    public bool HasUnit => UnitText is not null;
    public bool HasDefault => DefaultText is not null;
    public bool HasConstraint => ConstraintText is not null;
    public bool HasDocumentation => DocumentationText is not null;
    public bool HasAvailability => Field.AvailabilityCondition is not null;
    public string? AvailabilityText => Field.AvailabilityCondition is { } condition
        ? $"Shown because {condition}. Values from other types are preserved when you switch back."
        : Field.Kind == SchemaFieldKind.OneOf
            ? "Choose a type to show its fields. Switching type preserves earlier values and asks before removing active fields."
            : null;
    public bool HasAvailabilityText => AvailabilityText is not null;
    public string InformationText => string.Join(" ", new[]
    {
        RequirementText != RequirementStateText ? RequirementText + "." : null,
        Help,
        UnitText,
        DefaultText,
        ExampleText,
        ConstraintText is { } constraint ? $"Constraint: {constraint}" : null,
        AvailabilityText,
        DocumentationText,
    }.Where(value => !string.IsNullOrWhiteSpace(value)));
    public bool HasInformation => !string.IsNullOrWhiteSpace(InformationText);
    public string InformationAutomationName => $"More information about {FieldName}";
    public string InformationAutomationId => "SchemaFieldInfo_" + Path.Replace('.', '_').Replace('[', '_').Replace("]", string.Empty, StringComparison.Ordinal);
    public string AutomationName => FieldName;
    public string AutomationHelp => string.Join(" ", new[]
    {
        RequirementStateText + ".",
        InformationText,
        HasDiagnostic ? $"Error: {DiagnosticText}" : null,
    }.Where(value => !string.IsNullOrWhiteSpace(value)));
    public string AutomationId => "SchemaField_" + Path.Replace('.', '_').Replace('[', '_').Replace("]", string.Empty, StringComparison.Ordinal);
    public string AutomationStatus => HasDiagnostic ? $"Invalid: {DiagnosticText}" : RequirementText;
    public bool IsRequiredForForm => Field.IsRequired || ActiveConditionalRequirement is not null;
    public string AddFieldActionText => IsRequiredForForm ? "Add required field" : "Add optional field";
    public string AddOptionalAutomationName => $"Add {(IsRequiredForForm ? "required" : "optional")} {Field.Title}";
    public string RemoveOptionalAutomationName => $"Remove optional {Field.Title}";
    public string AddItemAutomationName => $"Add item to {Field.Title}";
    public string MoveUpAutomationName => $"Move {Field.Title} up";
    public string MoveDownAutomationName => $"Move {Field.Title} down";
    public string RemoveItemAutomationName => $"Remove {Field.Title}";
    public string ConfirmBranchAutomationName => $"Confirm {Field.Title} branch change";
    public bool IsPresent => Path.Length == 0 || _document.Get(Path) is not null;
    public bool CanEdit => !Field.IsReadOnly && Field.Kind != SchemaFieldKind.Unsupported;
    public bool IsScalar => Field.Kind == SchemaFieldKind.String;
    public bool IsNumeric => Field.Kind is SchemaFieldKind.Integer or SchemaFieldKind.Number;
    public bool IsBoolean => Field.Kind == SchemaFieldKind.Boolean;
    public bool IsEnum => Field.Kind == SchemaFieldKind.Enum;
    public bool IsObject => Field.Kind is SchemaFieldKind.Object or SchemaFieldKind.OneOf;
    public bool IsArray => Field.Kind == SchemaFieldKind.Array;
    public bool IsUnsupported => Field.Kind == SchemaFieldKind.Unsupported;
    public bool IsArrayItem => _arrayPath is not null;
    public bool HasVariants => IsPresent && Field.Variants.Count > 0;
    public bool CanAddOptional => !IsPresent && CanEdit;
    public bool CanRemoveOptional => !IsRequiredForForm && IsPresent && CanEdit && !IsArrayItem;
    public bool ShowScalar => IsPresent && IsScalar;
    public bool ShowNumeric => IsPresent && IsNumeric;
    public bool ShowBoolean => IsPresent && IsBoolean;
    public bool ShowEnum => IsPresent && IsEnum;
    public bool ShowChildren => IsPresent && IsObject;
    public bool CanAddArrayItem => IsPresent && IsArray;
    public string? UnsupportedReason => Field.UnsupportedReason;
    public IReadOnlyList<string> Choices => Field.Choices;
    public ObservableCollection<SchemaFieldViewModel> Children { get; } = [];
    public IReadOnlyList<string> VariantChoices => Field.Variants.Select(variant => variant.Key).ToArray();

    private string KindHelp => Field.Ui?.ReferenceType switch
    {
        "content:ability" => "Enter an ability ID such as core:ability.cleave_strike.",
        { } reference when reference.StartsWith("content:", StringComparison.Ordinal) => "Enter a content ID using namespace:type.name format.",
        { } reference when reference.StartsWith("asset:", StringComparison.Ordinal) => "Enter an asset-registry ID using namespace:asset_type.name format.",
        _ when Field.Kind == SchemaFieldKind.OneOf => "Choose the value type, then complete the fields that appear.",
        _ when Field.Kind == SchemaFieldKind.Array => "Add, remove, or reorder entries in this list.",
        _ when Field.Kind == SchemaFieldKind.Object => "Complete the related fields in this group.",
        _ => $"Enter {FieldName.ToLowerInvariant()}.",
    };

    private SchemaConditionalRequirement? ActiveConditionalRequirement => Field.ConditionalRequirements.FirstOrDefault(requirement =>
        requirement.Conditions.All(condition => string.Equals(
            _document.Get(condition.Path)?.ToString(),
            condition.ExpectedValue,
            StringComparison.Ordinal)));

    private string ConditionalRequirementDescription => string.Join(" or ", Field.ConditionalRequirements.Select(requirement => requirement.Description));

    private string? DerivedConstraint
    {
        get
        {
            if (Field.Choices.Count > 0) return $"Choose one of: {string.Join(", ", Field.Choices)}.";
            if (Field.Minimum is { } minimum && Field.Maximum is { } maximum)
                return $"{(Field.HasExclusiveMinimum ? "Greater than" : "At least")} {minimum} and {(Field.HasExclusiveMaximum ? "less than" : "at most")} {maximum}.";
            if (Field.Minimum is { } lower) return $"{(Field.HasExclusiveMinimum ? "Greater than" : "At least")} {lower}.";
            if (Field.Maximum is { } upper) return $"{(Field.HasExclusiveMaximum ? "Less than" : "At most")} {upper}.";
            if (Field.MinimumLength is { } minLength && Field.MaximumLength is { } maxLength) return $"Use {minLength}-{maxLength} characters.";
            if (Field.MinimumLength is { } minimumLength) return $"Use at least {minimumLength} character{(minimumLength == 1 ? string.Empty : "s")}.";
            if (Field.MaximumLength is { } maximumLength) return $"Use at most {maximumLength} characters.";
            if (Field.Pattern is not null) return "Use the required ID or value format shown in the example.";
            return null;
        }
    }

    private string RepairHint
    {
        get
        {
            if (ConstraintText is { } constraint) return $"Fix: {constraint}";
            if (ExampleText is { } example) return $"Fix: follow the format in {example.ToLowerInvariant()}.";
            return $"Fix: enter a valid value for {FieldName}.";
        }
    }

    private static string UnitName(string unit) => unit switch
    {
        "ms" => "milliseconds (ms)",
        "m" => "meters (m)",
        "mps" => "meters per second (m/s)",
        "percent" => "percent (%)",
        "copper" => "copper (100 copper = 1 silver; 10,000 copper = 1 gold)",
        "scale" => "model scale multiplier",
        _ => unit,
    };

    private static string Display(JsonNode value) => value.ToString();

    [ObservableProperty] private string? _branchWarning;

    public string? SelectedVariant
    {
        get
        {
            if (Field.Kind != SchemaFieldKind.OneOf || _document.Get(Path) is not JsonObject current) return null;
            return Field.Variants.FirstOrDefault(variant =>
                variant.Schema.Children.Any(child => child.Constant?.ToString() == current[child.Name]?.ToString()))?.Key;
        }
        set
        {
            if (value is null || value == SelectedVariant) return;
            var result = _document.SelectBranch(Path, Field, value);
            if (result.Changed) _refresh();
            else { _pendingVariant = value; BranchWarning = result.Warning; }
            OnPropertyChanged();
        }
    }

    public string? TextValue
    {
        get => _document.Get(Path)?.ToString();
        set
        {
            JsonNode? node = Field.Kind switch
            {
                SchemaFieldKind.Integer when long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var integer) => JsonValue.Create(integer),
                SchemaFieldKind.Number when decimal.TryParse(value, NumberStyles.Number, CultureInfo.InvariantCulture, out var number) => JsonValue.Create(number),
                _ => JsonValue.Create(value ?? string.Empty),
            };
            _document.Set(Path, node);
            OnPropertyChanged();
        }
    }

    public decimal? NumericValue
    {
        get => decimal.TryParse(_document.Get(Path)?.ToString(), NumberStyles.Number, CultureInfo.InvariantCulture, out var number) ? number : null;
        set
        {
            if (value is null) return;
            JsonNode node = Field.Kind == SchemaFieldKind.Integer
                ? JsonValue.Create(decimal.ToInt64(decimal.Truncate(value.Value)))!
                : JsonValue.Create(value.Value)!;
            _document.Set(Path, node);
            OnPropertyChanged();
        }
    }

    public decimal NumericIncrement => Field.Kind == SchemaFieldKind.Integer ? 1 : 0.1m;
    public decimal? NumericMinimum => Field.Minimum is { } minimum
        ? minimum + (Field.HasExclusiveMinimum ? NumericIncrement : 0)
        : null;
    public decimal? NumericMaximum => Field.Maximum is { } maximum
        ? maximum - (Field.HasExclusiveMaximum ? NumericIncrement : 0)
        : null;

    public string? DiagnosticText { get; private set; }
    public bool HasDiagnostic => !string.IsNullOrEmpty(DiagnosticText);

    public bool BoolValue
    {
        get => _document.Get(Path)?.GetValue<bool>() ?? false;
        set { _document.Set(Path, JsonValue.Create(value)); OnPropertyChanged(); }
    }

    public string? SelectedChoice
    {
        get => TextValue;
        set { if (value is not null) TextValue = value; }
    }

    [RelayCommand]
    private void AddOptional()
    {
        var value = SchemaDefaultValue.Create(Field);
        _document.Set(Path, value); _refresh();
    }

    [RelayCommand]
    private void RemoveOptional() { _document.Set(Path, null); _refresh(); }

    [RelayCommand]
    private void AddArrayItem() { _document.AddArrayItem(Path, SchemaDefaultValue.Create(Field.Item!)); _refresh(); }

    [RelayCommand]
    private void RemoveArrayItem(SchemaFieldViewModel item)
    {
        var index = Children.IndexOf(item); _document.RemoveArrayItem(Path, index); _refresh();
    }

    [RelayCommand]
    private void MoveUp(SchemaFieldViewModel item)
    {
        var index = Children.IndexOf(item); if (index > 0) { _document.MoveArrayItem(Path, index, index - 1); _refresh(); }
    }

    [RelayCommand]
    private void MoveDown(SchemaFieldViewModel item)
    {
        var index = Children.IndexOf(item); if (index >= 0 && index + 1 < Children.Count) { _document.MoveArrayItem(Path, index, index + 1); _refresh(); }
    }

    [RelayCommand]
    private void ConfirmBranchChange()
    {
        if (_pendingVariant is null) return;
        _document.SelectBranch(Path, Field, _pendingVariant, true);
        _pendingVariant = null; BranchWarning = null; _refresh();
    }

    [RelayCommand]
    private void RemoveSelf()
    {
        if (_arrayPath is null || _arrayIndex is null) return;
        _document.RemoveArrayItem(_arrayPath, _arrayIndex.Value); _refresh();
    }

    [RelayCommand]
    private void MoveSelfUp()
    {
        if (_arrayPath is null || _arrayIndex is null || _arrayIndex == 0) return;
        _document.MoveArrayItem(_arrayPath, _arrayIndex.Value, _arrayIndex.Value - 1); _refresh();
    }

    [RelayCommand]
    private void MoveSelfDown()
    {
        if (_arrayPath is null || _arrayIndex is null) return;
        var count = (_document.Get(_arrayPath) as JsonArray)?.Count ?? 0;
        if (_arrayIndex.Value + 1 >= count) return;
        _document.MoveArrayItem(_arrayPath, _arrayIndex.Value, _arrayIndex.Value + 1); _refresh();
    }

    private void RebuildChildren()
    {
        if (Field.Kind == SchemaFieldKind.Object)
            foreach (var child in Field.Children) Children.Add(new(child, _document, _refresh, ChildPath(child)));
        else if (Field.Kind == SchemaFieldKind.OneOf && _document.Get(Path) is JsonObject current)
        {
            var active = Field.Variants.FirstOrDefault(variant =>
                variant.Schema.Children.Any(child => child.Constant is not null && child.Constant.ToString() == current[child.Name]?.ToString()));
            if (active is not null)
                foreach (var child in active.Schema.Children) Children.Add(new(child, _document, _refresh, ChildPath(child)));
        }
        else if (Field.Kind == SchemaFieldKind.Array && _document.Get(Path) is JsonArray array && Field.Item is { } item)
            for (var i = 0; i < array.Count; i++) Children.Add(new(item, _document, _refresh, $"{Path}[{i}]", Path, i));
    }

    private string ChildPath(SchemaField child) => Path.Length == 0 ? child.Name : $"{Path}.{child.Name}";

    internal void SetDiagnostics(IReadOnlyList<SchemaDiagnostic> diagnostics)
    {
        var messages = diagnostics
            .Where(diagnostic => diagnostic.Path == Path)
            .Select(diagnostic => $"{diagnostic.Message} {RepairHint}")
            .ToList();
        if (IsRequiredForForm && !IsPresent)
            messages.Add($"This field is {RequirementText.ToLowerInvariant()}. Fix: add the required field and choose a value.");
        var distinctMessages = messages.Distinct().ToArray();
        DiagnosticText = distinctMessages.Length == 0 ? null : string.Join(Environment.NewLine, distinctMessages);
        OnPropertyChanged(nameof(DiagnosticText));
        OnPropertyChanged(nameof(HasDiagnostic));
        OnPropertyChanged(nameof(AutomationHelp));
        OnPropertyChanged(nameof(AutomationStatus));
        foreach (var child in Children) child.SetDiagnostics(diagnostics);
    }

    internal void NotifyRequirementStateChanged()
    {
        OnPropertyChanged(nameof(Label));
        OnPropertyChanged(nameof(RequirementText));
        OnPropertyChanged(nameof(RequirementStateText));
        OnPropertyChanged(nameof(InformationText));
        OnPropertyChanged(nameof(HasInformation));
        OnPropertyChanged(nameof(AutomationHelp));
        OnPropertyChanged(nameof(AutomationStatus));
        OnPropertyChanged(nameof(IsRequiredForForm));
        OnPropertyChanged(nameof(AddFieldActionText));
        OnPropertyChanged(nameof(AddOptionalAutomationName));
        OnPropertyChanged(nameof(IsPresent));
        OnPropertyChanged(nameof(CanAddOptional));
        OnPropertyChanged(nameof(CanRemoveOptional));
        OnPropertyChanged(nameof(ShowScalar));
        OnPropertyChanged(nameof(ShowNumeric));
        OnPropertyChanged(nameof(ShowBoolean));
        OnPropertyChanged(nameof(ShowEnum));
        OnPropertyChanged(nameof(ShowChildren));
        OnPropertyChanged(nameof(CanAddArrayItem));
        foreach (var child in Children) child.NotifyRequirementStateChanged();
    }

}
