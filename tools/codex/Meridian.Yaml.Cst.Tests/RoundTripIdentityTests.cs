using Xunit;

namespace Meridian.Yaml.Cst.Tests;

/// <summary>
/// Guarantee (2) from tools-sad.md §6.2: byte identity for untouched regions. Loading any
/// real content YAML and saving it without edits must reproduce the input byte-for-byte —
/// comments, formatting, key order, quote styles and blank lines all preserved.
/// </summary>
public class RoundTripIdentityTests
{
    [Theory]
    [MemberData(nameof(RepoFixtures.AllContentYaml), MemberType = typeof(RepoFixtures))]
    public void LoadAndSaveWithoutEdits_IsByteIdentical(string relativePath)
    {
        var path = RepoFixtures.Absolute(relativePath);
        var original = File.ReadAllText(path);

        var doc = YamlDocument.Parse(original);
        var roundTripped = doc.ToText();

        Assert.Equal(original, roundTripped);
        // Also assert reference identity of the fast path (no splices => original string).
        Assert.Same(original, doc.OriginalText);
    }

    [Fact]
    public void FixtureCorpus_IsNonEmpty_AndIncludesCommentedFiles()
    {
        var files = RepoFixtures.AllContentYaml().ToList();
        Assert.NotEmpty(files);

        // At least one fixture must carry comments and non-default (flow-style) formatting,
        // otherwise the round-trip test proves nothing about the hard cases.
        bool anyCommented = false;
        bool anyFlowStyle = false;
        foreach (var row in files)
        {
            var text = File.ReadAllText(RepoFixtures.Absolute((string)row[0]));
            if (text.Contains('#'))
            {
                anyCommented = true;
            }

            if (text.Contains("{ ") || text.Contains("[\""))
            {
                anyFlowStyle = true;
            }
        }

        Assert.True(anyCommented, "expected at least one commented content fixture");
        Assert.True(anyFlowStyle, "expected at least one flow-style content fixture");
    }
}
