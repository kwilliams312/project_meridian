using Meridian.Codex.Services;
using Meridian.Codex.ViewModels;
using Xunit;

namespace Meridian.Codex.Tests;

public class PackWorkspaceViewModelTests
{
    [Fact]
    public void Save_state_matrix_tracks_clean_dirty_invalid_and_persisted_values()
    {
        var contentRoot = TempDirectory();
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(Path.Combine(TempDirectory(), "recent.json")));

        Assert.False(vm.CanSave);
        Assert.False(vm.SaveCommand.CanExecute(null));
        Assert.Equal("Open or create a pack to save changes.", vm.SaveStateDescription);

        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);

        Assert.False(vm.IsDirty);
        Assert.False(vm.CanSave);
        Assert.False(vm.SaveCommand.CanExecute(null));
        Assert.Equal("No unsaved changes.", vm.SaveStateDescription);
        var persisted = File.ReadAllText(Path.Combine(vm.WorkspacePath, "pack.yaml"));
        vm.SaveCommand.Execute(null);
        Assert.Equal(persisted, File.ReadAllText(Path.Combine(vm.WorkspacePath, "pack.yaml")));

        vm.Manifest.Name = "Moonfall Revised";
        Assert.True(vm.IsDirty);
        Assert.True(vm.CanSave);
        Assert.True(vm.SaveCommand.CanExecute(null));
        Assert.Equal("Unsaved changes are ready to save.", vm.SaveStateDescription);

        vm.Manifest.Name = "Moonfall";
        Assert.False(vm.IsDirty);
        Assert.False(vm.CanSave);

        vm.Manifest.Version = "not-semver";
        Assert.True(vm.IsDirty);
        Assert.False(vm.CanSave);
        Assert.False(vm.SaveCommand.CanExecute(null));
        Assert.Contains("validation error", vm.SaveStateDescription);

        vm.Manifest.Version = "2.0.0";
        Assert.True(vm.CanSave);
        vm.SaveCommand.Execute(null);
        Assert.False(vm.IsDirty);
        Assert.False(vm.CanSave);
        Assert.Equal("No unsaved changes.", vm.SaveStateDescription);
    }

    [Fact]
    public void Create_command_opens_pack_and_exposes_header_dirty_and_status()
    {
        var contentRoot = TempDirectory();
        var recentPath = Path.Combine(TempDirectory(), "recent.json");
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(recentPath));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.Manifest.Version = "1.2.3";

        vm.CreateCommand.Execute(null);

        Assert.True(vm.IsWorkspaceOpen);
        Assert.Equal("moonfall · 1.2.3", vm.WorkspaceHeader);
        Assert.Contains("Validation: valid", vm.AggregateStatus);
        vm.Manifest.Name = "Moonfall Changed";
        Assert.True(vm.IsDirty);
        Assert.Contains("Unsaved changes", vm.AggregateStatus);
    }

    [Fact]
    public void Invalid_field_is_reported_inline_and_save_is_disabled()
    {
        var contentRoot = TempDirectory();
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(Path.Combine(TempDirectory(), "recent.json")));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);

        vm.Manifest.Version = "next";

        Assert.NotEmpty(vm.VersionError);
        Assert.False(vm.CanSave);
    }

    [Fact]
    public void Missing_description_and_compatibility_can_be_set_and_saved_together()
    {
        const string original = """
schema: meridian/pack@1
namespace: moonfall
name: Moonfall
version: 1.0.0
content_schema_version: 1
engine:
  godot: "4.6"
license: Apache-2.0
""";
        var contentRoot = TempDirectory();
        var packRoot = Path.Combine(contentRoot, "moonfall");
        Directory.CreateDirectory(packRoot);
        var manifestPath = Path.Combine(packRoot, "pack.yaml");
        File.WriteAllText(manifestPath, original);
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(Path.Combine(TempDirectory(), "recent.json")))
        {
            WorkspacePath = packRoot,
        };
        vm.OpenCommand.Execute(null);

        vm.Manifest.Description = "Moonfall pack";
        vm.Manifest.CompatibilityVersion = "2";
        Assert.True(vm.CanSave);
        vm.SaveCommand.Execute(null);

        Assert.Equal("pack.yaml saved without reformatting untouched YAML.", vm.StatusMessage);
        Assert.Equal(original + "\ndescription: Moonfall pack\ncompatibility_version: 2",
            File.ReadAllText(manifestPath));
    }

    [Fact]
    public void Opening_a_recent_workspace_replaces_dirty_state_with_the_opened_clean_baseline()
    {
        var contentRoot = TempDirectory();
        var recentPath = Path.Combine(TempDirectory(), "recent.json");
        var first = CreatePack(contentRoot, "first", "First");
        var second = CreatePack(contentRoot, "second", "Second");
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(recentPath)) { WorkspacePath = first };

        vm.OpenCommand.Execute(null);
        vm.Manifest.Name = "Dirty first";
        Assert.True(vm.CanSave);

        vm.OpenRecentCommand.Execute(second);

        Assert.Equal(second, vm.WorkspacePath);
        Assert.Equal("second", vm.Manifest.Namespace);
        Assert.False(vm.IsDirty);
        Assert.False(vm.CanSave);
        Assert.Equal("No unsaved changes.", vm.SaveStateDescription);
    }

    [Fact]
    public void Failed_write_preserves_dirty_retryable_state()
    {
        var contentRoot = TempDirectory();
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(Path.Combine(TempDirectory(), "recent.json")));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        vm.Manifest.Name = "Local edit";
        Assert.True(vm.CanSave);

        var manifestPath = Path.Combine(vm.WorkspacePath, "pack.yaml");
        var originalAttributes = File.GetAttributes(manifestPath);
        UnixFileMode? originalMode = OperatingSystem.IsWindows() ? null : File.GetUnixFileMode(manifestPath);
        try
        {
            if (OperatingSystem.IsWindows())
                File.SetAttributes(manifestPath, originalAttributes | FileAttributes.ReadOnly);
            else
                File.SetUnixFileMode(manifestPath, UnixFileMode.UserRead | UnixFileMode.GroupRead | UnixFileMode.OtherRead);
            vm.SaveCommand.Execute(null);
        }
        finally
        {
            if (OperatingSystem.IsWindows()) File.SetAttributes(manifestPath, originalAttributes);
            else File.SetUnixFileMode(manifestPath, originalMode!.Value);
        }

        Assert.Equal("Local edit", vm.Manifest.Name);
        Assert.True(vm.IsDirty);
        Assert.False(vm.HasExternalConflict);
        Assert.True(vm.CanSave);
        Assert.True(vm.SaveCommand.CanExecute(null));
        Assert.Equal("Unsaved changes are ready to save.", vm.SaveStateDescription);
        Assert.StartsWith("Save failed:", vm.StatusMessage);
    }

    private static string CreatePack(string contentRoot, string namespace_, string name)
    {
        using var workspace = ContentWorkspace.Create(contentRoot, namespace_, name);
        return workspace.RootPath;
    }

    private static string TempDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "codex-workspace-vm-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }
}
