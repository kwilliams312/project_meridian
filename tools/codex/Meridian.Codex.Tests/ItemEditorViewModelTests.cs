using System.IO;
using System.Text;
using Meridian.Codex.ViewModels;
using Xunit;

namespace Meridian.Codex.Tests;

/// <summary>
/// Behavior of the item editor ViewModel (#129): live validation, the edit→save→reload
/// round-trip driven through the VM, and stat/effect-row editing. Exercises the VM as
/// the view binds it, without needing a display. Mirrors <see cref="NpcEditorViewModelTests"/>.
/// </summary>
public class ItemEditorViewModelTests
{
    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);

    [Fact]
    public void New_item_is_valid_and_previews_yaml()
    {
        var vm = new ItemEditorViewModel();

        Assert.True(vm.IsValid);
        Assert.StartsWith("schema: meridian/item@1", vm.PreviewYaml);
    }

    [Fact]
    public void Editing_a_field_updates_the_preview_and_revalidates()
    {
        var vm = new ItemEditorViewModel { PriceSell = "99" };

        Assert.True(vm.IsValid);
        Assert.Contains("sell: 99", vm.PreviewYaml);
    }

    [Fact]
    public void A_non_numeric_stat_is_reported_invalid()
    {
        var vm = new ItemEditorViewModel { RequiredLevel = "lots" };

        Assert.False(vm.IsValid);
        Assert.Contains("integer", vm.ValidationMessage);
    }

    [Fact]
    public void Open_edit_save_reload_round_trips_through_the_viewmodel()
    {
        var path = ContentFixtures.CopyToTemp("items/rusty_pickaxe.item.yaml");
        var original = File.ReadAllText(path, Utf8);

        var vm = new ItemEditorViewModel { FilePath = path };
        vm.OpenCommand.Execute(null);
        Assert.Equal("Rusty Pickaxe", vm.Name);
        Assert.True(vm.IsValid);

        vm.WeaponSpeedMs = "2900";
        vm.SaveCommand.Execute(null);

        Assert.Equal(original.Replace("speed_ms: 2600", "speed_ms: 2900"), File.ReadAllText(path, Utf8));

        // Reload the saved file into a fresh VM: the edit persisted, nothing else moved.
        var reloaded = new ItemEditorViewModel { FilePath = path };
        reloaded.OpenCommand.Execute(null);
        Assert.Equal("2900", reloaded.WeaponSpeedMs);
        Assert.Equal("mace_1h", reloaded.Subclass);
    }

    [Fact]
    public void Adding_a_stat_row_appears_in_the_model_and_preview()
    {
        var vm = new ItemEditorViewModel();
        vm.AddStatCommand.Execute(null);
        Assert.Single(vm.Stats);

        vm.Stats[0].Stat = "intellect";
        vm.Stats[0].Amount = "7";

        Assert.True(vm.IsValid);
        Assert.Contains("stat: intellect", vm.PreviewYaml);
        Assert.Contains("amount: 7", vm.PreviewYaml);
    }

    [Fact]
    public void Removing_a_stat_row_drops_it_from_the_preview()
    {
        var vm = new ItemEditorViewModel();
        vm.AddStatCommand.Execute(null);
        vm.Stats[0].Stat = "agility";
        vm.Stats[0].Amount = "1";
        Assert.Contains("stats:", vm.PreviewYaml);

        vm.RemoveStatCommand.Execute(vm.Stats[0]);

        Assert.Empty(vm.Stats);
        Assert.DoesNotContain("stats:", vm.PreviewYaml);
    }

    [Fact]
    public void Toggling_unique_appears_in_the_preview()
    {
        var vm = new ItemEditorViewModel { Unique = true };

        Assert.True(vm.IsValid);
        Assert.Contains("unique: true", vm.PreviewYaml);
    }
}
