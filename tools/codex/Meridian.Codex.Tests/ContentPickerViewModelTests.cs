using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.VisualTree;
using Meridian.Codex.Services;
using Meridian.Codex.ViewModels;
using Meridian.Codex.Views;
using Xunit;

namespace Meridian.Codex.Tests;

public sealed class ContentPickerViewModelTests
{
    [AvaloniaFact]
    public void Item_toolbar_exposes_accessible_native_picker_and_compact_path_controls()
    {
        var view = new ItemEditorView { DataContext = new ItemEditorViewModel() };
        var window = new Window { Content = view };
        window.Show();

        var buttons = view.GetVisualDescendants().OfType<Button>().ToArray();
        Assert.Contains(buttons, button => AutomationProperties.GetName(button) == "Open item file");
        Assert.Contains(buttons, button => AutomationProperties.GetName(button) == "Copy full item path");
        Assert.Contains(view.GetVisualDescendants().OfType<TextBlock>(),
            text => AutomationProperties.GetName(text) == "Selected item file");
    }

    [Fact]
    public async Task Item_picker_opens_selected_yaml_and_records_picker_context()
    {
        var path = ContentFixtures.CopyToTemp("items/rusty_pickaxe.item.yaml");
        var dialogs = new FakeDialogs { EntitySelection = path };
        var vm = new ItemEditorViewModel(dialogs, () => "/content/moonfall");

        await vm.OpenCommand.ExecuteAsync(null);

        Assert.Equal(EntityFileKind.Item, dialogs.EntityKind);
        Assert.Equal("/content/moonfall", dialogs.WorkspacePath);
        Assert.Equal(Path.GetFullPath(path), vm.FilePath);
        Assert.Equal("Rusty Pickaxe", vm.Name);
        Assert.False(vm.IsDirty);
    }

    [Fact]
    public async Task Cancelling_item_picker_preserves_document_dirty_and_validation_state()
    {
        var dialogs = new FakeDialogs { EntitySelection = null };
        var vm = new ItemEditorViewModel(dialogs) { Name = "Unsaved name" };
        var beforePreview = vm.PreviewYaml;
        var beforeValidation = vm.ValidationMessage;

        await vm.OpenCommand.ExecuteAsync(null);

        Assert.Equal("Unsaved name", vm.Name);
        Assert.True(vm.IsDirty);
        Assert.Equal(beforePreview, vm.PreviewYaml);
        Assert.Equal(beforeValidation, vm.ValidationMessage);
        Assert.Equal(0, dialogs.ConfirmCalls);
    }

    [Fact]
    public async Task Declining_discard_after_selection_keeps_dirty_item()
    {
        var selected = ContentFixtures.CopyToTemp("items/rusty_pickaxe.item.yaml");
        var dialogs = new FakeDialogs { EntitySelection = selected, ConfirmDiscard = false };
        var vm = new ItemEditorViewModel(dialogs) { Name = "Keep me" };

        await vm.OpenCommand.ExecuteAsync(null);

        Assert.Equal("Keep me", vm.Name);
        Assert.True(vm.IsDirty);
        Assert.Equal(1, dialogs.ConfirmCalls);
        Assert.Equal(string.Empty, vm.FilePath);
    }

    [Fact]
    public async Task Pack_folder_cancel_does_not_change_workspace_or_recents()
    {
        var recentPath = Path.Combine(TempDirectory(), "recent.json");
        var dialogs = new FakeDialogs { FolderSelection = null };
        using var vm = new PackWorkspaceViewModel(new RecentWorkspaceStore(recentPath), dialogs);
        var beforeStatus = vm.StatusMessage;

        await vm.OpenCommand.ExecuteAsync(null);

        Assert.False(vm.IsWorkspaceOpen);
        Assert.Empty(vm.RecentWorkspaces);
        Assert.Equal(beforeStatus, vm.StatusMessage);
    }

    [Fact]
    public async Task Declining_discard_keeps_unsaved_pack_draft()
    {
        var packRoot = Path.Combine(TempDirectory(), "existing");
        Directory.CreateDirectory(packRoot);
        File.WriteAllText(Path.Combine(packRoot, "pack.yaml"), """
            schema: meridian/pack@1
            namespace: existing
            name: Existing
            version: 1.0.0
            content_schema_version: 1
            compatibility_version: 1
            engine:
              godot: "4.6"
            license: Apache-2.0
            """);
        var dialogs = new FakeDialogs { FolderSelection = packRoot, ConfirmDiscard = false };
        using var vm = new PackWorkspaceViewModel(
            new RecentWorkspaceStore(Path.Combine(TempDirectory(), "recent.json")), dialogs);
        vm.Manifest.Name = "Unsaved draft";

        await vm.OpenCommand.ExecuteAsync(null);

        Assert.False(vm.IsWorkspaceOpen);
        Assert.True(vm.IsDirty);
        Assert.Equal("Unsaved draft", vm.Manifest.Name);
        Assert.Equal(1, dialogs.ConfirmCalls);
        Assert.Empty(vm.RecentWorkspaces);
    }

    [Fact]
    public async Task Invalid_pack_selection_is_rejected_before_current_workspace_is_replaced()
    {
        var contentRoot = TempDirectory();
        var dialogs = new FakeDialogs { FolderSelection = contentRoot };
        using var vm = new PackWorkspaceViewModel(
            new RecentWorkspaceStore(Path.Combine(TempDirectory(), "recent.json")), dialogs);
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        await vm.CreateCommand.ExecuteAsync(null);
        var current = vm.WorkspacePath;
        vm.Manifest.Name = "Dirty pack";

        dialogs.FolderSelection = TempDirectory(); // no pack.yaml
        await vm.OpenCommand.ExecuteAsync(null);

        Assert.Equal(current, vm.WorkspacePath);
        Assert.Equal("Dirty pack", vm.Manifest.Name);
        Assert.True(vm.IsDirty);
        Assert.Equal(0, dialogs.ConfirmCalls);
        Assert.StartsWith("Open failed:", vm.StatusMessage);
    }

    [Fact]
    public void Compact_path_keeps_filename_and_hides_long_prefix()
    {
        var path = Path.Combine(Path.GetTempPath(), new string('a', 70), "items", "rusty_pickaxe.item.yaml");
        var compact = PathPresentation.Compact(path, 40);

        Assert.StartsWith("…", compact);
        Assert.EndsWith(Path.Combine("items", "rusty_pickaxe.item.yaml"), compact);
        Assert.DoesNotContain(new string('a', 70), compact);
        Assert.True(compact.Length <= 40);
    }

    private static string TempDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "codex-picker-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    private sealed class FakeDialogs : IContentDialogService
    {
        public bool IsNativePickerAvailable => true;
        public string? EntitySelection { get; set; }
        public string? FolderSelection { get; set; }
        public bool ConfirmDiscard { get; set; }
        public EntityFileKind? EntityKind { get; private set; }
        public string? WorkspacePath { get; private set; }
        public int ConfirmCalls { get; private set; }

        public Task<string?> PickEntityFileAsync(EntityFileKind kind, string? currentPath, string? workspacePath = null)
        {
            EntityKind = kind;
            WorkspacePath = workspacePath;
            return Task.FromResult(EntitySelection);
        }

        public Task<string?> PickFolderAsync(FolderPickerPurpose purpose, string? currentPath) =>
            Task.FromResult(FolderSelection);

        public Task<bool> ConfirmDiscardChangesAsync(string documentName)
        {
            ConfirmCalls++;
            return Task.FromResult(ConfirmDiscard);
        }

        public Task CopyPathAsync(string path) => Task.CompletedTask;
    }
}
