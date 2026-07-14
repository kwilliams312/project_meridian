using System.Globalization;
using System.Text;
using System.Text.Json.Nodes;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.SchemaForms;

/// <summary>
/// Generic form state bound to a CST-backed YAML document. Edits replace only the
/// addressed scalar/collection or insert/remove one mapping entry.
/// </summary>
public sealed class SchemaFormDocument
{
    private string _yaml;
    private JsonNode _value;
    private readonly Dictionary<(string Path, string Branch), JsonObject> _branchCache = [];

    public SchemaFormDocument(SchemaField schema, string yaml)
    {
        Schema = schema;
        _yaml = yaml;
        _value = SchemaCatalog.ParseYaml(yaml);
    }

    public SchemaField Schema { get; }
    public event EventHandler? Changed;
    public string ToYaml() => _yaml;
    public JsonNode Value => _value.DeepClone();

    public JsonNode? Get(string path)
    {
        JsonNode? current = _value;
        foreach (var part in ParsePath(path))
        {
            current = part.Index is { } index ? current?[index] : current?[part.Key!];
            if (current is null) return null;
        }
        return current.DeepClone();
    }

    public void Set(string path, JsonNode? value)
    {
        if (path.Length == 0) throw new ArgumentException("The root cannot be replaced by a form scalar edit.", nameof(path));
        var current = Get(path);
        if (JsonNode.DeepEquals(current, value)) return;

        if (value is null)
        {
            var doc = YamlDocument.Parse(_yaml);
            doc.RemoveKey(path);
            Commit(doc.ToText());
            return;
        }

        var cst = YamlDocument.Parse(_yaml);
        if (cst.Resolve(path) is { Kind: CstKind.Scalar } && value is JsonValue scalar)
        {
            // String edits retain the original YAML scalar style where possible.
            // Typed JSON primitives must replace the raw node so recovery from an
            // invalid quoted edit cannot preserve quotes and change their type.
            if (scalar.TryGetValue<string>(out var text)) cst.SetValue(path, text);
            else cst.ReplaceNode(path, ScalarText(scalar));
        }
        else if (cst.Resolve(path) is not null)
            cst.ReplaceNode(path, Render(value));
        else
        {
            var (parent, name) = Split(path);
            var parentNode = parent is null ? cst.Root : cst.Resolve(parent);
            if (parentNode is { Kind: CstKind.Mapping, IsFlow: false } && parentNode.Entries.Count > 0)
                cst.AddRawKey(parent, name, RenderForInsertion(value));
            else
            {
                var updated = _value.DeepClone();
                Assign(updated, path, value.DeepClone());
                var existingPath = parent;
                while (existingPath is not null && cst.Resolve(existingPath) is null)
                    existingPath = Split(existingPath).Parent;
                if (existingPath is null)
                {
                    cst.AddRawKey(null, path.Split('.')[0], RenderForInsertion(updated[path.Split('.')[0]]!));
                }
                else
                {
                    cst.ReplaceNode(existingPath, Render(GetAt(updated, existingPath)!));
                }
            }
        }
        Commit(cst.ToText());
    }

    public void AddArrayItem(string path, JsonNode? value = null)
    {
        var array = Get(path) as JsonArray ?? new JsonArray();
        array.Add(value?.DeepClone() ?? new JsonObject());
        Set(path, array);
    }

    public void RemoveArrayItem(string path, int index)
    {
        var array = Get(path) as JsonArray ?? throw new ArgumentException($"'{path}' is not an array.");
        array.RemoveAt(index);
        Set(path, array);
    }

    public void MoveArrayItem(string path, int oldIndex, int newIndex)
    {
        var array = Get(path) as JsonArray ?? throw new ArgumentException($"'{path}' is not an array.");
        if (oldIndex == newIndex) return;
        var item = array[oldIndex]?.DeepClone();
        array.RemoveAt(oldIndex);
        array.Insert(newIndex, item);
        Set(path, array);
    }

    /// <summary>
    /// Changes a discriminated oneOf branch. Returns an actionable warning rather
    /// than deleting branch-only values unless the caller explicitly confirms.
    /// </summary>
    public BranchChangeResult SelectBranch(string path, SchemaField oneOf, string branchKey, bool confirmDestructive = false)
    {
        var target = oneOf.Variants.SingleOrDefault(v => v.Key == branchKey)
            ?? throw new ArgumentException($"Unknown oneOf branch '{branchKey}'.", nameof(branchKey));
        var current = Get(path) as JsonObject ?? new JsonObject();
        var allowed = target.Schema.Children.Select(child => child.Name).ToHashSet(StringComparer.Ordinal);
        var discarded = current.Select(pair => pair.Key).Where(key => !allowed.Contains(key)).ToArray();
        if (discarded.Length > 0 && !confirmDestructive)
            return new(false, $"Changing to {target.Label} removes {string.Join(", ", discarded)}. Confirm to continue.", discarded);

        var currentBranch = oneOf.Variants.FirstOrDefault(variant =>
            variant.Schema.Children.Any(child => child.Constant is not null && child.Constant.ToString() == current[child.Name]?.ToString()));
        if (currentBranch is not null) _branchCache[(path, currentBranch.Key)] = (JsonObject)current.DeepClone();

        var next = _branchCache.TryGetValue((path, branchKey), out var cached)
            ? (JsonObject)cached.DeepClone()
            : new JsonObject();
        foreach (var child in target.Schema.Children)
        {
            if (child.Constant is { } constant) next[child.Name] = constant.DeepClone();
            else if (current[child.Name] is { } common && allowed.Contains(child.Name)) next[child.Name] = common.DeepClone();
            else if (next[child.Name] is null && child.Default is { } defaultValue) next[child.Name] = defaultValue.DeepClone();
            else if (next[child.Name] is null && child.IsRequired) next[child.Name] = SchemaDefaultValue.Create(child);
        }
        Set(path, next);
        return new(true, null, []);
    }

    private void Commit(string yaml)
    {
        JsonNode parsed;
        try
        {
            parsed = SchemaCatalog.ParseYaml(yaml);
        }
        catch (Exception ex)
        {
            throw new InvalidDataException("The CST edit produced YAML that could not be reparsed; the document was not changed.", ex);
        }
        _yaml = yaml;
        _value = parsed;
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private static string ScalarText(JsonNode value)
    {
        if (value is JsonValue scalar)
        {
            if (scalar.TryGetValue<string>(out var text)) return text;
            // JSON primitive syntax is valid YAML and covers every signed and
            // unsigned integral CLR type as well as decimal/floating/bool values.
            return scalar.ToJsonString();
        }
        return value.ToString();
    }

    internal static string Render(JsonNode node, int indent = 0)
    {
        var sb = new StringBuilder();
        RenderInto(sb, node, indent, false);
        return sb.ToString().TrimEnd('\n');
    }

    private static string RenderForInsertion(JsonNode node)
    {
        var rendered = Render(node);
        // AddRawKey distinguishes scalar from block content by the presence of a
        // newline. A one-member mapping/array is still block content.
        return node is JsonObject or JsonArray && !rendered.Contains('\n', StringComparison.Ordinal)
            ? rendered + "\n"
            : rendered;
    }

    private static void RenderInto(StringBuilder sb, JsonNode? node, int indent, bool sequenceItem)
    {
        var pad = new string(' ', indent);
        switch (node)
        {
            case JsonObject obj when obj.Count == 0:
                sb.Append("{}");
                break;
            case JsonObject obj:
                var first = true;
                foreach (var (key, value) in obj)
                {
                    if (!first || !sequenceItem) sb.Append(pad);
                    sb.Append(key).Append(':');
                    if (value is JsonObject or JsonArray)
                    {
                        sb.Append('\n');
                        RenderInto(sb, value, indent + 2, false);
                    }
                    else sb.Append(' ').Append(RenderScalar(value)).Append('\n');
                    first = false;
                }
                break;
            case JsonArray array when array.Count == 0:
                sb.Append("[]");
                break;
            case JsonArray array:
                foreach (var item in array)
                {
                    sb.Append(pad).Append("- ");
                    if (item is JsonObject)
                    {
                        RenderInto(sb, item, indent + 2, true);
                    }
                    else if (item is JsonArray)
                    {
                        sb.Append('\n');
                        RenderInto(sb, item, indent + 2, false);
                    }
                    else sb.Append(RenderScalar(item)).Append('\n');
                }
                break;
            default:
                sb.Append(RenderScalar(node));
                break;
        }
    }

    private static string RenderScalar(JsonNode? node)
    {
        if (node is null) return "null";
        if (node is JsonValue value)
        {
            if (value.TryGetValue<string>(out var text)) return YamlDocument.RenderString(text);
            return value.ToJsonString();
        }
        return YamlDocument.RenderString(node.ToString());
    }

    private static (string? Parent, string Name) Split(string path)
    {
        var dot = path.LastIndexOf('.');
        return dot < 0 ? (null, path) : (path[..dot], path[(dot + 1)..]);
    }

    private static IEnumerable<(string? Key, int? Index)> ParsePath(string path)
    {
        foreach (var component in path.Split('.', StringSplitOptions.RemoveEmptyEntries))
        {
            var bracket = component.IndexOf('[');
            if (bracket < 0) yield return (component, null);
            else
            {
                if (bracket > 0) yield return (component[..bracket], null);
                yield return (null, int.Parse(component[(bracket + 1)..^1], CultureInfo.InvariantCulture));
            }
        }
    }

    private static JsonNode? GetAt(JsonNode root, string path)
    {
        JsonNode? current = root;
        foreach (var part in ParsePath(path)) current = part.Index is { } index ? current?[index] : current?[part.Key!];
        return current;
    }

    private static void Assign(JsonNode root, string path, JsonNode value)
    {
        var parts = ParsePath(path).ToArray();
        JsonNode current = root;
        for (var i = 0; i < parts.Length - 1; i++)
        {
            var part = parts[i];
            if (part.Index is { } index) current = current[index] ?? throw new InvalidOperationException($"Missing array item in '{path}'.");
            else
            {
                if (current[part.Key!] is null) current[part.Key!] = new JsonObject();
                current = current[part.Key!]!;
            }
        }
        var last = parts[^1];
        if (last.Index is { } lastIndex) current[lastIndex] = value;
        else current[last.Key!] = value;
    }
}

public sealed record BranchChangeResult(bool Changed, string? Warning, IReadOnlyList<string> DestructiveFields);
