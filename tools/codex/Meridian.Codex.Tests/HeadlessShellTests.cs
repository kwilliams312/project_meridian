using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.VisualTree;
using Meridian.Codex;
using Meridian.Codex.Controls;
using Meridian.Codex.Tests;
using Meridian.Codex.ViewModels;
using Meridian.Codex.Views;
using Xunit;

// Registers a headless Avalonia application so shell Views/XAML can be loaded and
// shown in tests with no display attached.
[assembly: AvaloniaTestApplication(typeof(TestAppBuilder))]

namespace Meridian.Codex.Tests;

/// <summary>Headless Avalonia bootstrap for the test assembly.</summary>
public sealed class TestAppBuilder
{
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions());
}

/// <summary>
/// Runtime smoke tests: prove the shell XAML actually loads and binds, which the
/// pure-VM tests cannot catch. Guards against App.axaml / MainWindow.axaml /
/// ViewLocator wiring errors that only surface when Avalonia initialises.
/// </summary>
public class HeadlessShellTests
{
    [AvaloniaFact]
    public void MainWindow_loads_xaml_and_binds_the_viewmodel_title()
    {
        var window = new MainWindow { DataContext = new MainWindowViewModel() };
        window.Show();

        Assert.IsType<MainWindowViewModel>(window.DataContext);
        Assert.Equal("Meridian Codex — Content Editor", window.Title);
    }

    [AvaloniaFact]
    public void MainWindow_hosts_the_npc_editor_view_for_the_npcs_selection()
    {
        var vm = new MainWindowViewModel();
        vm.SelectedEditor = vm.Editors.Single(e => e.Name == "NPCs");
        var window = new MainWindow { DataContext = vm };
        window.Show();

        Assert.Same(vm.NpcEditor, vm.ActiveEditor);
        Assert.NotNull(window.GetVisualDescendants().OfType<NpcEditorView>().FirstOrDefault());
    }

    [AvaloniaFact]
    public void MainWindow_opens_on_the_pack_workspace_and_renders_manifest_fields()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        Assert.Same(vm.PackWorkspace, vm.ActiveEditor);
        Assert.NotNull(window.GetVisualDescendants().OfType<PackWorkspaceView>().FirstOrDefault());
        Assert.Contains(window.GetVisualDescendants().OfType<TextBox>(), b => b.Text == "my_pack");
    }

    [AvaloniaFact]
    public void Pack_workspace_renders_one_sticky_bottom_action_using_the_save_command()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        var actionBar = Assert.Single(window.GetVisualDescendants().OfType<EditorActionBar>());
        Assert.Same(vm.PackWorkspace.SaveCommand, actionBar.ActionCommand);
        Assert.Equal("Open or create a pack to save changes.", actionBar.StateDescription);
        var layout = Assert.IsType<Grid>(actionBar.Parent);
        Assert.Equal(1, Grid.GetRow(actionBar));
        Assert.Contains(layout.Children.OfType<ScrollViewer>(), scroll => Grid.GetRow(scroll) == 0);
        var save = Assert.Single(actionBar.GetVisualDescendants().OfType<Button>());
        Assert.Equal("Save manifest", save.Content);
        Assert.Same(vm.PackWorkspace.SaveCommand, save.Command);
        Assert.False(save.Command!.CanExecute(null));
        Assert.False(save.IsEnabled);
        Assert.Equal(HorizontalAlignment.Right, save.HorizontalAlignment);
    }

    [AvaloniaFact]
    public void MainWindow_save_shortcuts_share_the_save_commands_can_execute_gate()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        var shortcuts = window.KeyBindings
            .Where(binding => binding.Gesture?.Key == Key.S)
            .ToList();
        Assert.Equal(2, shortcuts.Count);
        Assert.Contains(shortcuts, binding => binding.Gesture!.KeyModifiers == KeyModifiers.Control);
        Assert.Contains(shortcuts, binding => binding.Gesture!.KeyModifiers == KeyModifiers.Meta);
        Assert.All(shortcuts, binding =>
        {
            Assert.Same(vm.PackWorkspace.SaveCommand, binding.Command);
            Assert.False(binding.Command!.CanExecute(null));
        });
    }

    [AvaloniaFact]
    public void NpcEditorView_renders_an_ability_row_without_binding_errors()
    {
        // Exercises the ability-rotation ItemsControl DataTemplate — including the
        // compiled RemoveAbility command binding — with a live row present.
        var vm = new NpcEditorViewModel();
        vm.AddAbilityCommand.Execute(null);
        vm.Abilities[0].Ability = "ability.pickaxe_slam";

        var view = new NpcEditorView { DataContext = vm };
        var window = new Window { Content = view };
        window.Show();

        var boxes = view.GetVisualDescendants().OfType<TextBox>().ToList();
        Assert.Contains(boxes, b => b.Text == "ability.pickaxe_slam");
    }

    [AvaloniaFact]
    public async Task Pack_workspace_renders_external_conflict_after_malformed_watcher_write()
    {
        var contentRoot = Path.Combine(Path.GetTempPath(), "codex-headless-watcher", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(contentRoot);
        var recent = Path.Combine(contentRoot, "recent.json");
        using var vm = new PackWorkspaceViewModel(new Meridian.Codex.Services.RecentWorkspaceStore(recent));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        Assert.True(vm.IsWorkspaceOpen);

        File.WriteAllText(Path.Combine(vm.WorkspacePath, "pack.yaml"), "schema: [unterminated\n");
        for (var i = 0; i < 100 && !vm.HasExternalConflict; i++)
            await Task.Delay(25);

        Assert.True(vm.HasExternalConflict);
        Assert.False(vm.CanSave);
        Assert.Equal("Resolve the external pack.yaml conflict before saving.", vm.SaveStateDescription);
        var view = new PackWorkspaceView { DataContext = vm };
        var window = new Window { Content = view };
        window.Show();
        Assert.Contains(view.GetVisualDescendants().OfType<TextBlock>(),
            text => text.Text?.Contains("External conflict", StringComparison.Ordinal) == true);
    }

    [AvaloniaFact]
    public async Task Clean_external_reload_replaces_manifest_and_keeps_save_disabled()
    {
        var contentRoot = Path.Combine(Path.GetTempPath(), "codex-headless-reload", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(contentRoot);
        using var vm = new PackWorkspaceViewModel(new Meridian.Codex.Services.RecentWorkspaceStore(
            Path.Combine(contentRoot, "recent.json")));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        var manifestPath = Path.Combine(vm.WorkspacePath, "pack.yaml");
        var external = File.ReadAllText(manifestPath).Replace("Moonfall", "Externally Reloaded");

        File.WriteAllText(manifestPath, external);
        for (var i = 0; i < 100 && vm.Manifest.Name != "Externally Reloaded"; i++)
            await Task.Delay(25);

        Assert.Equal("Externally Reloaded", vm.Manifest.Name);
        Assert.False(vm.IsDirty);
        Assert.False(vm.HasExternalConflict);
        Assert.False(vm.CanSave);
        Assert.Equal("No unsaved changes.", vm.SaveStateDescription);
    }

    [AvaloniaFact]
    public async Task Pack_workspace_reenables_save_when_exact_baseline_is_restored()
    {
        var contentRoot = Path.Combine(Path.GetTempPath(), "codex-headless-recovery", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(contentRoot);
        using var vm = new PackWorkspaceViewModel(new Meridian.Codex.Services.RecentWorkspaceStore(
            Path.Combine(contentRoot, "recent.json")));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        Assert.True(vm.IsWorkspaceOpen);
        vm.Manifest.Name = "Local Moonfall edit";
        Assert.True(vm.CanSave);
        var manifestPath = Path.Combine(vm.WorkspacePath, "pack.yaml");
        var original = File.ReadAllText(manifestPath);
        string[] invalidDocuments =
        [
            "schema: [unterminated\n",
            original.Replace("schema: meridian/pack@1", "schema: meridian/pack@9"),
        ];

        foreach (var invalid in invalidDocuments)
        {
            File.WriteAllText(manifestPath, invalid);
            for (var i = 0; i < 100 && !vm.HasExternalConflict; i++) await Task.Delay(25);
            Assert.True(vm.HasExternalConflict);
            Assert.False(vm.CanSave);

            File.WriteAllText(manifestPath, original);
            for (var i = 0; i < 100 && vm.HasExternalConflict; i++) await Task.Delay(25);
            Assert.False(vm.HasExternalConflict);
            Assert.True(vm.CanSave);
        }
    }
}
