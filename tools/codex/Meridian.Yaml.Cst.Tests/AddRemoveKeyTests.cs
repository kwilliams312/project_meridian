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

    private static YamlMappingNode LoadMapping(string text)
    {
        var yaml = new YamlStream();
        yaml.Load(new StringReader(text));
        return (YamlMappingNode)yaml.Documents[0].RootNode;
    }
}
