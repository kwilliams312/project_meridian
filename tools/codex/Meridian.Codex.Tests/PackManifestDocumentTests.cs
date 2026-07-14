using System.Text;
using Meridian.Codex.Editing;
using Xunit;

namespace Meridian.Codex.Tests;

public class PackManifestDocumentTests
{
    private static readonly UTF8Encoding Utf8 = new(false);

    [Fact]
    public void Loaded_manifest_projects_all_workspace_fields()
    {
        var doc = PackManifestDocument.Load(ContentFixtures.ContentCorePath("pack.yaml"));

        Assert.Equal("core", doc.Data.Namespace);
        Assert.Equal("Meridian Core", doc.Data.Name);
        Assert.Equal("0.1.0", doc.Data.Version);
        Assert.Equal("1", doc.Data.ContentSchemaVersion);
        Assert.Equal("1", doc.Data.CompatibilityVersion);
        Assert.Equal("4.6", doc.Data.GodotVersion);
        Assert.Empty(doc.Data.Dependencies);
    }

    [Fact]
    public void Scalar_edit_preserves_every_untouched_manifest_byte()
    {
        var original = File.ReadAllText(ContentFixtures.ContentCorePath("pack.yaml"), Utf8);
        var path = ContentFixtures.NewTempPath("pack.yaml");
        File.WriteAllText(path, "# hand-authored pack note\n" + original, Utf8);

        var doc = PackManifestDocument.Load(path);
        doc.Data.Version = "0.2.0";
        doc.Save(path);

        Assert.Equal(("# hand-authored pack note\n" + original).Replace("version: 0.1.0", "version: 0.2.0"),
            File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Dependencies_can_be_added_without_reformatting_other_fields()
    {
        var original = File.ReadAllText(ContentFixtures.ContentCorePath("pack.yaml"), Utf8);
        var doc = PackManifestDocument.Parse(original);
        doc.Data.Dependencies.Add(new PackDependencyData("shared", "2.1.0"));

        var saved = doc.ToYaml();

        Assert.Contains("dependencies:\n  - namespace: \"shared\"\n    version: \"2.1.0\"", saved);
        Assert.Contains("description: First-party content for Project Meridian. Zone-01 starter content.", saved);
        Assert.StartsWith("schema: meridian/pack@1\nnamespace: core", saved);
    }

    [Fact]
    public void New_manifest_is_complete_and_reopens()
    {
        var path = ContentFixtures.NewTempPath("pack.yaml");
        var doc = PackManifestDocument.New("moonfall", "Moonfall");
        doc.Save(path);

        var reopened = PackManifestDocument.Load(path);
        Assert.Equal("moonfall", reopened.Data.Namespace);
        Assert.Equal("Moonfall", reopened.Data.Name);
        Assert.Equal("0.1.0", reopened.Data.Version);
        Assert.Equal("4.6", reopened.Data.GodotVersion);
    }

    [Theory]
    [InlineData("true")]
    [InlineData("null")]
    [InlineData("123")]
    public void New_manifest_preserves_string_types_and_passes_authoritative_validator(string stringLikeScalar)
    {
        var root = ContentFixtures.NewValidatorRoot();
        var packDirectory = Path.Combine(root, "content", "moonfall");
        Directory.CreateDirectory(packDirectory);
        var values = new PackManifestData
        {
            Namespace = "moonfall",
            Name = stringLikeScalar,
            Description = stringLikeScalar,
            License = stringLikeScalar,
        };
        PackManifestDocument.New(values).Save(Path.Combine(packDirectory, "pack.yaml"));

        var yaml = File.ReadAllText(Path.Combine(packDirectory, "pack.yaml"), Utf8);
        Assert.Contains($"name: \"{stringLikeScalar}\"", yaml);
        Assert.Contains("godot: \"4.6\"", yaml);
        var authoritative = ContentFixtures.RunAuthoritativeValidator(root);
        Assert.True(authoritative.ExitCode == 0,
            $"validate_content.py exited {authoritative.ExitCode}:\n{authoritative.Output}");
    }
}
