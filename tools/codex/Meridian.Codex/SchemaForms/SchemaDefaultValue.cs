using System.Text.Json.Nodes;

namespace Meridian.Codex.SchemaForms;

internal static class SchemaDefaultValue
{
    public static JsonNode Create(SchemaField field)
    {
        if (field.Constant is { } constant) return constant.DeepClone();
        if (field.Default is { } direct) return direct.DeepClone();
        if (field.Kind == SchemaFieldKind.Object)
        {
            var result = new JsonObject();
            foreach (var child in field.Children.Where(child => child.IsRequired)) result[child.Name] = Create(child);
            return result;
        }
        if (field.Kind == SchemaFieldKind.OneOf && field.Variants.FirstOrDefault() is { } variant)
            return Create(variant.Schema);
        return field.Kind switch
        {
            SchemaFieldKind.Array => new JsonArray(),
            SchemaFieldKind.Boolean => JsonValue.Create(false),
            SchemaFieldKind.Integer => JsonValue.Create((long)(field.Minimum ?? 0)),
            SchemaFieldKind.Number => JsonValue.Create(field.Minimum ?? 0),
            SchemaFieldKind.Enum => JsonValue.Create(field.Choices.FirstOrDefault() ?? string.Empty),
            _ => JsonValue.Create(string.Empty),
        };
    }
}
