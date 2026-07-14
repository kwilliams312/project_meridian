using System.Text;
using Meridian.Codex.Services;
using Xunit;

namespace Meridian.Codex.Tests;

public class ContentWorkspaceTests
{
    private static readonly UTF8Encoding Utf8 = new(false);

    [Fact]
    public void Create_makes_namespace_directory_and_reopens_without_yaml_authoring()
    {
        var contentRoot = TempDirectory();

        using var created = ContentWorkspace.Create(contentRoot, "moonfall", "Moonfall");
        Assert.True(File.Exists(Path.Combine(contentRoot, "moonfall", "pack.yaml")));

        using var reopened = ContentWorkspace.Open(Path.Combine(contentRoot, "moonfall"));
        Assert.Equal("moonfall", reopened.Manifest.Data.Namespace);
        Assert.Equal("Moonfall", reopened.Manifest.Data.Name);
        Assert.Equal(WorkspaceValidationState.Valid, reopened.Validation.State);
    }

    [Theory]
    [InlineData("Namespace", "Bad-Namespace")]
    [InlineData("Version", "version one")]
    [InlineData("GodotVersion", "5.0")]
    [InlineData("ContentSchemaVersion", "2")]
    [InlineData("CompatibilityVersion", "0")]
    public void Invalid_manifest_values_have_field_diagnostics(string field, string value)
    {
        var contentRoot = TempDirectory();
        using var workspace = ContentWorkspace.Create(contentRoot, "moonfall", "Moonfall");
        typeof(Meridian.Codex.Editing.PackManifestData).GetProperty(field)!.SetValue(workspace.Manifest.Data, value);

        var result = workspace.Validate();

        Assert.Equal(WorkspaceValidationState.Invalid, result.State);
        Assert.Contains(result.Diagnostics, d => d.Field == field && d.Severity == DiagnosticSeverity.Error);
    }

    [Fact]
    public void Missing_and_mismatched_dependencies_are_reported_on_the_dependency_field()
    {
        var contentRoot = TempDirectory();
        using var dependency = ContentWorkspace.Create(contentRoot, "shared", "Shared");
        using var workspace = ContentWorkspace.Create(contentRoot, "moonfall", "Moonfall");
        workspace.Manifest.Data.Dependencies.Add(new("shared", "9.0.0"));
        workspace.Manifest.Data.Dependencies.Add(new("missing_pack", "1.0.0"));

        var result = workspace.Validate();

        Assert.Contains(result.Diagnostics, d => d.Field == "Dependencies[0].Version" && d.Message.Contains("0.1.0"));
        Assert.Contains(result.Diagnostics, d => d.Field == "Dependencies[1].Namespace" && d.Message.Contains("not found"));
    }

    [Fact]
    public void Clean_workspace_reloads_an_external_manifest_edit()
    {
        var contentRoot = TempDirectory();
        using var workspace = ContentWorkspace.Create(contentRoot, "moonfall", "Moonfall");
        var text = File.ReadAllText(workspace.ManifestPath, Utf8);
        File.WriteAllText(workspace.ManifestPath, text.Replace("name: Moonfall", "name: Moonfall Reloaded"), Utf8);

        var change = workspace.RefreshFromDisk();

        Assert.Contains(change, new[] { ExternalChangeResult.Reloaded, ExternalChangeResult.None });
        Assert.Equal("Moonfall Reloaded", workspace.Manifest.Data.Name);
        Assert.False(workspace.HasExternalConflict);
    }

    [Fact]
    public void Dirty_workspace_never_overwrites_an_external_manifest_edit()
    {
        var contentRoot = TempDirectory();
        using var workspace = ContentWorkspace.Create(contentRoot, "moonfall", "Moonfall");
        workspace.Manifest.Data.Name = "Unsaved local name";
        var disk = File.ReadAllText(workspace.ManifestPath, Utf8)
            .Replace("version: 0.1.0", "version: 0.2.0");
        File.WriteAllText(workspace.ManifestPath, disk, Utf8);

        var change = workspace.RefreshFromDisk();

        Assert.Equal(ExternalChangeResult.Conflict, change);
        Assert.True(workspace.HasExternalConflict);
        Assert.Throws<WorkspaceConflictException>(() => workspace.Save());
        Assert.Equal(disk, File.ReadAllText(workspace.ManifestPath, Utf8));
    }

    [Fact]
    public void Aggregate_status_combines_validation_build_and_dirty_state()
    {
        var contentRoot = TempDirectory();
        using var workspace = ContentWorkspace.Create(contentRoot, "moonfall", "Moonfall");
        workspace.RecordBuildResult(false, "mcc build failed");
        workspace.Manifest.Data.Version = "bad";

        var status = workspace.GetAggregateStatus();

        Assert.Contains("Validation: 1 error", status.Summary);
        Assert.Contains("Build: failed", status.Summary);
        Assert.True(status.IsDirty);
    }

    private static string TempDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "codex-workspace-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }
}
