using System.Reflection;
using System.Text.Json.Nodes;
using Json.Schema;
using YamlDotNet.RepresentationModel;

namespace Meridian.Codex.SchemaForms;

/// <summary>
/// Loads Meridian's checked-in Draft 2020-12 schemas and projects their structural
/// metadata into reusable form fields. Constraints remain owned by the schema files.
/// </summary>
public sealed class SchemaCatalog
{
    private static readonly string[] SupportedKeywords =
    [
        "$schema", "$id", "$defs", "$ref", "type", "properties", "required",
        "additionalProperties", "description", "title", "default", "const", "enum",
        "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minLength",
        "maxLength", "pattern", "items", "minItems", "maxItems", "uniqueItems", "oneOf",
        // Conditional constraints validate the document but do not alter its field shape.
        "allOf", "if", "then", "else",
    ];

    private readonly IReadOnlyDictionary<string, JsonObject> _schemas;
    private readonly JsonObject _definitions;

    public SchemaCatalog() : this(LoadEmbeddedSchemas()) { }

    internal SchemaCatalog(IReadOnlyDictionary<string, JsonObject> schemas)
    {
        _schemas = schemas;
        _definitions = new JsonObject();
        foreach (var defsName in new[] { "common.defs.yaml", "skeleton.defs.yaml" })
        {
            if (_schemas[defsName]["$defs"] is not JsonObject defs) continue;
            foreach (var (name, value) in defs) _definitions[name] = value?.DeepClone();
        }
    }

    public SchemaField GetRoot(string schemaFile)
    {
        var merged = GetMergedSchema(schemaFile);
        return Project(schemaFile[..^".schema.yaml".Length], string.Empty, merged, merged, true);
    }

    public IReadOnlyList<string> Validate(string schemaFile, string yaml)
    {
        var schema = JsonSchema.FromText(GetMergedSchema(schemaFile).ToJsonString());
        var result = schema.Evaluate(ParseYaml(yaml), new EvaluationOptions { OutputFormat = OutputFormat.List });
        if (result.IsValid) return [];
        return result.Details
            .Where(detail => !detail.IsValid && detail.Errors is not null)
            .SelectMany(detail => detail.Errors!.Values.Select(error => $"{detail.InstanceLocation}: {error}"))
            .Distinct(StringComparer.Ordinal)
            .ToArray();
    }

    private JsonObject GetMergedSchema(string schemaFile)
    {
        if (!_schemas.TryGetValue(schemaFile, out var schema))
            throw new KeyNotFoundException($"Embedded schema '{schemaFile}' was not found.");
        var merged = (JsonObject)schema.DeepClone();
        var local = merged["$defs"] as JsonObject ?? new JsonObject();
        merged["$defs"] = local;
        foreach (var (name, value) in _definitions)
            if (!local.ContainsKey(name)) local[name] = value?.DeepClone();
        return merged;
    }

    private SchemaField Project(string name, string path, JsonObject raw, JsonObject root, bool required)
    {
        var schema = Dereference(raw, root);
        var unsupported = schema.Select(pair => pair.Key)
            .Where(key => !SupportedKeywords.Contains(key, StringComparer.Ordinal))
            .ToArray();
        if (unsupported.Length > 0)
            return Unsupported(name, path, required, $"Unsupported JSON Schema keyword(s): {string.Join(", ", unsupported)}. Edit this object in YAML.");

        if (schema["oneOf"] is JsonArray variants)
            return ProjectOneOf(name, path, schema, root, required, variants);

        var choices = Strings(schema["enum"] as JsonArray);
        if (choices.Count > 0)
            return Base(SchemaFieldKind.Enum, choices);

        if (schema["const"] is JsonNode constant)
            return new SchemaField
            {
                Name = name,
                Path = path,
                Title = Humanize(name),
                Description = Text(schema, "description"),
                Kind = SchemaFieldKind.String,
                IsRequired = required,
                IsReadOnly = true,
                Constant = constant.DeepClone(),
                Default = constant.DeepClone(),
            };

        var type = Text(schema, "type") ?? InferType(schema);
        return type switch
        {
            "object" => ProjectObject(),
            "array" => ProjectArray(),
            "boolean" => Base(SchemaFieldKind.Boolean),
            "integer" => Base(SchemaFieldKind.Integer),
            "number" => Base(SchemaFieldKind.Number),
            "string" => Base(SchemaFieldKind.String),
            _ => Unsupported(name, path, required, "This schema does not declare a supported value type. Edit it in YAML."),
        };

        SchemaField Base(SchemaFieldKind kind, IReadOnlyList<string>? enumChoices = null) => new()
        {
            Name = name,
            Path = path,
            Title = Text(schema, "title") ?? Humanize(name),
            Description = Text(schema, "description"),
            Kind = kind,
            IsRequired = required,
            Default = schema["default"]?.DeepClone(),
            Minimum = Decimal(schema, "minimum") ?? Decimal(schema, "exclusiveMinimum"),
            Maximum = Decimal(schema, "maximum") ?? Decimal(schema, "exclusiveMaximum"),
            Choices = enumChoices ?? [],
        };

        SchemaField ProjectObject()
        {
            if (schema["patternProperties"] is not null || schema["propertyNames"] is not null || schema["additionalProperties"] is JsonObject)
                return Unsupported(name, path, required, "Dynamic object keys are not supported by the form renderer. Edit this object in YAML.");
            var requiredNames = Strings(schema["required"] as JsonArray).ToHashSet(StringComparer.Ordinal);
            var children = new List<SchemaField>();
            if (schema["properties"] is JsonObject properties)
                foreach (var (childName, childSchema) in properties)
                    if (childSchema is JsonObject childObject)
                        children.Add(Project(childName, Join(path, childName), childObject, root, requiredNames.Contains(childName)));
            return new SchemaField
            {
                Name = name,
                Path = path,
                Title = Text(schema, "title") ?? Humanize(name),
                Description = Text(schema, "description"),
                Kind = SchemaFieldKind.Object,
                IsRequired = required,
                Children = children,
            };
        }

        SchemaField ProjectArray()
        {
            if (schema["items"] is not JsonObject itemSchema)
                return Unsupported(name, path, required, "Tuple and untyped arrays are not supported. Edit this array in YAML.");
            return new SchemaField
            {
                Name = name,
                Path = path,
                Title = Text(schema, "title") ?? Humanize(name),
                Description = Text(schema, "description"),
                Kind = SchemaFieldKind.Array,
                IsRequired = required,
                Item = Project("item", path + "[]", itemSchema, root, true),
                Minimum = Decimal(schema, "minItems"),
                Maximum = Decimal(schema, "maxItems"),
            };
        }
    }

    private SchemaField ProjectOneOf(string name, string path, JsonObject schema, JsonObject root, bool required, JsonArray rawVariants)
    {
        var variants = new List<SchemaVariant>();
        foreach (var raw in rawVariants)
        {
            if (raw is not JsonObject branch) return Unsupported(name, path, required, "Non-object oneOf branches are not supported.");
            var projected = Project(name, path, branch, root, required);
            var discriminator = projected.Children.FirstOrDefault(child => child.Constant is not null);
            if (projected.Kind != SchemaFieldKind.Object || discriminator?.Constant is null)
                return Unsupported(name, path, required, "oneOf needs object branches with a unique const discriminator.");
            var key = discriminator.Constant.ToString();
            if (variants.Any(v => v.Key == key))
                return Unsupported(name, path, required, "oneOf discriminator values must be unique.");
            variants.Add(new SchemaVariant(key, Humanize(key), projected));
        }
        return new SchemaField
        {
            Name = name,
            Path = path,
            Title = Text(schema, "title") ?? Humanize(name),
            Description = Text(schema, "description"),
            Kind = SchemaFieldKind.OneOf,
            IsRequired = required,
            Variants = variants,
        };
    }

    private static SchemaField Unsupported(string name, string path, bool required, string reason) => new()
    {
        Name = name,
        Path = path,
        Title = Humanize(name),
        Kind = SchemaFieldKind.Unsupported,
        IsRequired = required,
        IsReadOnly = true,
        UnsupportedReason = reason,
    };

    private static JsonObject Dereference(JsonObject schema, JsonObject root)
    {
        if (schema["$ref"]?.GetValue<string>() is not { } reference) return schema;
        const string prefix = "#/$defs/";
        if (!reference.StartsWith(prefix, StringComparison.Ordinal))
            return new JsonObject { ["unsupportedReference"] = reference };
        if (root["$defs"]?[reference[prefix.Length..]] is not JsonObject resolved)
            return new JsonObject { ["missingReference"] = reference };
        var merged = (JsonObject)resolved.DeepClone();
        foreach (var (key, value) in schema)
            if (key != "$ref") merged[key] = value?.DeepClone();
        return merged;
    }

    private static string InferType(JsonObject schema) => schema["properties"] is not null ? "object" : string.Empty;
    private static string Join(string path, string name) => path.Length == 0 ? name : $"{path}.{name}";
    private static string? Text(JsonObject value, string key) => value[key]?.GetValue<string>();
    private static decimal? Decimal(JsonObject value, string key) => value[key] is JsonValue node && node.TryGetValue<decimal>(out var number) ? number : null;
    private static IReadOnlyList<string> Strings(JsonArray? values) => values?.Select(v => v?.ToString() ?? string.Empty).ToArray() ?? [];
    private static string Humanize(string value) => string.Join(' ', value.Split('_', StringSplitOptions.RemoveEmptyEntries).Select(word => char.ToUpperInvariant(word[0]) + word[1..]));

    private static IReadOnlyDictionary<string, JsonObject> LoadEmbeddedSchemas()
    {
        var assembly = typeof(SchemaCatalog).Assembly;
        var result = new Dictionary<string, JsonObject>(StringComparer.Ordinal);
        foreach (var suffix in new[] { "pack.schema.yaml", "npc.schema.yaml", "item.schema.yaml", "ability.schema.yaml", "common.defs.yaml", "skeleton.defs.yaml" })
        {
            var resource = assembly.GetManifestResourceNames().Single(name => name.EndsWith(suffix, StringComparison.Ordinal));
            using var stream = assembly.GetManifestResourceStream(resource)!;
            using var reader = new StreamReader(stream);
            result[suffix] = ParseYaml(reader.ReadToEnd()).AsObject();
        }
        return result;
    }

    internal static JsonNode ParseYaml(string yaml)
    {
        var stream = new YamlStream();
        stream.Load(new StringReader(yaml));
        return ToJson(stream.Documents[0].RootNode)!;
    }

    private static JsonNode? ToJson(YamlNode node) => node switch
    {
        YamlMappingNode map => new JsonObject(map.Children.ToDictionary(pair => ((YamlScalarNode)pair.Key).Value ?? string.Empty, pair => ToJson(pair.Value))),
        YamlSequenceNode sequence => new JsonArray(sequence.Children.Select(ToJson).ToArray()),
        YamlScalarNode scalar when scalar.Style == YamlDotNet.Core.ScalarStyle.Plain && scalar.Value is null or "null" or "~" => null,
        YamlScalarNode scalar when scalar.Style == YamlDotNet.Core.ScalarStyle.Plain && bool.TryParse(scalar.Value, out var boolean) => JsonValue.Create(boolean),
        YamlScalarNode scalar when scalar.Style == YamlDotNet.Core.ScalarStyle.Plain && decimal.TryParse(scalar.Value, System.Globalization.NumberStyles.Number, System.Globalization.CultureInfo.InvariantCulture, out var number) => JsonValue.Create(number),
        YamlScalarNode scalar => JsonValue.Create(scalar.Value),
        _ => throw new InvalidDataException($"Unsupported YAML node {node.NodeType}.")
    };
}
