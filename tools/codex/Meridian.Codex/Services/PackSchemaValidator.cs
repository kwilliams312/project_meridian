using System.Globalization;
using System.Reflection;
using System.Text.Json.Nodes;
using Json.Schema;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;

namespace Meridian.Codex.Services;

/// <summary>
/// Evaluates pack manifests against Meridian's authoritative Draft 2020-12
/// schema contract. The resource merge deliberately mirrors
/// <c>validate_content.py::load_schemas</c>; constraints are never restated in C#.
/// </summary>
internal static class PackSchemaValidator
{
    private static readonly Lazy<JsonSchema> Schema = new(LoadSchema);

    public static IReadOnlyList<WorkspaceDiagnostic> Validate(string yaml)
    {
        JsonNode? instance;
        try
        {
            instance = ParseYaml(yaml);
        }
        catch (Exception ex)
        {
            return [new("pack.yaml", $"Invalid YAML: {ex.Message}")];
        }

        var result = Schema.Value.Evaluate(instance, new EvaluationOptions
        {
            OutputFormat = OutputFormat.List,
        });
        if (result.IsValid) return [];

        var diagnostics = new List<WorkspaceDiagnostic>();
        foreach (var detail in result.Details.Where(d => !d.IsValid && d.Errors is not null))
        {
            var field = ToField(detail.InstanceLocation.ToString());
            foreach (var error in detail.Errors!.Values)
            {
                diagnostics.Add(new(field, error));
            }
        }

        return diagnostics.Count > 0
            ? diagnostics
            : [new("pack.yaml", "Manifest does not satisfy meridian/pack@1.")];
    }

    private static JsonSchema LoadSchema()
    {
        var schema = ParseYaml(ReadResource("pack.schema.yaml"))!.AsObject();
        var common = ParseYaml(ReadResource("common.defs.yaml"))!.AsObject();
        var skeleton = ParseYaml(ReadResource("skeleton.defs.yaml"))!.AsObject();
        var definitions = schema["$defs"] as JsonObject ?? new JsonObject();
        schema["$defs"] = definitions;
        MergeDefinitions(definitions, common);
        MergeDefinitions(definitions, skeleton);
        return JsonSchema.FromText(schema.ToJsonString());
    }

    private static void MergeDefinitions(JsonObject destination, JsonObject source)
    {
        if (source["$defs"] is not JsonObject definitions) return;
        foreach (var (name, definition) in definitions)
        {
            destination[name] = definition?.DeepClone();
        }
    }

    private static string ReadResource(string suffix)
    {
        var assembly = typeof(PackSchemaValidator).Assembly;
        var name = assembly.GetManifestResourceNames().Single(n => n.EndsWith(suffix, StringComparison.Ordinal));
        using var stream = assembly.GetManifestResourceStream(name)
            ?? throw new InvalidOperationException($"Embedded schema resource '{suffix}' is missing.");
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }

    private static JsonNode? ParseYaml(string yaml)
    {
        var stream = new YamlStream();
        stream.Load(new StringReader(yaml));
        return stream.Documents.Count == 0 ? null : ToJsonNode(stream.Documents[0].RootNode);
    }

    private static JsonNode? ToJsonNode(YamlNode value) => value switch
    {
        YamlMappingNode map => new JsonObject(map.Children.ToDictionary(
            pair => ((YamlScalarNode)pair.Key).Value ?? string.Empty,
            pair => ToJsonNode(pair.Value))),
        YamlSequenceNode sequence => new JsonArray(sequence.Children.Select(ToJsonNode).ToArray()),
        YamlScalarNode scalar => ToScalar(scalar),
        _ => throw new InvalidDataException($"Unsupported YAML node type {value.NodeType}."),
    };

    // YamlDotNet's representation model intentionally exposes lexemes rather
    // than applying a schema. Resolve the YAML core scalar types that JSON
    // Schema observes; quoted/block scalars always remain strings.
    private static JsonNode? ToScalar(YamlScalarNode scalar)
    {
        var value = scalar.Value ?? string.Empty;
        if (scalar.Style is not ScalarStyle.Plain) return JsonValue.Create(value);
        if (value is "~" or "null" or "Null" or "NULL") return null;
        if (value is "true" or "True" or "TRUE" or "yes" or "Yes" or "YES" or "on" or "On" or "ON")
            return JsonValue.Create(true);
        if (value is "false" or "False" or "FALSE" or "no" or "No" or "NO" or "off" or "Off" or "OFF")
            return JsonValue.Create(false);
        if (long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var integer))
            return JsonValue.Create(integer);
        if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var number))
            return JsonValue.Create(number);
        return JsonValue.Create(value);
    }

    private static string ToField(string instanceLocation)
    {
        var path = instanceLocation.TrimStart('/');
        if (path.Length == 0) return "pack.yaml";
        if (path == "engine/godot") return "GodotVersion";
        var parts = path.Split('/').Select(p => p.Replace("~1", "/").Replace("~0", "~")).ToArray();
        return string.Join('.', parts.Select(p => int.TryParse(p, out _) ? $"[{p}]" : ToPascalCase(p)))
            .Replace(".[", "[");
    }

    private static string ToPascalCase(string value) => string.Concat(value
        .Split('_', StringSplitOptions.RemoveEmptyEntries)
        .Select(part => char.ToUpperInvariant(part[0]) + part[1..]));
}
