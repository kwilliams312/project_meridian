using Xunit;
using YamlDotNet.RepresentationModel;

namespace Meridian.Yaml.Cst.Tests;

/// <summary>
/// The core requirement (task goal + §6.2): change one value and have the output differ
/// ONLY in that value — every comment, key order, sibling value and formatting byte
/// preserved. Edits are proven against the real content/core fixtures.
/// </summary>
public class SurgicalEditTests
{
    /// <summary>Compute the exact character-level diff regions between two strings.</summary>
    private static (int prefix, int suffix, string oldMid, string newMid) DiffRegions(string a, string b)
    {
        int prefix = 0;
        int min = Math.Min(a.Length, b.Length);
        while (prefix < min && a[prefix] == b[prefix])
        {
            prefix++;
        }

        int suffix = 0;
        while (suffix < min - prefix && a[a.Length - 1 - suffix] == b[b.Length - 1 - suffix])
        {
            suffix++;
        }

        return (prefix,
                suffix,
                a.Substring(prefix, a.Length - prefix - suffix),
                b.Substring(prefix, b.Length - prefix - suffix));
    }

    /// <summary>Count lines that differ between two texts (same line count assumed for edits).</summary>
    private static int CountDifferingLines(string a, string b)
    {
        var la = a.Split('\n');
        var lb = b.Split('\n');
        int n = Math.Max(la.Length, lb.Length);
        int diff = 0;
        for (int i = 0; i < n; i++)
        {
            var x = i < la.Length ? la[i] : null;
            var y = i < lb.Length ? lb[i] : null;
            if (x != y)
            {
                diff++;
            }
        }

        return diff;
    }

    [Fact]
    public void SetValue_ChangesOnlyTheTargetScalar_NpcHealth()
    {
        // stats.health is a plain scalar on its own line in kobold_miner.npc.yaml.
        var path = RepoFixtures.Absolute(Path.Combine("npcs", "kobold_miner.npc.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        Assert.Equal("120", doc.GetValue("stats.health"));

        doc.SetValue("stats.health", "150");
        var edited = doc.ToText();

        // Minimal char-level diff: "120" -> "150" shares the "1" prefix and "0" suffix,
        // so only the middle digit differs.
        var (_, _, oldMid, newMid) = DiffRegions(original, edited);
        Assert.Equal("2", oldMid);
        Assert.Equal("5", newMid);

        // Every non-value byte is identical: reconstruct original by reverting the region.
        Assert.Equal(original, edited.Replace("health: 150", "health: 120"));
        // Exactly one line differs between input and output.
        Assert.Equal(1, CountDifferingLines(original, edited));
    }

    [Fact]
    public void SetValue_InsideFlowMapping_ChangesOnlyThatMember()
    {
        // level: { min: 3, max: 4 } — edit max only; the flow braces/commas/min survive.
        var path = RepoFixtures.Absolute(Path.Combine("npcs", "kobold_miner.npc.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        Assert.Equal("4", doc.GetValue("level.max"));

        doc.SetValue("level.max", "5");
        var edited = doc.ToText();

        var (_, _, oldMid, newMid) = DiffRegions(original, edited);
        Assert.Equal("4", oldMid);
        Assert.Equal("5", newMid);
        Assert.Contains("level: { min: 3, max: 5 }", edited);
    }

    [Fact]
    public void SetValue_InsideSequenceItem_ChangesOnlyThatScalar()
    {
        // spawns[0].wander_radius_m in a commented spawn file with block sequences.
        var path = RepoFixtures.Absolute(Path.Combine("spawns", "zone01_kobold_camp.spawn.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        Assert.Equal("8", doc.GetValue("spawns[0].wander_radius_m"));

        doc.SetValue("spawns[0].wander_radius_m", "12");
        var edited = doc.ToText();

        var (_, _, oldMid, newMid) = DiffRegions(original, edited);
        Assert.Equal("8", oldMid);
        Assert.Equal("12", newMid);

        // The leading comments must be byte-preserved.
        Assert.StartsWith("# Cinderdeep Mine kobold camp", edited);
        Assert.Contains("# Exercises: point spawns with wander", edited);
    }

    [Fact]
    public void SetValue_PreservesComments_AndKeyOrder_InLootFile()
    {
        var path = RepoFixtures.Absolute(Path.Combine("loot", "kobold_miner.loot.yaml"));
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        // Change a nested chance value; every comment line and key must survive verbatim.
        doc.SetValue("entries[0].chance_pct", "75");
        var edited = doc.ToText();

        var (_, _, oldMid, newMid) = DiffRegions(original, edited);
        Assert.Equal("80", oldMid);
        Assert.Equal("75", newMid);

        // The two leading comment lines are intact and in place.
        var lines = edited.Split('\n');
        Assert.StartsWith("# Shared by kobold_miner", lines[0]);
        Assert.StartsWith("# Exercises:", lines[1]);
    }

    [Fact]
    public void SetValue_QuotedScalar_KeepsQuotingStyle()
    {
        // pack.yaml: engine.godot is a double-quoted "4.6".
        var path = RepoFixtures.Absolute("pack.yaml");
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        Assert.Equal("4.6", doc.GetValue("engine.godot"));

        doc.SetValue("engine.godot", "4.7");
        var edited = doc.ToText();

        Assert.Contains("godot: \"4.7\"", edited);
        // Minimal char-level diff: only the final digit changes; the quotes and "4." survive.
        var (_, _, oldMid, newMid) = DiffRegions(original, edited);
        Assert.Equal("6", oldMid);
        Assert.Equal("7", newMid);
        // And the ONLY textual difference is inside the quoted value: revert restores original.
        Assert.Equal(original, edited.Replace("godot: \"4.7\"", "godot: \"4.6\""));
    }

    [Fact]
    public void SetValue_StringNeedingQuotes_GetsQuotedToPreserveType()
    {
        var path = RepoFixtures.Absolute(Path.Combine("npcs", "kobold_miner.npc.yaml"));
        var doc = YamlDocument.Parse(File.ReadAllText(path));

        // name is a plain string; set it to a value that would otherwise parse as a number.
        doc.SetValue("name", "42");
        var edited = doc.ToText();

        // Must be quoted so it stays a string when re-parsed.
        var yaml = new YamlStream();
        yaml.Load(new StringReader(edited));
        var mapping = (YamlMappingNode)yaml.Documents[0].RootNode;
        var nameNode = (YamlScalarNode)mapping.Children[new YamlScalarNode("name")];
        Assert.Equal("42", nameNode.Value);
        Assert.Equal(YamlDotNet.Core.ScalarStyle.SingleQuoted, nameNode.Style);
    }
}
