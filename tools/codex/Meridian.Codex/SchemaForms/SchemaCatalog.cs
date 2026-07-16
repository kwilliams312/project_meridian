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
    private static readonly string[] ManifestKeys = ["schema", "schemas"];
    private static readonly string[] ManifestSchemaKeys = ["schema_file", "schema_id", "content_schema", "fields"];
    private static readonly string[] ManifestFieldKeys = ["path", "ui", "asset"];
    private static readonly string[] ManifestUiKeys = ["group", "label", "widget", "unit", "reference_type", "help", "example", "constraint", "documentation"];
    private static readonly string[] ManifestAssetKeys = ["allowed_classes", "eligible_generators"];
    private static readonly string[] SupportedKeywords =
    [
        "$schema", "$id", "$defs", "$ref", "type", "properties", "required",
        "additionalProperties", "description", "title", "default", "const", "enum",
        "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minLength",
        "maxLength", "pattern", "items", "minItems", "maxItems", "uniqueItems", "oneOf",
        "x-meridian-ui", "x-meridian-asset",
        // Conditional constraints validate the document but do not alter its field shape.
        "allOf", "if", "then", "else",
    ];

    private readonly IReadOnlyDictionary<string, JsonObject> _schemas;
    private readonly JsonObject _definitions;
    private readonly IReadOnlyDictionary<string, IReadOnlyDictionary<string, JsonObject>> _descriptors;

    public SchemaCatalog() : this(LoadEmbeddedSchemas(), LoadEmbeddedDescriptorManifest()) { }

    internal SchemaCatalog(IReadOnlyDictionary<string, JsonObject> schemas) : this(schemas, EmptyDescriptorManifest()) { }

    internal SchemaCatalog(IReadOnlyDictionary<string, JsonObject> schemas, JsonObject descriptorManifest)
    {
        _schemas = schemas;
        _descriptors = ParseDescriptors(descriptorManifest);
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
        return Project(
            schemaFile,
            schemaFile[..^".schema.yaml".Length],
            string.Empty,
            merged,
            merged,
            true,
            CollectConditionalRequirements(merged),
            availabilityCondition: null);
    }

    public IReadOnlyList<SchemaDiagnostic> Validate(string schemaFile, string yaml)
    {
        try
        {
            var schema = JsonSchema.FromText(GetMergedSchema(schemaFile).ToJsonString());
            var result = schema.Evaluate(ParseYaml(yaml), new EvaluationOptions { OutputFormat = OutputFormat.List });
            if (result.IsValid) return [];
            return result.Details
                .Where(detail => !detail.IsValid && detail.Errors is not null)
                .SelectMany(detail => detail.Errors!.Values.Select(error =>
                    new SchemaDiagnostic(ToFormPath(detail.InstanceLocation.ToString()), error)))
                .Distinct()
                .ToArray();
        }
        catch (Exception ex)
        {
            return [new SchemaDiagnostic(string.Empty, $"Validation could not complete: {ex.Message}")];
        }
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

    private SchemaField Project(
        string schemaFile,
        string name,
        string path,
        JsonObject raw,
        JsonObject root,
        bool required,
        IReadOnlyDictionary<string, IReadOnlyList<SchemaConditionalRequirement>> conditionalRequirements,
        string? availabilityCondition)
    {
        var schema = Dereference(raw, root);
        var descriptor = GetDescriptor(schemaFile, path);
        var ui = ProjectUi(descriptor);
        var asset = ProjectAsset(descriptor);
        var unsupported = schema.Select(pair => pair.Key)
            .Where(key => !SupportedKeywords.Contains(key, StringComparer.Ordinal))
            .ToArray();
        if (unsupported.Length > 0)
            return Unsupported(name, path, required, $"Unsupported JSON Schema keyword(s): {string.Join(", ", unsupported)}. Edit this object in YAML.", ui, asset);

        // A discriminated oneOf of variant OBJECTS is a tagged union (ProjectOneOf).
        // A oneOf that sits ALONGSIDE `properties` is a validation-only CONSTRAINT on a
        // single object (e.g. npc@2 visual's model-XOR-appearance required/not pair) —
        // the field is the object; the oneOf just narrows which properties may co-occur.
        // Project it as the object (below); the constraint validates the document but
        // does not change the form's field shape (same posture as allOf/if/then/else).
        if (schema["oneOf"] is JsonArray variants && schema["properties"] is null)
            return ProjectOneOf(schemaFile, name, path, schema, root, required, variants, ui, asset, conditionalRequirements, availabilityCondition);

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
                Ui = ui,
                Asset = asset,
                Kind = SchemaFieldKind.String,
                IsRequired = required,
                ConditionalRequirements = conditionalRequirements.GetValueOrDefault(path) ?? [],
                AvailabilityCondition = availabilityCondition,
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
            _ => Unsupported(name, path, required, "This schema does not declare a supported value type. Edit it in YAML.", ui, asset),
        };

        SchemaField Base(SchemaFieldKind kind, IReadOnlyList<string>? enumChoices = null) => new()
        {
            Name = name,
            Path = path,
            Title = Text(schema, "title") ?? Humanize(name),
            Description = Text(schema, "description"),
            Ui = ui,
            Asset = asset,
            Kind = kind,
            IsRequired = required,
            ConditionalRequirements = conditionalRequirements.GetValueOrDefault(path) ?? [],
            AvailabilityCondition = availabilityCondition,
            Default = schema["default"]?.DeepClone(),
            Minimum = Decimal(schema, "minimum") ?? Decimal(schema, "exclusiveMinimum"),
            Maximum = Decimal(schema, "maximum") ?? Decimal(schema, "exclusiveMaximum"),
            HasExclusiveMinimum = schema["exclusiveMinimum"] is not null,
            HasExclusiveMaximum = schema["exclusiveMaximum"] is not null,
            MinimumLength = Integer(schema, "minLength"),
            MaximumLength = Integer(schema, "maxLength"),
            Pattern = Text(schema, "pattern"),
            Choices = enumChoices ?? [],
        };

        SchemaField ProjectObject()
        {
            if (schema["patternProperties"] is not null || schema["propertyNames"] is not null || schema["additionalProperties"] is JsonObject)
                return Unsupported(name, path, required, "Dynamic object keys are not supported by the form renderer. Edit this object in YAML.", ui, asset);
            var requiredNames = Strings(schema["required"] as JsonArray).ToHashSet(StringComparer.Ordinal);
            var children = new List<SchemaField>();
            if (schema["properties"] is JsonObject properties)
                foreach (var (childName, childSchema) in properties)
                    if (childSchema is JsonObject childObject)
                        children.Add(Project(schemaFile, childName, Join(path, childName), childObject, root, requiredNames.Contains(childName), conditionalRequirements, availabilityCondition));
            return new SchemaField
            {
                Name = name,
                Path = path,
                Title = Text(schema, "title") ?? Humanize(name),
                Description = Text(schema, "description"),
                Ui = ui,
                Asset = asset,
                Kind = SchemaFieldKind.Object,
                IsRequired = required,
                ConditionalRequirements = conditionalRequirements.GetValueOrDefault(path) ?? [],
                AvailabilityCondition = availabilityCondition,
                Children = children,
            };
        }

        SchemaField ProjectArray()
        {
            if (schema["items"] is not JsonObject itemSchema)
                return Unsupported(name, path, required, "Tuple and untyped arrays are not supported. Edit this array in YAML.", ui, asset);
            return new SchemaField
            {
                Name = name,
                Path = path,
                Title = Text(schema, "title") ?? Humanize(name),
                Description = Text(schema, "description"),
                Ui = ui,
                Asset = asset,
                Kind = SchemaFieldKind.Array,
                IsRequired = required,
                ConditionalRequirements = conditionalRequirements.GetValueOrDefault(path) ?? [],
                AvailabilityCondition = availabilityCondition,
                Item = Project(schemaFile, "item", path + "[]", itemSchema, root, true, conditionalRequirements, availabilityCondition),
                Minimum = Decimal(schema, "minItems"),
                Maximum = Decimal(schema, "maxItems"),
            };
        }
    }

    private SchemaField ProjectOneOf(string schemaFile, string name, string path, JsonObject schema, JsonObject root, bool required, JsonArray rawVariants, SchemaUiDescriptor? ui, SchemaAssetDescriptor? asset, IReadOnlyDictionary<string, IReadOnlyList<SchemaConditionalRequirement>> conditionalRequirements, string? availabilityCondition)
    {
        var variants = new List<SchemaVariant>();
        foreach (var raw in rawVariants)
        {
            if (raw is not JsonObject branch) return Unsupported(name, path, required, "Non-object oneOf branches are not supported.", ui, asset);
            var discriminatorEntry = FindConstDiscriminator(Dereference(branch, root), root);
            if (discriminatorEntry is null)
                return Unsupported(name, path, required, "oneOf needs object branches with a unique const discriminator.", ui, asset);
            var (discriminatorName, key) = discriminatorEntry.Value;
            var branchCondition = $"{HumanizeCondition(discriminatorName)} is {Humanize(key)}";
            var projected = Project(schemaFile, name, path, branch, root, required, conditionalRequirements, branchCondition);
            var projectedDiscriminator = projected.Children.FirstOrDefault(child => child.Constant is not null);
            if (projected.Kind != SchemaFieldKind.Object || projectedDiscriminator?.Constant is null)
                return Unsupported(name, path, required, "oneOf needs object branches with a unique const discriminator.", ui, asset);
            if (variants.Any(v => v.Key == key))
                return Unsupported(name, path, required, "oneOf discriminator values must be unique.", ui, asset);
            variants.Add(new SchemaVariant(key, Humanize(key), projected));
        }
        return new SchemaField
        {
            Name = name,
            Path = path,
            Title = Text(schema, "title") ?? Humanize(name),
            Description = Text(schema, "description"),
            Ui = ui,
            Asset = asset,
            Kind = SchemaFieldKind.OneOf,
            IsRequired = required,
            ConditionalRequirements = conditionalRequirements.GetValueOrDefault(path) ?? [],
            AvailabilityCondition = availabilityCondition,
            Variants = variants,
        };
    }

    private static SchemaField Unsupported(string name, string path, bool required, string reason, SchemaUiDescriptor? ui = null, SchemaAssetDescriptor? asset = null) => new()
    {
        Name = name,
        Path = path,
        Title = Humanize(name),
        Ui = ui,
        Asset = asset,
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
    private static int? Integer(JsonObject value, string key) => value[key] is JsonValue node && node.TryGetValue<int>(out var number) ? number : null;
    private static IReadOnlyList<string> Strings(JsonArray? values) => values?.Select(v => v?.ToString() ?? string.Empty).ToArray() ?? [];
    private static string Humanize(string value) => string.Join(' ', value.Split('_', StringSplitOptions.RemoveEmptyEntries).Select(word => char.ToUpperInvariant(word[0]) + word[1..]));
    private static string HumanizeCondition(string value)
    {
        var text = Humanize(value);
        return text.Length == 0 ? text : text[..1] + text[1..].ToLowerInvariant();
    }

    private static (string Name, string Key)? FindConstDiscriminator(JsonObject schema, JsonObject root)
    {
        if (schema["properties"] is not JsonObject properties) return null;
        foreach (var (name, node) in properties)
            if (node is JsonObject candidate && Dereference(candidate, root)["const"] is JsonNode constant)
                return (name, constant.ToString());
        return null;
    }

    private static IReadOnlyDictionary<string, IReadOnlyList<SchemaConditionalRequirement>> CollectConditionalRequirements(JsonObject root)
    {
        var requirements = new Dictionary<string, List<SchemaConditionalRequirement>>(StringComparer.Ordinal);
        Collect(root, string.Empty);
        return requirements.ToDictionary(
            pair => pair.Key,
            pair => (IReadOnlyList<SchemaConditionalRequirement>)pair.Value,
            StringComparer.Ordinal);

        void Collect(JsonObject schema, string path)
        {
            if (schema["allOf"] is JsonArray clauses)
                foreach (var clauseNode in clauses)
                {
                    if (clauseNode is not JsonObject clause ||
                        clause["if"]?["properties"] is not JsonObject conditions ||
                        clause["then"]?["required"] is not JsonArray requiredNames)
                        continue;
                    var projectedConditions = conditions
                        .Select(pair => pair.Value is JsonObject value && value["const"] is JsonNode constant
                            ? new SchemaCondition(Join(path, pair.Key), constant.ToString())
                            : null)
                        .OfType<SchemaCondition>()
                        .ToArray();
                    if (projectedConditions.Length == 0) continue;
                    var description = string.Join(" and ", projectedConditions.Select(condition =>
                        $"{HumanizeCondition(condition.Path.Split('.').Last())} is {Humanize(condition.ExpectedValue)}"));
                    foreach (var requiredName in Strings(requiredNames))
                    {
                        var requiredPath = Join(path, requiredName);
                        if (!requirements.TryGetValue(requiredPath, out var reasons))
                            requirements[requiredPath] = reasons = [];
                        reasons.Add(new SchemaConditionalRequirement(projectedConditions, description));
                    }
                }

            if (schema["properties"] is not JsonObject properties) return;
            foreach (var (name, node) in properties)
                if (node is JsonObject child)
                    Collect(child, Join(path, name));
        }
    }

    private JsonObject? GetDescriptor(string schemaFile, string path) =>
        _descriptors.TryGetValue(schemaFile, out var fields) && fields.TryGetValue(path, out var descriptor)
            ? descriptor
            : null;

    private static SchemaUiDescriptor? ProjectUi(JsonObject? descriptor)
    {
        if (descriptor?["ui"] is not JsonObject ui) return null;
        return new SchemaUiDescriptor(
            Text(ui, "group"),
            Text(ui, "label"),
            Text(ui, "widget"),
            Text(ui, "unit"),
            Text(ui, "reference_type"),
            Text(ui, "help"),
            ui["example"]?.DeepClone(),
            Text(ui, "constraint"),
            Text(ui, "documentation"));
    }

    private static SchemaAssetDescriptor? ProjectAsset(JsonObject? descriptor)
    {
        if (descriptor?["asset"] is not JsonObject asset) return null;
        return new SchemaAssetDescriptor(
            Strings(asset["allowed_classes"] as JsonArray),
            Strings(asset["eligible_generators"] as JsonArray));
    }

    private static IReadOnlyDictionary<string, IReadOnlyDictionary<string, JsonObject>> ParseDescriptors(JsonObject manifest)
    {
        EnsureKnownKeys(manifest, ManifestKeys, "manifest");
        if (OptionalString(manifest, "schema", "manifest") != "meridian/codex-form-descriptors@1")
            throw new InvalidDataException("Unsupported or missing Codex form-descriptor manifest version.");
        if (manifest["schemas"] is not JsonArray schemas)
            throw new InvalidDataException("Codex form-descriptor manifest has no schemas array.");

        var result = new Dictionary<string, IReadOnlyDictionary<string, JsonObject>>(StringComparer.Ordinal);
        foreach (var node in schemas)
        {
            if (node is not JsonObject schema)
                throw new InvalidDataException("Codex form-descriptor schemas entries must be objects.");
            EnsureKnownKeys(schema, ManifestSchemaKeys, "schema entry");
            if (OptionalString(schema, "schema_file", "schema entry") is not { Length: > 0 } schemaFile)
                throw new InvalidDataException("Codex form-descriptor schema entry is missing schema_file.");
            OptionalString(schema, "schema_id", $"schema '{schemaFile}'");
            OptionalString(schema, "content_schema", $"schema '{schemaFile}'");
            if (schema["fields"] is not JsonArray fields)
                throw new InvalidDataException($"Codex form-descriptor entry '{schemaFile}' has no fields array.");

            var byPath = new Dictionary<string, JsonObject>(StringComparer.Ordinal);
            foreach (var fieldNode in fields)
            {
                if (fieldNode is not JsonObject field)
                    throw new InvalidDataException($"Codex form-descriptor entry '{schemaFile}' fields must be objects.");
                if (field["path"] is not JsonValue pathNode || !pathNode.TryGetValue<string>(out var path))
                    throw new InvalidDataException($"Codex form-descriptor entry '{schemaFile}' has a field without a string path.");
                var location = $"{schemaFile}:{(path.Length == 0 ? "<root>" : path)}";
                EnsureKnownKeys(field, ManifestFieldKeys, location);
                ValidateFieldDescriptor(field, location);
                if (!byPath.TryAdd(path, field))
                    throw new InvalidDataException($"Codex form-descriptor entry '{schemaFile}' repeats path '{path}'.");
            }
            if (!result.TryAdd(schemaFile, byPath))
                throw new InvalidDataException($"Codex form-descriptor manifest repeats schema '{schemaFile}'.");
        }
        return result;
    }

    private static void ValidateFieldDescriptor(JsonObject field, string location)
    {
        var hasUi = field.ContainsKey("ui");
        var hasAsset = field.ContainsKey("asset");
        if (!hasUi && !hasAsset)
            throw new InvalidDataException($"Codex form-descriptor {location}: field must contain ui or asset metadata.");

        if (hasUi)
        {
            if (field["ui"] is not JsonObject ui)
                throw new InvalidDataException($"Codex form-descriptor {location}: ui must be an object.");
            EnsureKnownKeys(ui, ManifestUiKeys, location, "ui");
            foreach (var key in ManifestUiKeys.Where(key => key != "example"))
                OptionalString(ui, key, location, "ui");
            if (OptionalString(ui, "documentation", location, "ui") is { } documentation && !IsValidDocumentation(documentation))
                throw new InvalidDataException($"Codex form-descriptor {location}: ui.documentation must be a repository docs path or HTTPS URL.");
            if (ui.ContainsKey("example") && ui["example"] is JsonObject or JsonArray)
                throw new InvalidDataException($"Codex form-descriptor {location}: ui.example must be a scalar.");
        }

        if (hasAsset)
        {
            if (field["asset"] is not JsonObject asset)
                throw new InvalidDataException($"Codex form-descriptor {location}: asset must be an object.");
            EnsureKnownKeys(asset, ManifestAssetKeys, location, "asset");
            var allowed = StringArray(asset, "allowed_classes", location, "asset", required: true);
            if (allowed.Count == 0)
                throw new InvalidDataException($"Codex form-descriptor {location}: asset.allowed_classes must not be empty.");
            StringArray(asset, "eligible_generators", location, "asset", required: false);
        }
    }

    private static void EnsureKnownKeys(JsonObject value, IReadOnlyList<string> allowed, string location, string? prefix = null)
    {
        var unknown = value.Select(pair => pair.Key)
            .Where(key => !allowed.Contains(key, StringComparer.Ordinal))
            .Order(StringComparer.Ordinal)
            .ToArray();
        if (unknown.Length == 0) return;
        var qualified = unknown.Select(key => prefix is null ? key : $"{prefix}.{key}");
        throw new InvalidDataException($"Codex form-descriptor {location}: unknown key(s): {string.Join(", ", qualified)}.");
    }

    private static string? OptionalString(JsonObject value, string key, string location, string? prefix = null)
    {
        if (!value.ContainsKey(key)) return null;
        if (value[key] is JsonValue node && node.TryGetValue<string>(out var text)) return text;
        var qualified = prefix is null ? key : $"{prefix}.{key}";
        throw new InvalidDataException($"Codex form-descriptor {location}: {qualified} must be a string.");
    }

    private static bool IsValidDocumentation(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Split('/').Contains("..", StringComparer.Ordinal)) return false;
        if (value.StartsWith("docs/", StringComparison.Ordinal) || value.StartsWith("schema/content/README.md", StringComparison.Ordinal)) return true;
        return Uri.TryCreate(value, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps && !string.IsNullOrWhiteSpace(uri.Host);
    }

    private static IReadOnlyList<string> StringArray(JsonObject value, string key, string location, string prefix, bool required)
    {
        if (!value.ContainsKey(key))
        {
            if (!required) return [];
            throw new InvalidDataException($"Codex form-descriptor {location}: {prefix}.{key} is required.");
        }
        if (value[key] is not JsonArray array)
            throw new InvalidDataException($"Codex form-descriptor {location}: {prefix}.{key} must be an array of strings.");
        var strings = new List<string>();
        foreach (var item in array)
        {
            if (item is not JsonValue node || !node.TryGetValue<string>(out var text))
                throw new InvalidDataException($"Codex form-descriptor {location}: {prefix}.{key} must be an array of strings.");
            strings.Add(text);
        }
        if (strings.Count != strings.Distinct(StringComparer.Ordinal).Count())
            throw new InvalidDataException($"Codex form-descriptor {location}: {prefix}.{key} values must be unique.");
        return strings;
    }

    private static JsonObject EmptyDescriptorManifest() => new()
    {
        ["schema"] = "meridian/codex-form-descriptors@1",
        ["schemas"] = new JsonArray(),
    };

    private static string ToFormPath(string pointer)
    {
        var parts = pointer.TrimStart('/').Split('/', StringSplitOptions.RemoveEmptyEntries)
            .Select(part => part.Replace("~1", "/", StringComparison.Ordinal).Replace("~0", "~", StringComparison.Ordinal));
        var path = string.Empty;
        foreach (var part in parts)
        {
            if (int.TryParse(part, out _)) path += $"[{part}]";
            else path += path.Length == 0 ? part : $".{part}";
        }
        return path;
    }

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

    private static JsonObject LoadEmbeddedDescriptorManifest()
    {
        var assembly = typeof(SchemaCatalog).Assembly;
        const string resourceName = "Meridian.Codex.FormDescriptors.g.json";
        using var stream = assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidDataException($"Embedded form-descriptor manifest '{resourceName}' was not found.");
        return JsonNode.Parse(stream)?.AsObject()
            ?? throw new InvalidDataException("Embedded form-descriptor manifest is empty.");
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

public sealed record SchemaDiagnostic(string Path, string Message);
