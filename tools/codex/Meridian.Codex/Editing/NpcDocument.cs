using System.Text;
using Meridian.Codex.Models;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.Editing;

/// <summary>
/// An open NPC document in the editor: the editable <see cref="NpcData"/> plus the
/// provenance needed to save it without corrupting the file. For a loaded file that
/// is the original source text and a snapshot of its parsed values, so a save is a
/// surgical reconcile; for a new NPC there is no source, so a save emits a fresh
/// canonical document.
/// </summary>
public sealed class NpcDocument
{
    private readonly string? _originalText;
    private readonly NpcData _original;

    private NpcDocument(string? path, string? originalText, NpcData original, NpcData data)
    {
        Path = path;
        _originalText = originalText;
        _original = original;
        Data = data;
    }

    /// <summary>The file path this document was loaded from / last saved to (null until saved).</summary>
    public string? Path { get; private set; }

    /// <summary>The editable NPC field values.</summary>
    public NpcData Data { get; }

    /// <summary>True when this document has no source file yet (created via <see cref="NewNpc"/>).</summary>
    public bool IsNew => _originalText is null;

    /// <summary>Load an existing NPC YAML file into an editable document.</summary>
    public static NpcDocument Load(string path)
    {
        var text = File.ReadAllText(path, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        return Parse(text, path);
    }

    /// <summary>Parse NPC YAML text into an editable document (source-text form of <see cref="Load"/>).</summary>
    public static NpcDocument Parse(string text, string? path = null)
    {
        var doc = YamlDocument.Parse(text);
        var original = NpcYaml.Read(doc);
        var working = NpcYaml.Read(doc); // independent editable copy
        return new NpcDocument(path, text, original, working);
    }

    /// <summary>
    /// Create a new NPC pre-filled with the schema's required fields at sensible
    /// defaults, ready to be edited and saved to a fresh file.
    /// </summary>
    public static NpcDocument NewNpc(string id = "core:npc.new_npc", string name = "New NPC")
    {
        var data = new NpcData();
        data.Set("id", id);
        data.Set("name", name);
        data.Set("level.min", "1");
        data.Set("level.max", "1");
        data.Set("creature_type", "humanoid");
        data.Set("faction", "hostile");
        data.Set("stats.health", "100");
        data.Set("stats.damage.min", "1");
        data.Set("stats.damage.max", "2");
        data.Set("stats.attack_speed_ms", "2000");
        return new NpcDocument(null, null, new NpcData(), data);
    }

    /// <summary>
    /// Serialize the current values. For a loaded file this preserves every untouched
    /// byte and changes only edited fields; for a new NPC it renders canonical YAML.
    /// </summary>
    public string ToYaml() =>
        _originalText is null ? NpcYaml.Emit(Data) : NpcYaml.Save(_originalText, _original, Data);

    /// <summary>Project the current values to the generated <see cref="Npc"/> model.</summary>
    public Npc ToModel() => Data.ToModel();

    /// <summary>Write the current values to <paramref name="path"/> as UTF-8 (no BOM).</summary>
    public void Save(string path)
    {
        File.WriteAllText(path, ToYaml(), new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        Path = path;
    }
}
