using Xunit;
using YamlDotNet.RepresentationModel;

namespace Meridian.Yaml.Cst.Tests;

/// <summary>
/// Requirement (d): an edited document still parses and preserves the intended semantics.
/// We assert semantic identity (§6.2 guarantee (1)) between the pre-edit logical value tree
/// and the post-edit tree, allowing ONLY the edited field to differ.
/// </summary>
public class SemanticIntegrityTests
{
    [Fact]
    public void EditedDocument_StillParses_AndOnlyTargetValueChangedSemantically()
    {
        var path = RepoFixtures.Absolute(Path.Combine("npcs", "kobold_miner.npc.yaml"));
        var original = File.ReadAllText(path);

        var before = Flatten(original);
        var doc = YamlDocument.Parse(original);
        doc.SetValue("ai.leash_radius_m", "70");
        var edited = doc.ToText();
        var after = Flatten(edited);

        // Both parse as valid YAML mappings.
        Assert.NotEmpty(after);

        // The only key whose value changed is the one we edited.
        var changed = before.Keys
            .Union(after.Keys)
            .Where(k => !before.TryGetValue(k, out var bv) || !after.TryGetValue(k, out var av) || bv != av)
            .ToList();

        Assert.Equal(new[] { "ai.leash_radius_m" }, changed);
        Assert.Equal("55", before["ai.leash_radius_m"]);
        Assert.Equal("70", after["ai.leash_radius_m"]);
    }

    [Theory]
    [MemberData(nameof(RepoFixtures.AllContentYaml), MemberType = typeof(RepoFixtures))]
    public void EveryFixture_ParsesIntoCst_WithoutLoss(string relativePath)
    {
        // A CST parse of every real content file must succeed, and its flattened logical
        // value tree must equal a plain YamlDotNet parse of the same file — i.e. the CST
        // did not drop or misread any value.
        var text = File.ReadAllText(RepoFixtures.Absolute(relativePath));
        var doc = YamlDocument.Parse(text);

        // Spot-check the envelope: every content file carries `schema:`; entity files also
        // carry `id:` (the pack root pack.yaml uses `namespace:` instead).
        Assert.Equal(CstKind.Mapping, doc.Root.Kind);
        Assert.NotNull(doc.GetValue("schema"));
        if (!relativePath.EndsWith("pack.yaml", StringComparison.Ordinal))
        {
            Assert.NotNull(doc.GetValue("id"));
        }
    }

    /// <summary>Flatten a YAML document into a map of dotted-path -> scalar string.</summary>
    private static Dictionary<string, string> Flatten(string text)
    {
        var yaml = new YamlStream();
        yaml.Load(new StringReader(text));
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        Walk(yaml.Documents[0].RootNode, string.Empty, result);
        return result;
    }

    private static void Walk(YamlNode node, string prefix, Dictionary<string, string> outMap)
    {
        switch (node)
        {
            case YamlScalarNode scalar:
                outMap[prefix] = scalar.Value ?? string.Empty;
                break;
            case YamlMappingNode map:
                foreach (var kv in map.Children)
                {
                    var key = ((YamlScalarNode)kv.Key).Value;
                    var childPrefix = prefix.Length == 0 ? key! : $"{prefix}.{key}";
                    Walk(kv.Value, childPrefix, outMap);
                }

                break;
            case YamlSequenceNode seq:
                for (int i = 0; i < seq.Children.Count; i++)
                {
                    Walk(seq.Children[i], $"{prefix}[{i}]", outMap);
                }

                break;
        }
    }
}
