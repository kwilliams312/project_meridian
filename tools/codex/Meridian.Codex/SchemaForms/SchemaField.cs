using System.Text.Json.Nodes;

namespace Meridian.Codex.SchemaForms;

/// <summary>A UI-neutral form description projected from the authoritative JSON Schema.</summary>
public sealed class SchemaField
{
    public required string Name { get; init; }
    public required string Path { get; init; }
    public string Title { get; init; } = string.Empty;
    public string? Description { get; init; }
    public SchemaUiDescriptor? Ui { get; init; }
    public SchemaAssetDescriptor? Asset { get; init; }
    public SchemaFieldKind Kind { get; init; }
    public bool IsRequired { get; init; }
    public IReadOnlyList<SchemaConditionalRequirement> ConditionalRequirements { get; init; } = [];
    public string? AvailabilityCondition { get; init; }
    public bool IsReadOnly { get; init; }
    public string? UnsupportedReason { get; init; }
    public JsonNode? Default { get; init; }
    public JsonNode? Constant { get; init; }
    public decimal? Minimum { get; init; }
    public decimal? Maximum { get; init; }
    public bool HasExclusiveMinimum { get; init; }
    public bool HasExclusiveMaximum { get; init; }
    public int? MinimumLength { get; init; }
    public int? MaximumLength { get; init; }
    public string? Pattern { get; init; }
    public IReadOnlyList<string> Choices { get; init; } = [];
    public IReadOnlyList<SchemaField> Children { get; init; } = [];
    public SchemaField? Item { get; init; }
    public IReadOnlyList<SchemaVariant> Variants { get; init; } = [];
}

public sealed record SchemaUiDescriptor(
    string? Group,
    string? Label,
    string? Widget,
    string? Unit,
    string? ReferenceType,
    string? Help,
    JsonNode? Example,
    string? Constraint,
    string? Documentation);

public sealed record SchemaAssetDescriptor(
    IReadOnlyList<string> AllowedClasses,
    IReadOnlyList<string> EligibleGenerators);

public sealed record SchemaConditionalRequirement(
    IReadOnlyList<SchemaCondition> Conditions,
    string Description);

public sealed record SchemaCondition(string Path, string ExpectedValue);

public sealed record SchemaVariant(string Key, string Label, SchemaField Schema);

public enum SchemaFieldKind
{
    String,
    Integer,
    Number,
    Boolean,
    Enum,
    Object,
    Array,
    OneOf,
    Unsupported,
}
