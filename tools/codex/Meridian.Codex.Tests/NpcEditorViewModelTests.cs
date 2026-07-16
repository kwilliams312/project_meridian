using System.IO;
using System.Text;
using Meridian.Codex.ViewModels;
using Xunit;

namespace Meridian.Codex.Tests;

/// <summary>
/// Behavior of the NPC editor ViewModel (#128): live validation, the edit→save→reload
/// round-trip driven through the VM, and ability-rotation editing. Exercises the VM as
/// the view binds it, without needing a display.
/// </summary>
public class NpcEditorViewModelTests
{
    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);

    [Fact]
    public void New_npc_is_valid_and_previews_yaml()
    {
        var vm = new NpcEditorViewModel();

        Assert.True(vm.IsValid);
        Assert.StartsWith("schema: meridian/npc@2", vm.PreviewYaml);
    }

    [Fact]
    public void Editing_a_field_updates_the_preview_and_revalidates()
    {
        var vm = new NpcEditorViewModel { Health = "333" };

        Assert.True(vm.IsValid);
        Assert.Contains("health: 333", vm.PreviewYaml);
    }

    [Fact]
    public void A_non_numeric_stat_is_reported_invalid()
    {
        var vm = new NpcEditorViewModel { Health = "lots" };

        Assert.False(vm.IsValid);
        Assert.Contains("integer", vm.ValidationMessage);
    }

    [Fact]
    public void Open_edit_save_reload_round_trips_through_the_viewmodel()
    {
        var path = ContentFixtures.CopyToTemp("npcs/kobold_miner.npc.yaml");
        var original = File.ReadAllText(path, Utf8);

        var vm = new NpcEditorViewModel { FilePath = path };
        vm.OpenCommand.Execute(null);
        Assert.Equal("Kobold Miner", vm.Name);
        Assert.True(vm.IsValid);

        vm.Health = "175";
        vm.SaveCommand.Execute(null);

        Assert.Equal(original.Replace("health: 120", "health: 175"), File.ReadAllText(path, Utf8));

        // Reload the saved file into a fresh VM: the edit persisted, nothing else moved.
        var reloaded = new NpcEditorViewModel { FilePath = path };
        reloaded.OpenCommand.Execute(null);
        Assert.Equal("175", reloaded.Health);
        Assert.Equal("18", reloaded.AggroRadiusM);
    }

    [Fact]
    public void Adding_an_ability_row_appears_in_the_model_and_preview()
    {
        var vm = new NpcEditorViewModel();
        vm.AddAbilityCommand.Execute(null);
        Assert.Single(vm.Abilities);

        vm.Abilities[0].Ability = "ability.bite";
        vm.Abilities[0].Priority = "1";

        Assert.True(vm.IsValid);
        Assert.Contains("ability: ability.bite", vm.PreviewYaml);
        Assert.Contains("priority: 1", vm.PreviewYaml);
    }

    [Fact]
    public void Removing_an_ability_row_drops_it_from_the_preview()
    {
        var vm = new NpcEditorViewModel();
        vm.AddAbilityCommand.Execute(null);
        vm.Abilities[0].Ability = "ability.bite";
        Assert.Contains("ability.bite", vm.PreviewYaml);

        vm.RemoveAbilityCommand.Execute(vm.Abilities[0]);

        Assert.Empty(vm.Abilities);
        Assert.DoesNotContain("abilities:", vm.PreviewYaml);
    }
}
