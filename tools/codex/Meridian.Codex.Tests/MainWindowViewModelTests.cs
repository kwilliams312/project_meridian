using System.Linq;
using Meridian.Codex.ViewModels;
using Xunit;

namespace Meridian.Codex.Tests;

/// <summary>
/// Skeleton contract for the shell ViewModel (#124). Verifies the MVVM foundation
/// the NPC/Item editors (#128/#129) build on is constructible and wired, without
/// needing a display.
/// </summary>
public class MainWindowViewModelTests
{
    [Fact]
    public void Constructs_without_arguments()
    {
        var vm = new MainWindowViewModel();
        Assert.NotNull(vm);
    }

    [Fact]
    public void Exposes_editor_navigation_stubs_with_a_default_selection()
    {
        var vm = new MainWindowViewModel();

        Assert.NotEmpty(vm.Editors);
        Assert.Same(vm.Editors.First(), vm.SelectedEditor);
    }

    [Fact]
    public void Reports_generated_model_types_from_the_models_assembly()
    {
        var vm = new MainWindowViewModel();

        // Proves the generated Meridian.Codex.Models layer is referenced and loaded.
        Assert.True(vm.GeneratedModelTypeCount > 0);
    }

    [Fact]
    public void ShowModelInfo_command_updates_status_text()
    {
        var vm = new MainWindowViewModel();

        Assert.Equal("Ready.", vm.StatusText);
        vm.ShowModelInfoCommand.Execute(null);
        Assert.Contains("content model types", vm.StatusText);
    }
}
