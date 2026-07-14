using System.Globalization;
using System.Reflection;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
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
        YamlScalarNode scalar => Yaml11ScalarResolver.Resolve(scalar),
        _ => throw new InvalidDataException($"Unsupported YAML node type {value.NodeType}."),
    };

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

    /// <summary>
    /// YAML 1.1 core scalar resolution used by PyYAML's SafeLoader. YamlDotNet's
    /// object deserializer follows a different vocabulary (notably timestamps,
    /// yes/no/on/off, binary/sexagesimal integers, and exponent forms), so the
    /// editor resolves tags from the YAML 1.1 grammar before JSON Schema sees
    /// the instance. This is tag resolution, not a second set of pack rules.
    /// </summary>
    private static class Yaml11ScalarResolver
    {
        private static readonly Regex Boolean = new(
            "^(?:yes|no|true|false|on|off)$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Regex Null = new(
            "^(?:~|null)?$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Regex Integer = new(
            "^[-+]?(?:0b[0-1_]+|0[0-7_]+|(?:0|[1-9][0-9_]*)|0x[0-9a-f_]+|[1-9][0-9_]*(?::[0-5]?[0-9])+)$",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Regex Float = new(
            "^[-+]?(?:(?:[0-9][0-9_]*)\\.[0-9_]*(?:[eE][-+][0-9]+)?|(?:[0-9][0-9_]*)(?:[eE][-+][0-9]+)|\\.[0-9_]+(?:[eE][-+][0-9]+)?|(?:[0-9][0-9_]*)(?::[0-5]?[0-9])+\\.[0-9_]*|\\.(?:inf|nan))$",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Regex Timestamp = new(
            "^[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}(?:(?:[Tt]|[ \\t]+)[0-9]{1,2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]*)?(?:[ \\t]*(?:Z|[-+][0-9]{1,2}(?::[0-9]{2})?))?)?$",
            RegexOptions.CultureInvariant);

        public static JsonNode? Resolve(YamlScalarNode scalar)
        {
            var value = scalar.Value ?? string.Empty;
            if (scalar.Style is not ScalarStyle.Plain) return JsonValue.Create(value);
            if (Null.IsMatch(value)) return null;
            if (Boolean.IsMatch(value))
                return JsonValue.Create(value.Equals("yes", StringComparison.OrdinalIgnoreCase) ||
                    value.Equals("true", StringComparison.OrdinalIgnoreCase) ||
                    value.Equals("on", StringComparison.OrdinalIgnoreCase));
            if (Integer.IsMatch(value)) return JsonValue.Create(ParseInteger(value));
            if (Float.IsMatch(value))
            {
                // Exact magnitude is irrelevant to pack@1 (no floating fields),
                // but the JSON numeric type is essential for schema parity.
                return JsonValue.Create(0.0);
            }
            if (Timestamp.IsMatch(value))
                return new JsonObject { ["$yamlType"] = "timestamp" };
            return JsonValue.Create(value);
        }

        private static decimal ParseInteger(string value)
        {
            var normalized = value.Replace("_", string.Empty, StringComparison.Ordinal);
            var sign = 1m;
            if (normalized[0] is '-' or '+')
            {
                if (normalized[0] == '-') sign = -1m;
                normalized = normalized[1..];
            }
            try
            {
                if (normalized.StartsWith("0b", StringComparison.OrdinalIgnoreCase))
                    return sign * Convert.ToInt64(normalized[2..], 2);
                if (normalized.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                    return sign * Convert.ToInt64(normalized[2..], 16);
                if (normalized.Length > 1 && normalized[0] == '0')
                    return sign * Convert.ToInt64(normalized[1..], 8);
                if (normalized.Contains(':'))
                {
                    decimal total = 0;
                    foreach (var part in normalized.Split(':')) total = total * 60 + decimal.Parse(part, CultureInfo.InvariantCulture);
                    return sign * total;
                }
                return sign * decimal.Parse(normalized, CultureInfo.InvariantCulture);
            }
            catch (OverflowException)
            {
                return sign < 0 ? decimal.MinValue : decimal.MaxValue;
            }
        }
    }
}
