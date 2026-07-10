using System.IO;
using System.Linq;
using System.Text;
using Meridian.Codex.Editing;
using Meridian.Codex.Models;
using Xunit;

namespace Meridian.Codex.Tests;

/// <summary>
/// Round-trip and serialization contract for the item editor's document layer (#129):
/// load→edit→save preserves structure, a known item round-trips byte-stable except the
/// intended edit, and a new item serializes to schema-valid YAML. All file writes go to
/// temp copies — the committed content/core fixtures are never mutated. Mirrors
/// <see cref="NpcEditorTests"/>.
/// </summary>
public class ItemEditorTests
{
    private const string Pickaxe = "items/rusty_pickaxe.item.yaml";
    private const string Potion = "items/minor_healing_potion.item.yaml";
    private const string Signet = "items/brens_signet.item.yaml";

    [Fact]
    public void Load_maps_a_known_item_onto_the_generated_model()
    {
        var doc = ItemDocument.Load(ContentFixtures.ContentCorePath(Pickaxe));
        Item item = doc.ToModel();

        Assert.Equal("core:item.rusty_pickaxe", item.Id.Id);
        Assert.Equal("Rusty Pickaxe", item.Name);
        Assert.Equal(ItemClass.Weapon, item.ItemClass);
        Assert.Equal("mace_1h", item.Subclass);
        Assert.Equal(ItemSlot.MainHand, item.Slot);
        Assert.Equal(ItemRarity.Uncommon, item.Rarity);
        Assert.Equal(3, item.RequiredLevel);
        Assert.Equal(8, item.ItemLevel);
        Assert.Equal(ItemBinding.OnEquip, item.Binding);
        Assert.Equal(9, item.Weapon!.Damage.Min);
        Assert.Equal(14, item.Weapon.Damage.Max);
        Assert.Equal(2600, item.Weapon.SpeedMs);
        Assert.Single(item.Stats!);
        Assert.Equal(StatKey.Strength, item.Stats![0].Stat);
        Assert.Equal(2, item.Stats[0].Amount);
        Assert.Equal(425, item.Price!.Sell);
        Assert.Equal("core:art.icon.item.pickaxe_rusty", item.Visual.Icon.Id);
        Assert.Equal("core:art.item.weapon.pickaxe_rusty", item.Visual.Model!.Value.Id);

        // item@2 worn block (contract ①): one model, mounted main_hand / sheathed back.
        var worn = item.Visual.Worn!;
        Assert.Single(worn.Models);
        Assert.Equal("core:art.item.weapon.pickaxe_rusty", worn.Models[0].Model.Id);
        Assert.Equal(ItemVisualWornModelMirror.None, worn.Models[0].Mirror);
        Assert.Equal(AttachSocket.MainHand, worn.Attach!.Socket);
        Assert.Equal(AttachSocket.Back, worn.Attach.SheathSocket);
        Assert.Null(worn.Hides);
        Assert.Null(worn.DyeChannels);
    }

    [Fact]
    public void Save_with_no_edits_is_byte_identical_to_the_original()
    {
        var path = ContentFixtures.CopyToTemp(Pickaxe);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Save(path);

        Assert.Equal(original, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Editing_one_field_changes_only_that_field_and_preserves_everything_else()
    {
        var path = ContentFixtures.CopyToTemp(Pickaxe);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("weapon.speed_ms", "2800");
        doc.Save(path);

        var expected = original.Replace("speed_ms: 2600", "speed_ms: 2800");
        Assert.Equal(expected, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Editing_a_value_inside_a_flow_mapping_touches_only_that_member()
    {
        var path = ContentFixtures.CopyToTemp(Pickaxe);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("weapon.damage.max", "16"); // damage: { min: 9, max: 14 }
        doc.Save(path);

        var expected = original.Replace("{ min: 9, max: 14 }", "{ min: 9, max: 16 }");
        Assert.Equal(expected, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Editing_a_stat_row_value_is_surgical()
    {
        // Editing an existing array-element scalar goes through SetValue and touches
        // only that value — the surrounding sequence layout is untouched.
        var path = ContentFixtures.CopyToTemp(Pickaxe);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("stats[0].amount", "4");
        var saved = doc.ToYaml();

        Assert.Equal(original.Replace("amount: 2", "amount: 4"), saved);
    }

    [Fact]
    public void Edit_preserves_hand_authored_comments_and_blank_lines()
    {
        // A hand-edited item carries a comment and a blank line the canonical formatter
        // would not produce; the surgical save must leave them byte-for-byte intact.
        var commented =
            "# Kobold heirloom — balance owner: itemization, do not touch without sign-off\n" +
            File.ReadAllText(ContentFixtures.ContentCorePath(Pickaxe), Utf8)
                .Replace("stats:\n", "\nstats:\n");
        var path = ContentFixtures.NewTempPath("commented.item.yaml");
        File.WriteAllText(path, commented, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("weapon.speed_ms", "3000");
        doc.Save(path);

        var saved = File.ReadAllText(path, Utf8);
        Assert.Equal(commented.Replace("speed_ms: 2600", "speed_ms: 3000"), saved);
        Assert.Contains("# Kobold heirloom", saved);
    }

    [Fact]
    public void Adding_an_optional_scalar_under_an_existing_block_is_surgical()
    {
        // minor_healing_potion's price block has buy and sell; adding nothing there,
        // instead add price under an existing block on the pickaxe: it has price.sell
        // only, so adding price.buy appends one line under the existing price block.
        var path = ContentFixtures.CopyToTemp(Pickaxe);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("price.buy", "1200");
        var saved = doc.ToYaml();

        Assert.Contains("buy: 1200", saved);
        // Every original line survives verbatim; exactly one line was added.
        Assert.Equal(original.Split('\n').Length + 1, saved.Split('\n').Length);
        foreach (var line in original.Split('\n').Where(l => l.Length > 0))
        {
            Assert.Contains(line, saved);
        }
    }

    [Fact]
    public void Removing_an_optional_scalar_from_a_block_deletes_only_that_line()
    {
        // minor_healing_potion's price block has buy and sell; removing buy leaves sell
        // (and everything else) in place.
        var path = ContentFixtures.CopyToTemp(Potion);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("price.buy", null);
        var saved = doc.ToYaml();

        Assert.DoesNotContain("buy: 150", saved);
        Assert.Contains("sell: 38", saved);
        Assert.Equal(original.Replace("  buy: 150\n", string.Empty), saved);
    }

    [Fact]
    public void Consumable_with_on_use_effect_round_trips_byte_stable()
    {
        // minor_healing_potion carries an effects.on_use ability ref and a price block;
        // a no-op save must not reorder, requote, or reflow any of it.
        var path = ContentFixtures.CopyToTemp(Potion);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        Assert.Equal("ability.minor_healing", doc.ToModel().Effects!.OnUse!.Value.Id);
        doc.Save(path);

        Assert.Equal(original, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Armor_item_with_multiple_stats_round_trips_byte_stable()
    {
        // brens_signet is an armor/finger item with a two-entry stats sequence; a no-op
        // save preserves the whole sequence byte-for-byte.
        var path = ContentFixtures.CopyToTemp(Signet);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        Item item = doc.ToModel();
        Assert.Equal(ItemClass.Armor, item.ItemClass);
        Assert.Equal(ItemSlot.Finger, item.Slot);
        Assert.Equal(2, item.Stats!.Count);
        doc.Save(path);

        Assert.Equal(original, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void New_item_serializes_to_schema_valid_yaml_that_reloads()
    {
        var doc = ItemDocument.NewItem("core:item.test_blade", "Test Blade");
        doc.Data.Set("flavor_text", "Forged for the test suite.");
        doc.Data.Set("item_class", "weapon");
        doc.Data.Set("subclass", "sword_1h");
        doc.Data.Set("slot", "main_hand");
        doc.Data.Set("rarity", "rare");
        doc.Data.Set("required_level", "10");
        doc.Data.Set("item_level", "20");
        doc.Data.Set("unique", "true");
        doc.Data.Set("binding", "on_pickup");
        doc.Data.Set("weapon.damage.min", "18");
        doc.Data.Set("weapon.damage.max", "27");
        doc.Data.Set("weapon.speed_ms", "1800");
        doc.Data.Set("weapon.school", "fire");
        doc.Data.Set("stats[0].stat", "strength");
        doc.Data.Set("stats[0].amount", "5");
        doc.Data.Set("stats[1].stat", "stamina");
        doc.Data.Set("stats[1].amount", "3");
        doc.Data.Set("effects.on_equip[0]", "ability.flame_aura");
        doc.Data.Set("price.sell", "5000");
        doc.Data.Set("price.buy", "25000");
        doc.Data.Set("visual.icon", "core:art.icon.item.test_blade");
        doc.Data.Set("visual.model", "core:art.item.weapon.test_blade");
        // Weapons require a worn block with an attach socket (lints L080/L081).
        doc.Data.Set("visual.worn.models[0].model", "core:art.item.weapon.test_blade");
        doc.Data.Set("visual.worn.models[0].mirror", "none");
        doc.Data.Set("visual.worn.attach.socket", "main_hand");
        doc.Data.Set("visual.worn.attach.sheath_socket", "hip_l");
        doc.Data.Set("visual.worn.dye_channels[0]", "primary");

        var yaml = doc.ToYaml();

        // Required envelope + fields per item.schema.yaml.
        Assert.StartsWith("schema: meridian/item@2\n", yaml);
        Assert.Contains("id: core:item.test_blade", yaml);
        Assert.Contains("item_class: weapon", yaml);
        Assert.Contains("unique: true", yaml);
        Assert.Contains("damage: { min: 18, max: 27 }", yaml);
        Assert.Contains("school: fire", yaml);
        Assert.Contains("worn:", yaml);
        Assert.Contains("socket: main_hand", yaml);

        // Reloads and projects back to the model with all values intact.
        var reloaded = ItemDocument.Parse(yaml);
        Item item = reloaded.ToModel();
        Assert.Equal("core:item.test_blade", item.Id.Id);
        Assert.Equal(ItemClass.Weapon, item.ItemClass);
        Assert.Equal(ItemSlot.MainHand, item.Slot);
        Assert.Equal(ItemBinding.OnPickup, item.Binding);
        Assert.True(item.Unique);
        Assert.Equal(1800, item.Weapon!.SpeedMs);
        Assert.Equal(School.Fire, item.Weapon.School);
        Assert.Equal(2, item.Stats!.Count);
        Assert.Equal(StatKey.Stamina, item.Stats[1].Stat);
        Assert.Equal("ability.flame_aura", item.Effects!.OnEquip![0].Id);
        Assert.Equal(25000, item.Price!.Buy);
        Assert.Equal("core:art.item.weapon.test_blade", item.Visual.Model!.Value.Id);

        // The worn block survives the write→re-read round-trip intact.
        var worn = item.Visual.Worn!;
        Assert.Equal("core:art.item.weapon.test_blade", worn.Models[0].Model.Id);
        Assert.Equal(ItemVisualWornModelMirror.None, worn.Models[0].Mirror);
        Assert.Equal(AttachSocket.MainHand, worn.Attach!.Socket);
        Assert.Equal(AttachSocket.HipL, worn.Attach.SheathSocket);
        Assert.Equal(DyeChannel.Primary, Assert.Single(worn.DyeChannels!));
    }

    [Fact]
    public void Editing_a_worn_attach_socket_is_surgical()
    {
        // rusty_pickaxe carries `attach: { socket: main_hand, sheath_socket: back }`
        // in flow style; changing one member touches only that value.
        var path = ContentFixtures.CopyToTemp(Pickaxe);
        var original = File.ReadAllText(path, Utf8);

        var doc = ItemDocument.Load(path);
        doc.Data.Set("visual.worn.attach.sheath_socket", "hip_r");
        var saved = doc.ToYaml();

        Assert.Equal(original.Replace("sheath_socket: back", "sheath_socket: hip_r"), saved);
    }

    [Fact]
    public void Armor_worn_with_hides_and_dye_channels_round_trips_through_canonical_emit()
    {
        // An armor item's worn block skins onto the body: models + hides + dye
        // channels, no attach (L081). Emit → reload → model projection is intact,
        // and a second emit is byte-stable.
        var doc = ItemDocument.NewItem("core:item.test_vest", "Test Vest");
        doc.Data.Set("item_class", "armor");
        doc.Data.Set("subclass", "leather");
        doc.Data.Set("slot", "chest");
        doc.Data.Set("armor", "45");
        doc.Data.Set("visual.icon", "core:art.icon.item.test_vest");
        doc.Data.Set("visual.worn.models[0].model", "core:art.item.armor.test_vest");
        doc.Data.Set("visual.worn.models[1].model", "core:art.item.armor.test_vest_trim");
        doc.Data.Set("visual.worn.models[1].mirror", "x");
        doc.Data.Set("visual.worn.hides[0]", "torso");
        doc.Data.Set("visual.worn.hides[1]", "waist");
        doc.Data.Set("visual.worn.dye_channels[0]", "primary");
        doc.Data.Set("visual.worn.dye_channels[1]", "accent");

        var first = doc.ToYaml();
        var reloaded = ItemDocument.Parse(first);
        Assert.Equal(first, reloaded.ToYaml());

        var worn = reloaded.ToModel().Visual.Worn!;
        Assert.Equal(2, worn.Models.Count);
        Assert.Equal("core:art.item.armor.test_vest_trim", worn.Models[1].Model.Id);
        Assert.Equal(ItemVisualWornModelMirror.X, worn.Models[1].Mirror);
        Assert.Equal(new[] { GeosetRegion.Torso, GeosetRegion.Waist }, worn.Hides!);
        Assert.Equal(new[] { DyeChannel.Primary, DyeChannel.Accent }, worn.DyeChannels!);
        Assert.Null(worn.Attach);
    }

    [Fact]
    public void Canonical_emit_is_idempotent_on_reload()
    {
        var doc = ItemDocument.NewItem("core:item.test_ore", "Test Ore");
        doc.Data.Set("item_class", "trade_good");
        doc.Data.Set("stack_size", "200");
        var first = doc.ToYaml();

        // Re-loading the emitted text and saving it back is a no-op (byte-stable).
        var second = ItemDocument.Parse(first).ToYaml();
        Assert.Equal(first, second);
    }

    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);
}
