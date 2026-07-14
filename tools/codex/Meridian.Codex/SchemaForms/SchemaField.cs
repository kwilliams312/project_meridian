using System.Text.Json.Nodes;

namespace Meridian.Codex.SchemaForms;

/// <summary>A UI-neutral form description projected from the authoritative JSON Schema.</summary>
public sealed class SchemaField
{
    public required string Name { get; init; }
    public required string Path { get; init; }
    public string Title { get; init; } = string.Empty;
    public string? Description { get; init; }
    public SchemaFieldKind Kind { get; init; }
    public bool IsRequired { get; init; }
    public bool IsReadOnly { get; init; }
    public string? UnsupportedReason { get; init; }
    public JsonNode? Default { get; init; }
    public JsonNode? Constant { get; init; }
    public decimal? Minimum { get; init; }
    public decimal? Maximum { get; init; }
    public IReadOnlyList<string> Choices { get; init; } = [];
    public IReadOnlyList<SchemaField> Children { get; init; } = [];
    public SchemaField? Item { get; init; }
    public IReadOnlyList<SchemaVariant> Variants { get; init; } = [];
}

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
