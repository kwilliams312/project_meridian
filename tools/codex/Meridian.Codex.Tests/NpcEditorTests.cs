using System.IO;
using System.Linq;
using System.Text;
using Meridian.Codex.Editing;
using Meridian.Codex.Models;
using Xunit;

namespace Meridian.Codex.Tests;

/// <summary>
/// Round-trip and serialization contract for the NPC editor's document layer (#128):
/// load→edit→save preserves structure, a known NPC round-trips byte-stable except the
/// intended edit, and a new NPC serializes to schema-valid YAML. All file writes go to
/// temp copies — the committed content/core fixtures are never mutated.
/// </summary>
public class NpcEditorTests
{
    private const string Miner = "npcs/kobold_miner.npc.yaml";
    private const string Digmaster = "npcs/kobold_digmaster.npc.yaml";
    private const string Bren = "npcs/quartermaster_bren.npc.yaml";

    [Fact]
    public void Load_maps_a_known_npc_onto_the_generated_model()
    {
        var doc = NpcDocument.Load(ContentFixtures.ContentCorePath(Miner));
        Npc npc = doc.ToModel();

        Assert.Equal("core:npc.kobold_miner", npc.Id.Id);
        Assert.Equal("Kobold Miner", npc.Name);
        Assert.Equal(NpcCreatureType.Humanoid, npc.CreatureType);
        Assert.Equal(NpcFaction.Hostile, npc.Faction);
        Assert.Equal(3, npc.Level.Min);
        Assert.Equal(4, npc.Level.Max);
        Assert.Equal(120, npc.Stats.Health);
        Assert.Equal(45, npc.Stats.Armor);
        Assert.Equal(6, npc.Stats.Damage.Min);
        Assert.Equal(NpcAiBehavior.Aggressive, npc.Ai!.Behavior);
        Assert.Equal(18, npc.Ai.AggroRadiusM);
        Assert.Single(npc.Ai.Abilities!);
        Assert.Equal("ability.pickaxe_slam", npc.Ai.Abilities![0].Ability.Id);
        Assert.Equal("loot.kobold_miner", npc.Loot!.Table!.Value.Id);
    }

    [Fact]
    public void Save_with_no_edits_is_byte_identical_to_the_original()
    {
        var path = ContentFixtures.CopyToTemp(Miner);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Save(path);

        Assert.Equal(original, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Editing_one_field_changes_only_that_field_and_preserves_everything_else()
    {
        var path = ContentFixtures.CopyToTemp(Miner);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Data.Set("stats.health", "150");
        doc.Save(path);

        var expected = original.Replace("health: 120", "health: 150");
        Assert.Equal(expected, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Editing_a_value_inside_a_flow_mapping_touches_only_that_member()
    {
        var path = ContentFixtures.CopyToTemp(Miner);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Data.Set("level.max", "6"); // level: { min: 3, max: 4 }
        doc.Save(path);

        var expected = original.Replace("{ min: 3, max: 4 }", "{ min: 3, max: 6 }");
        Assert.Equal(expected, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void Edit_preserves_hand_authored_comments_and_blank_lines()
    {
        // A hand-edited NPC carries a comment and a blank line the canonical formatter
        // would not produce; the surgical save must leave them byte-for-byte intact.
        var commented =
            "# Emberfall mine boss — do not rebalance without design sign-off\n" +
            File.ReadAllText(ContentFixtures.ContentCorePath(Digmaster), Utf8)
                .Replace("stats:\n", "\nstats:\n");
        var path = ContentFixtures.NewTempPath("commented.npc.yaml");
        File.WriteAllText(path, commented, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Data.Set("stats.health", "999");
        doc.Save(path);

        var saved = File.ReadAllText(path, Utf8);
        Assert.Equal(commented.Replace("health: 420", "health: 999"), saved);
        Assert.Contains("# Emberfall mine boss", saved);
    }

    [Fact]
    public void Adding_an_optional_scalar_under_an_existing_block_is_surgical()
    {
        // kobold_miner has no stats.mana; adding it appends one line under the existing
        // stats block and leaves the rest of the file untouched.
        var path = ContentFixtures.CopyToTemp(Miner);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Data.Set("stats.mana", "60");
        var saved = doc.ToYaml();

        Assert.Contains("mana: 60", saved);
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
        // digmaster's ai block has aggro_radius_m and leash_radius_m; removing aggro
        // leaves leash (and everything else) in place.
        var path = ContentFixtures.CopyToTemp(Digmaster);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Data.Set("ai.aggro_radius_m", null);
        var saved = doc.ToYaml();

        Assert.DoesNotContain("aggro_radius_m", saved);
        Assert.Contains("leash_radius_m: 70", saved);
        Assert.Equal(original.Replace("  aggro_radius_m: 22\n", string.Empty), saved);
    }

    [Fact]
    public void Editing_an_ability_rotation_field_is_surgical()
    {
        var path = ContentFixtures.CopyToTemp(Digmaster);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        doc.Data.Set("ai.abilities[0].cooldown_override_ms", "4500");
        var saved = doc.ToYaml();

        Assert.Equal(original.Replace("cooldown_override_ms: 6000", "cooldown_override_ms: 4500"), saved);
    }

    [Fact]
    public void Vendor_npc_with_folded_gossip_round_trips_byte_stable()
    {
        // quartermaster_bren uses a folded block scalar (>-) for gossip_text; a no-op
        // save must not fold, requote, or reflow it.
        var path = ContentFixtures.CopyToTemp(Bren);
        var original = File.ReadAllText(path, Utf8);

        var doc = NpcDocument.Load(path);
        Assert.Equal("vendor.bren_general_goods", doc.ToModel().Interaction!.Vendor!.Value.Id);
        doc.Save(path);

        Assert.Equal(original, File.ReadAllText(path, Utf8));
    }

    [Fact]
    public void New_npc_serializes_to_schema_valid_yaml_that_reloads()
    {
        var doc = NpcDocument.NewNpc("core:npc.test_wolf", "Timber Wolf");
        doc.Data.Set("subtitle", "Pack Hunter");
        doc.Data.Set("creature_type", "beast");
        doc.Data.Set("faction", "hostile");
        doc.Data.Set("rank", "elite");
        doc.Data.Set("level.min", "5");
        doc.Data.Set("level.max", "7");
        doc.Data.Set("stats.health", "240");
        doc.Data.Set("stats.damage.min", "10");
        doc.Data.Set("stats.damage.max", "15");
        doc.Data.Set("stats.attack_speed_ms", "1800");
        doc.Data.Set("ai.behavior", "aggressive");
        doc.Data.Set("ai.aggro_radius_m", "25");
        doc.Data.Set("ai.abilities[0].ability", "ability.rend");
        doc.Data.Set("ai.abilities[0].priority", "0");
        doc.Data.Set("loot.table", "loot.timber_wolf");
        doc.Data.Set("loot.money.min", "5");
        doc.Data.Set("loot.money.max", "20");
        doc.Data.Set("visual.model", "core:art.char.wolf.timber");
        doc.Data.Set("visual.scale", "1.1");

        var yaml = doc.ToYaml();

        // Required envelope + fields per npc.schema.yaml.
        Assert.StartsWith("schema: meridian/npc@1\n", yaml);
        Assert.Contains("id: core:npc.test_wolf", yaml);
        Assert.Contains("level: { min: 5, max: 7 }", yaml);
        Assert.Contains("creature_type: beast", yaml);
        Assert.Contains("attack_speed_ms: 1800", yaml);

        // Reloads and projects back to the model with all values intact.
        var reloaded = NpcDocument.Parse(yaml);
        Npc npc = reloaded.ToModel();
        Assert.Equal("core:npc.test_wolf", npc.Id.Id);
        Assert.Equal(NpcCreatureType.Beast, npc.CreatureType);
        Assert.Equal(NpcRank.Elite, npc.Rank);
        Assert.Equal(240, npc.Stats.Health);
        Assert.Equal("ability.rend", npc.Ai!.Abilities![0].Ability.Id);
        Assert.Equal("core:art.char.wolf.timber", npc.Visual!.Model.Id);
    }

    [Fact]
    public void Canonical_emit_is_idempotent_on_reload()
    {
        var doc = NpcDocument.NewNpc("core:npc.test_rat", "Sewer Rat");
        doc.Data.Set("creature_type", "critter");
        doc.Data.Set("faction", "neutral");
        var first = doc.ToYaml();

        // Re-loading the emitted text and saving it back is a no-op (byte-stable).
        var second = NpcDocument.Parse(first).ToYaml();
        Assert.Equal(first, second);
    }

    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);
}
