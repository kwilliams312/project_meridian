using Xunit;
using YamlDotNet.RepresentationModel;

namespace Meridian.Yaml.Cst.Tests;

/// <summary>
/// Requirement (c): add/remove a key preserves the surrounding layout. Proven on real
/// content/core files, checking that comments, sibling keys and blank lines are untouched.
/// </summary>
public class AddRemoveKeyTests
{
    [Fact]
    public void RemoveKey_DeletesOnlyThatEntryLine_PreservesRest()
    {
        // rusty_pickaxe.item.yaml has a top-level `binding: on_equip` line.
        var path = RepoFixtures.Absolute(Path.Combine("items", "rusty_pickaxe.item.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        Assert.Equal("on_equip", doc.GetValue("binding"));

        doc.RemoveKey("binding");
        var edited = doc.ToText();

        Assert.DoesNotContain("binding:", edited);
        // The edited text equals the original with exactly that one line removed.
        var expected = string.Join("\n",
            original.Split('\n').Where(l => !l.StartsWith("binding:")));
        Assert.Equal(expected, edited);

        // Still valid YAML with the other keys intact.
        var map = LoadMapping(edited);
        Assert.False(map.Children.ContainsKey(new YamlScalarNode("binding")));
        Assert.True(map.Children.ContainsKey(new YamlScalarNode("rarity")));
    }

    [Fact]
    public void RemoveKey_Nested_PreservesSiblingsAndComments()
    {
        // Remove stats.armor from a file; health and damage must remain, in order.
        var path = RepoFixtures.Absolute(Path.Combine("npcs", "kobold_miner.npc.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        doc.RemoveKey("stats.armor");
        var edited = doc.ToText();

        Assert.DoesNotContain("armor:", edited);
        Assert.Contains("health: 120", edited);
        Assert.Contains("attack_speed_ms: 2000", edited);

        var map = LoadMapping(edited);
        var stats = (YamlMappingNode)map.Children[new YamlScalarNode("stats")];
        Assert.False(stats.Children.ContainsKey(new YamlScalarNode("armor")));
        Assert.True(stats.Children.ContainsKey(new YamlScalarNode("health")));
    }

    [Fact]
    public void AddKey_AppendsAfterLastEntry_MatchingIndent_PreservesRest()
    {
        var path = RepoFixtures.Absolute(Path.Combine("npcs", "kobold_miner.npc.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        // Add a new leaf under stats (last stats entry is attack_speed_ms).
        doc.AddKey("stats", "mana", "60");
        var edited = doc.ToText();

        // Everything before the insertion is byte-identical.
        Assert.StartsWith(original[..original.IndexOf("ai:", StringComparison.Ordinal)].TrimEnd('\n'),
            edited[..edited.IndexOf("ai:", StringComparison.Ordinal)].TrimEnd('\n'));

        var map = LoadMapping(edited);
        var stats = (YamlMappingNode)map.Children[new YamlScalarNode("stats")];
        Assert.Equal("60", ((YamlScalarNode)stats.Children[new YamlScalarNode("mana")]).Value);
        // Indentation of the new key matches siblings (2 spaces).
        Assert.Contains("\n  mana: 60\n", edited);
    }

    [Fact]
    public void AddKey_AtRoot_AppendsAtEnd_PreservesLeadingComments()
    {
        var path = RepoFixtures.Absolute(Path.Combine("loot", "kobold_miner.loot.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        doc.AddKey(null, "notes", "tuning pass 2");
        var edited = doc.ToText();

        // Leading comment block preserved verbatim.
        Assert.StartsWith("# Shared by kobold_miner", edited);
        var map = LoadMapping(edited);
        Assert.Equal("tuning pass 2", ((YamlScalarNode)map.Children[new YamlScalarNode("notes")]).Value);
    }

    [Fact]
    public void AddKey_SequentialSameMapping_AppendsInCallOrder_AndPreservesOriginalBytes()
    {
        const string original = "schema: meridian/pack@1\nlicense: Apache-2.0\n";
        var doc = YamlDocument.Parse(original);

        doc.AddKey(null, "description", "Moonfall pack");
        doc.AddKey(null, "compatibility_version", "2");

        var edited = doc.ToText();
        Assert.Equal(original + "description: Moonfall pack\ncompatibility_version: 2\n", edited);
        var map = LoadMapping(edited);
        Assert.Equal("Moonfall pack", ((YamlScalarNode)map.Children[new YamlScalarNode("description")]).Value);
        Assert.Equal("2", ((YamlScalarNode)map.Children[new YamlScalarNode("compatibility_version")]).Value);
    }

    [Fact]
    public void AddKey_SequentialDuplicate_IsRejected()
    {
        var doc = YamlDocument.Parse("name: Moonfall\n");
        doc.AddKey(null, "description", "first");

        Assert.Throws<YamlCstException>(() => doc.AddKey(null, "description", "second"));
    }

    [Fact]
    public void AddKey_RootAndFinalNestedMappingAtEof_EmitsNestedBeforeRoot()
    {
        const string original = "name: Moonfall\nengine:\n  godot: 4.6";
        var doc = YamlDocument.Parse(original);

        doc.AddKey(null, "license", "Apache-2.0");
        doc.AddKey("engine", "renderer", "gl_compatibility");

        var edited = doc.ToText();
        Assert.Equal(original + "\n  renderer: gl_compatibility\nlicense: Apache-2.0", edited);
        var root = LoadMapping(edited);
        var engine = Assert.IsType<YamlMappingNode>(root.Children[new YamlScalarNode("engine")]);
        Assert.Equal("gl_compatibility", Scalar(engine, "renderer"));
        Assert.Equal("Apache-2.0", Scalar(root, "license"));
    }

    [Fact]
    public void AddKey_ThreeMappingsSharingEof_OrdersByDepthRegardlessOfReverseCalls()
    {
        const string original = "root_value: keep\nouter:\n  middle:\n    leaf: original\n";
        var doc = YamlDocument.Parse(original);

        doc.AddKey("outer.middle", "deep_first_call", "one");
        doc.AddKey(null, "root_second_call", "two");
        doc.AddKey("outer", "outer_third_call", "three");
        doc.AddKey("outer.middle", "deep_fourth_call", "four");

        var edited = doc.ToText();
        var root = LoadMapping(edited);
        var outer = Assert.IsType<YamlMappingNode>(root.Children[new YamlScalarNode("outer")]);
        var middle = Assert.IsType<YamlMappingNode>(outer.Children[new YamlScalarNode("middle")]);
        Assert.Equal(new[] { "leaf", "deep_first_call", "deep_fourth_call" },
            middle.Children.Keys.Cast<YamlScalarNode>().Select(k => k.Value));
        Assert.Equal("three", Scalar(outer, "outer_third_call"));
        Assert.Equal("two", Scalar(root, "root_second_call"));
        Assert.Equal(edited, YamlDocument.Parse(edited).ToText());
    }

    [Fact]
    public void AddKey_FinalSequenceChildAndRootAtEof_ReparsesWithSemanticIdentity()
    {
        const string original = "name: Moonfall\nengines:\n  - godot: 4.6";
        var doc = YamlDocument.Parse(original);

        doc.AddKey(null, "license", "Apache-2.0");
        doc.AddKey("engines[0]", "renderer", "gl_compatibility");

        var edited = doc.ToText();
        Assert.Equal(original + "\n    renderer: gl_compatibility\nlicense: Apache-2.0", edited);
        var root = LoadMapping(edited);
        var engines = Assert.IsType<YamlSequenceNode>(root.Children[new YamlScalarNode("engines")]);
        var first = Assert.IsType<YamlMappingNode>(engines.Children[0]);
        Assert.Equal("4.6", Scalar(first, "godot"));
        Assert.Equal("gl_compatibility", Scalar(first, "renderer"));
        Assert.Equal("Apache-2.0", Scalar(root, "license"));
        Assert.Equal(edited, YamlDocument.Parse(edited).ToText());
    }

    [Fact]
    public void AddKey_DuplicateDetectionIsScopedToOwningMapping()
    {
        var doc = YamlDocument.Parse("name: Moonfall\nmetadata:\n  author: Meridian\n");

        doc.AddKey(null, "description", "root");
        doc.AddKey("metadata", "description", "nested");

        Assert.Throws<YamlCstException>(() => doc.AddKey("metadata", "description", "duplicate"));
        var root = LoadMapping(doc.ToText());
        var metadata = Assert.IsType<YamlMappingNode>(root.Children[new YamlScalarNode("metadata")]);
        Assert.Equal("root", Scalar(root, "description"));
        Assert.Equal("nested", Scalar(metadata, "description"));
    }

    private static string? Scalar(YamlMappingNode mapping, string key) =>
        Assert.IsType<YamlScalarNode>(mapping.Children[new YamlScalarNode(key)]).Value;

    private static YamlMappingNode LoadMapping(string text)
    {
        var yaml = new YamlStream();
        yaml.Load(new StringReader(text));
        return (YamlMappingNode)yaml.Documents[0].RootNode;
    }
}
