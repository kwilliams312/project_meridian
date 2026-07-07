using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.VisualTree;
using Meridian.Codex;
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
        // The NPCs rail entry is selected by default, so the shell must resolve and
        // render NpcEditorView (via ViewLocator) with no binding errors.
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        Assert.Same(vm.NpcEditor, vm.ActiveEditor);
        Assert.NotNull(window.GetVisualDescendants().OfType<NpcEditorView>().FirstOrDefault());
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
}
