using System.Text;
using Meridian.Codex.Models;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.Editing;

/// <summary>
/// An open item document in the editor: the editable <see cref="ItemData"/> plus the
/// provenance needed to save it without corrupting the file. For a loaded file that is
/// the original source text and a snapshot of its parsed values, so a save is a surgical
/// reconcile; for a new item there is no source, so a save emits a fresh canonical
/// document. This is the item-editor twin of <see cref="NpcDocument"/> (#128).
/// </summary>
public sealed class ItemDocument
{
    private readonly string? _originalText;
    private readonly ItemData _original;

    private ItemDocument(string? path, string? originalText, ItemData original, ItemData data)
    {
        Path = path;
        _originalText = originalText;
        _original = original;
        Data = data;
    }

    /// <summary>The file path this document was loaded from / last saved to (null until saved).</summary>
    public string? Path { get; private set; }

    /// <summary>The editable item field values.</summary>
    public ItemData Data { get; }

    /// <summary>True when this document has no source file yet (created via <see cref="NewItem"/>).</summary>
    public bool IsNew => _originalText is null;

    /// <summary>Load an existing item YAML file into an editable document.</summary>
    public static ItemDocument Load(string path)
    {
        var text = File.ReadAllText(path, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        return Parse(text, path);
    }

    /// <summary>Parse item YAML text into an editable document (source-text form of <see cref="Load"/>).</summary>
    public static ItemDocument Parse(string text, string? path = null)
    {
        var doc = YamlDocument.Parse(text);
        var schemaTag = doc.GetValue("schema");
        if (!string.Equals(schemaTag, ItemData.SchemaTag, StringComparison.Ordinal))
        {
            throw new FormatException(
                $"Expected item schema envelope '{ItemData.SchemaTag}', got "
                + (schemaTag is null ? "a missing schema field." : $"'{schemaTag}'."));
        }
        var original = ItemYaml.Read(doc);
        var working = ItemYaml.Read(doc); // independent editable copy
        return new ItemDocument(path, text, original, working);
    }

    /// <summary>
    /// Create a new item pre-filled with the schema's required fields at sensible
    /// defaults, ready to be edited and saved to a fresh file. Defaults to a
    /// consumable — the one item class with no conditional required fields — so a
    /// freshly created item projects to the model without further edits.
    /// </summary>
    public static ItemDocument NewItem(string id = "core:item.new_item", string name = "New Item")
    {
        var data = new ItemData();
        data.Set("id", id);
        data.Set("name", name);
        data.Set("item_class", "consumable");
        data.Set("rarity", "common");
        data.Set("visual.icon", "core:art.icon.item.placeholder");
        return new ItemDocument(null, null, new ItemData(), data);
    }

    /// <summary>
    /// Serialize the current values. For a loaded file this preserves every untouched
    /// byte and changes only edited fields; for a new item it renders canonical YAML.
    /// </summary>
    public string ToYaml() =>
        _originalText is null ? ItemYaml.Emit(Data) : ItemYaml.Save(_originalText, _original, Data);

    /// <summary>Project the current values to the generated <see cref="Item"/> model.</summary>
    public Item ToModel() => Data.ToModel();

    /// <summary>Write the current values to <paramref name="path"/> as UTF-8 (no BOM).</summary>
    public void Save(string path)
    {
        File.WriteAllText(path, ToYaml(), new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        Path = path;
    }
}
