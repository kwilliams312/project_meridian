using System.Text;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.Editing;

/// <summary>
/// CST-backed pack manifest. Existing manifests are reconciled with minimal CST
/// splices so comments, key order, quoting, and unrelated fields stay untouched.
/// </summary>
public sealed class PackManifestDocument
{
    private readonly string? _originalText;
    private readonly PackManifestData _original;

    private PackManifestDocument(string? path, string? originalText, PackManifestData original, PackManifestData data)
    {
        Path = path;
        _originalText = originalText;
        _original = original;
        Data = data;
    }

    public string? Path { get; private set; }
    public PackManifestData Data { get; }
    public bool IsDirty => !_original.ValueEquals(Data);

    public static PackManifestDocument Load(string path) =>
        Parse(File.ReadAllText(path, new UTF8Encoding(false)), path);

    public static PackManifestDocument Parse(string text, string? path = null)
    {
        var yaml = YamlDocument.Parse(text);
        var data = Read(yaml);
        return new PackManifestDocument(path, text, data.Copy(), data);
    }

    public static PackManifestDocument New(string namespace_, string name)
    {
        var data = new PackManifestData { Namespace = namespace_, Name = name };
        return new PackManifestDocument(null, null, data.Copy(), data);
    }

    public static PackManifestDocument New(PackManifestData values)
    {
        var data = values.Copy();
        return new PackManifestDocument(null, null, data.Copy(), data);
    }

    public string ToYaml()
    {
        if (_originalText is null)
        {
            return Emit(Data);
        }

        var yaml = YamlDocument.Parse(_originalText);
        SetScalar(yaml, "namespace", _original.Namespace, Data.Namespace);
        SetScalar(yaml, "name", _original.Name, Data.Name);
        SetOptionalScalar(yaml, "description", _original.Description, Data.Description);
        SetScalar(yaml, "version", _original.Version, Data.Version);
        SetScalar(yaml, "content_schema_version", _original.ContentSchemaVersion, Data.ContentSchemaVersion);
        SetOptionalScalar(yaml, "compatibility_version", _original.CompatibilityVersion, Data.CompatibilityVersion);
        SetScalar(yaml, "engine.godot", _original.GodotVersion, Data.GodotVersion);
        SetScalar(yaml, "license", _original.License, Data.License);

        if (!_original.Dependencies.SequenceEqual(Data.Dependencies))
        {
            var rendered = RenderDependencies(Data.Dependencies);
            if (yaml.Resolve("dependencies") is null)
            {
                yaml.AddRawKey(null, "dependencies", rendered);
            }
            else if (Data.Dependencies.Count == 0)
            {
                yaml.RemoveKey("dependencies");
            }
            else
            {
                yaml.ReplaceNode("dependencies", rendered);
            }
        }

        return yaml.ToText();
    }

    public void Save(string path)
    {
        File.WriteAllText(path, ToYaml(), new UTF8Encoding(false));
        Path = path;
    }

    private static PackManifestData Read(YamlDocument yaml)
    {
        var data = new PackManifestData
        {
            Namespace = yaml.GetValue("namespace") ?? string.Empty,
            Name = yaml.GetValue("name") ?? string.Empty,
            Description = yaml.GetValue("description") ?? string.Empty,
            Version = yaml.GetValue("version") ?? string.Empty,
            ContentSchemaVersion = yaml.GetValue("content_schema_version") ?? string.Empty,
            CompatibilityVersion = yaml.GetValue("compatibility_version") ?? "1",
            GodotVersion = yaml.GetValue("engine.godot") ?? string.Empty,
            License = yaml.GetValue("license") ?? string.Empty,
        };

        var dependencies = yaml.Resolve("dependencies");
        if (dependencies is { Kind: CstKind.Sequence })
        {
            for (var i = 0; i < dependencies.Items.Count; i++)
            {
                data.Dependencies.Add(new PackDependencyData(
                    yaml.GetValue($"dependencies[{i}].namespace") ?? string.Empty,
                    yaml.GetValue($"dependencies[{i}].version") ?? string.Empty));
            }
        }
        return data;
    }

    private static void SetScalar(YamlDocument yaml, string path, string oldValue, string newValue)
    {
        if (oldValue != newValue) yaml.SetValue(path, newValue);
    }

    private static void SetOptionalScalar(YamlDocument yaml, string path, string oldValue, string newValue)
    {
        if (oldValue == newValue) return;
        if (string.IsNullOrWhiteSpace(newValue))
        {
            if (yaml.Resolve(path) is not null) yaml.RemoveKey(path);
        }
        else if (yaml.Resolve(path) is null)
        {
            var dot = path.LastIndexOf('.');
            yaml.AddKey(dot < 0 ? null : path[..dot], dot < 0 ? path : path[(dot + 1)..], newValue);
        }
        else
        {
            yaml.SetValue(path, newValue);
        }
    }

    private static string RenderDependencies(IEnumerable<PackDependencyData> dependencies) =>
        string.Join("\n", dependencies.Select(d =>
            $"- namespace: {QuoteIfNeeded(d.Namespace)}\n  version: {QuoteIfNeeded(d.Version)}"));

    private static string Emit(PackManifestData data)
    {
        var sb = new StringBuilder();
        sb.AppendLine("schema: meridian/pack@1");
        sb.AppendLine($"namespace: {QuoteIfNeeded(data.Namespace)}");
        sb.AppendLine($"name: {QuoteIfNeeded(data.Name)}");
        if (!string.IsNullOrWhiteSpace(data.Description)) sb.AppendLine($"description: {QuoteIfNeeded(data.Description)}");
        sb.AppendLine($"version: {QuoteIfNeeded(data.Version)}");
        sb.AppendLine($"content_schema_version: {data.ContentSchemaVersion}");
        sb.AppendLine($"compatibility_version: {data.CompatibilityVersion}");
        sb.AppendLine("engine:");
        sb.AppendLine($"  godot: {QuoteIfNeeded(data.GodotVersion)}");
        if (data.Dependencies.Count > 0)
        {
            sb.AppendLine("dependencies:");
            foreach (var dependency in data.Dependencies)
            {
                sb.AppendLine($"  - namespace: {QuoteIfNeeded(dependency.Namespace)}");
                sb.AppendLine($"    version: {QuoteIfNeeded(dependency.Version)}");
            }
        }
        sb.AppendLine($"license: {QuoteIfNeeded(data.License)}");
        return sb.ToString();
    }

    private static string QuoteIfNeeded(string value)
    {
        if (value.Length > 0 && value.All(c => char.IsLetterOrDigit(c) || c is '_' or '-' or '.' or ' ')
            && value[0] != ' ' && value[^1] != ' ')
        {
            return value;
        }
        return $"\"{value.Replace("\\", "\\\\").Replace("\"", "\\\"")}\"";
    }
}
