using Meridian.Codex.Services;
using Meridian.Codex.ViewModels;
using Xunit;

namespace Meridian.Codex.Tests;

public class PackWorkspaceViewModelTests
{
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

    private static string TempDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "codex-workspace-vm-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }
}
