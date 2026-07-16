using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Meridian.Codex.Models;

namespace Meridian.Codex.Editing;

/// <summary>
/// The editor-facing value of an NPC, held as the flat set of canonical schema
/// leaf paths (e.g. <c>stats.health</c>, <c>ai.abilities[0].priority</c>) mapped
/// to their string values. This mirrors exactly the addressing the CST-preserving
/// YAML layer (<see cref="Meridian.Yaml.Cst.YamlDocument"/>) uses, so a save can be
/// reconciled path-by-path and untouched bytes preserved.
/// </summary>
/// <remarks>
/// A flat path store — rather than a mutable graph of the generated
/// <see cref="Npc"/> record — is deliberate: it is the natural currency of surgical
/// YAML edits (SetValue/AddKey/RemoveKey are path-addressed), it round-trips optional
/// fields without null-vs-absent ambiguity (absent = key not in the map), and it
/// still projects to and from the generated model via <see cref="ToModel"/> /
/// <see cref="FromModel"/> so the editor remains "bound to the generated C# model".
/// </remarks>
public sealed class NpcData
{
    private readonly Dictionary<string, string> _values = new(System.StringComparer.Ordinal);

    /// <summary>The schema const every NPC file carries.</summary>
    public const string SchemaTag = "meridian/npc@2";

    /// <summary>Present leaf paths and their string values (absent optionals are simply missing).</summary>
    public IReadOnlyDictionary<string, string> Values => _values;

    /// <summary>The value at <paramref name="path"/>, or null when absent.</summary>
    public string? Get(string path) => _values.TryGetValue(path, out var v) ? v : null;

    /// <summary>
    /// Set (or, when <paramref name="value"/> is null/blank, clear) the value at
    /// <paramref name="path"/>. Blank clears so an emptied optional form field maps
    /// to an absent YAML key rather than an empty scalar.
    /// </summary>
    public void Set(string path, string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            _values.Remove(path);
        }
        else
        {
            _values[path] = value.Trim();
        }
    }

    /// <summary>True when <paramref name="path"/> has a value.</summary>
    public bool Has(string path) => _values.ContainsKey(path);

    /// <summary>Remove every <c>ai.abilities[i].*</c> entry (used when the editor rebuilds the list).</summary>
    public void RemoveAbilities()
    {
        foreach (var key in _values.Keys.Where(k => k.StartsWith("ai.abilities[", System.StringComparison.Ordinal)).ToList())
        {
            _values.Remove(key);
        }
    }

    /// <summary>Number of <c>ai.abilities[i]</c> entries currently present (contiguous from 0).</summary>
    public int AbilityCount
    {
        get
        {
            int i = 0;
            while (_values.ContainsKey($"ai.abilities[{i}].ability"))
            {
                i++;
            }

            return i;
        }
    }

    /// <summary>The fixed (non-array) scalar leaf paths, in canonical schema order.</summary>
    public static readonly string[] FixedPaths =
    [
        "id", "name", "subtitle",
        "level.min", "level.max",
        "creature_type", "rank", "faction",
        "stats.health", "stats.mana", "stats.armor",
        "stats.damage.min", "stats.damage.max", "stats.attack_speed_ms",
        "ai.behavior", "ai.aggro_radius_m", "ai.leash_radius_m",
        "ai.call_for_help_radius_m", "ai.flee_at_health_pct",
        "movement.walk_speed_mps", "movement.run_speed_mps",
        "interaction.gossip_text", "interaction.vendor",
        "loot.table", "loot.money.min", "loot.money.max",
        "visual.model", "visual.scale", "visual.sound_set",
    ];

    /// <summary>The per-ability sub-field leaf names, in canonical schema order.</summary>
    public static readonly string[] AbilityFields =
        ["ability", "priority", "cooldown_override_ms", "use_at_health_below_pct"];

    // ---- projection to/from the generated model -----------------------------

    /// <summary>
    /// Project to the generated <see cref="Npc"/> record. Throws
    /// <see cref="System.FormatException"/> if a required field is missing or a
    /// numeric/enum field is malformed — the same failures schema validation would
    /// report, surfaced early for the editor's live-validation banner.
    /// </summary>
    public Npc ToModel()
    {
        return new Npc
        {
            Id = new ContentId(Require("id")),
            Name = Require("name"),
            Subtitle = Get("subtitle"),
            Level = RequireRange("level"),
            CreatureType = EnumFromYaml<NpcCreatureType>(Require("creature_type"), "creature_type"),
            Rank = Has("rank") ? EnumFromYaml<NpcRank>(Get("rank")!, "rank") : null,
            Faction = EnumFromYaml<NpcFaction>(Require("faction"), "faction"),
            Stats = BuildStats(),
            Ai = BuildAi(),
            Movement = BuildMovement(),
            Interaction = BuildInteraction(),
            Loot = BuildLoot(),
            Visual = BuildVisual(),
        };
    }

    /// <summary>Replace all fields with the flat projection of <paramref name="npc"/>.</summary>
    public static NpcData FromModel(Npc npc)
    {
        var d = new NpcData();
        d.Set("id", npc.Id.Id);
        d.Set("name", npc.Name);
        d.Set("subtitle", npc.Subtitle);
        d.Set("level.min", Num(npc.Level.Min));
        d.Set("level.max", Num(npc.Level.Max));
        d.Set("creature_type", EnumToYaml(npc.CreatureType));
        d.Set("rank", npc.Rank is { } rank ? EnumToYaml(rank) : null);
        d.Set("faction", EnumToYaml(npc.Faction));

        d.Set("stats.health", Num(npc.Stats.Health));
        d.Set("stats.mana", npc.Stats.Mana is { } mana ? Num(mana) : null);
        d.Set("stats.armor", npc.Stats.Armor is { } armor ? Num(armor) : null);
        d.Set("stats.damage.min", Num(npc.Stats.Damage.Min));
        d.Set("stats.damage.max", Num(npc.Stats.Damage.Max));
        d.Set("stats.attack_speed_ms", Num(npc.Stats.AttackSpeedMs));

        if (npc.Ai is { } ai)
        {
            d.Set("ai.behavior", ai.Behavior is { } b ? EnumToYaml(b) : null);
            d.Set("ai.aggro_radius_m", Num(ai.AggroRadiusM));
            d.Set("ai.leash_radius_m", Num(ai.LeashRadiusM));
            d.Set("ai.call_for_help_radius_m", Num(ai.CallForHelpRadiusM));
            d.Set("ai.flee_at_health_pct", Num(ai.FleeAtHealthPct));
            if (ai.Abilities is { } abilities)
            {
                for (int i = 0; i < abilities.Count; i++)
                {
                    var a = abilities[i];
                    d.Set($"ai.abilities[{i}].ability", a.Ability.Id);
                    d.Set($"ai.abilities[{i}].priority", Num(a.Priority));
                    d.Set($"ai.abilities[{i}].cooldown_override_ms", Num(a.CooldownOverrideMs));
                    d.Set($"ai.abilities[{i}].use_at_health_below_pct", Num(a.UseAtHealthBelowPct));
                }
            }
        }

        if (npc.Movement is { } mv)
        {
            d.Set("movement.walk_speed_mps", Num(mv.WalkSpeedMps));
            d.Set("movement.run_speed_mps", Num(mv.RunSpeedMps));
        }

        if (npc.Interaction is { } inter)
        {
            d.Set("interaction.gossip_text", inter.GossipText);
            d.Set("interaction.vendor", inter.Vendor?.Id);
        }

        if (npc.Loot is { } loot)
        {
            d.Set("loot.table", loot.Table?.Id);
            if (loot.Money is { } money)
            {
                d.Set("loot.money.min", Num(money.Min));
                d.Set("loot.money.max", Num(money.Max));
            }
        }

        // npc@2 made NpcVisual a oneOf: Model is now nullable (the assemble-like-a-
        // player branch B carries Appearance instead). The Codex NPC editor authors the
        // model branch only (branch B — per-NPC appearance — is not yet a Codex form,
        // #821 story 1 is mechanism-only), so project the model fields null-safely.
        if (npc.Visual is { } vis)
        {
            d.Set("visual.model", vis.Model?.Id);
            d.Set("visual.scale", Num(vis.Scale));
            d.Set("visual.sound_set", vis.SoundSet?.Id);
        }

        return d;
    }

    private NpcStats BuildStats() => new()
    {
        Health = RequireLong("stats.health"),
        Mana = OptLong("stats.mana"),
        Armor = OptLong("stats.armor"),
        Damage = RequireRange("stats.damage"),
        AttackSpeedMs = RequireLong("stats.attack_speed_ms"),
    };

    private NpcAi? BuildAi()
    {
        int abilityCount = AbilityCount;
        bool any = Has("ai.behavior") || Has("ai.aggro_radius_m") || Has("ai.leash_radius_m")
            || Has("ai.call_for_help_radius_m") || Has("ai.flee_at_health_pct") || abilityCount > 0;
        if (!any)
        {
            return null;
        }

        List<NpcAiAbility>? abilities = null;
        if (abilityCount > 0)
        {
            abilities = new List<NpcAiAbility>(abilityCount);
            for (int i = 0; i < abilityCount; i++)
            {
                abilities.Add(new NpcAiAbility
                {
                    Ability = new AbilityRef(Require($"ai.abilities[{i}].ability")),
                    Priority = OptLong($"ai.abilities[{i}].priority"),
                    CooldownOverrideMs = OptLong($"ai.abilities[{i}].cooldown_override_ms"),
                    UseAtHealthBelowPct = OptDouble($"ai.abilities[{i}].use_at_health_below_pct"),
                });
            }
        }

        return new NpcAi
        {
            Behavior = Has("ai.behavior") ? EnumFromYaml<NpcAiBehavior>(Get("ai.behavior")!, "ai.behavior") : null,
            AggroRadiusM = OptDouble("ai.aggro_radius_m"),
            LeashRadiusM = OptDouble("ai.leash_radius_m"),
            CallForHelpRadiusM = OptDouble("ai.call_for_help_radius_m"),
            FleeAtHealthPct = OptDouble("ai.flee_at_health_pct"),
            Abilities = abilities,
        };
    }

    private NpcMovement? BuildMovement()
    {
        if (!Has("movement.walk_speed_mps") && !Has("movement.run_speed_mps"))
        {
            return null;
        }

        return new NpcMovement
        {
            WalkSpeedMps = OptDouble("movement.walk_speed_mps"),
            RunSpeedMps = OptDouble("movement.run_speed_mps"),
        };
    }

    private NpcInteraction? BuildInteraction()
    {
        if (!Has("interaction.gossip_text") && !Has("interaction.vendor"))
        {
            return null;
        }

        return new NpcInteraction
        {
            GossipText = Get("interaction.gossip_text"),
            Vendor = Has("interaction.vendor") ? new VendorRef(Get("interaction.vendor")!) : null,
        };
    }

    private NpcLoot? BuildLoot()
    {
        bool hasMoney = Has("loot.money.min") || Has("loot.money.max");
        if (!Has("loot.table") && !hasMoney)
        {
            return null;
        }

        return new NpcLoot
        {
            Table = Has("loot.table") ? new LootRef(Get("loot.table")!) : null,
            Money = hasMoney ? RequireRange("loot.money") : null,
        };
    }

    private NpcVisual? BuildVisual()
    {
        if (!Has("visual.model"))
        {
            return null;
        }

        return new NpcVisual
        {
            Model = new ArtRef(Require("visual.model")),
            Scale = OptDouble("visual.scale"),
            SoundSet = Has("visual.sound_set") ? new SfxRef(Get("visual.sound_set")!) : null,
        };
    }

    // ---- scalar helpers -----------------------------------------------------

    private string Require(string path) =>
        Get(path) ?? throw new System.FormatException($"NPC is missing required field '{path}'.");

    private long RequireLong(string path) => ParseLong(Require(path), path);

    private IntRange RequireRange(string path) => new()
    {
        Min = RequireLong($"{path}.min"),
        Max = RequireLong($"{path}.max"),
    };

    private long? OptLong(string path) => Has(path) ? ParseLong(Get(path)!, path) : null;

    private double? OptDouble(string path) => Has(path) ? ParseDouble(Get(path)!, path) : null;

    private static long ParseLong(string value, string path) =>
        long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var n)
            ? n
            : throw new System.FormatException($"Field '{path}' must be an integer, got '{value}'.");

    private static double ParseDouble(string value, string path) =>
        double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var n)
            ? n
            : throw new System.FormatException($"Field '{path}' must be a number, got '{value}'.");

    private static string Num(long value) => value.ToString(CultureInfo.InvariantCulture);

    private static string? Num(long? value) => value is { } v ? Num(v) : null;

    private static string? Num(double? value) =>
        value is { } v ? v.ToString("0.############", CultureInfo.InvariantCulture) : null;

    // ---- enum <-> yaml token -------------------------------------------------

    /// <summary>Convert a snake_case YAML enum token to its generated PascalCase member.</summary>
    internal static TEnum EnumFromYaml<TEnum>(string token, string path) where TEnum : struct, System.Enum
    {
        var pascal = string.Concat(token.Split('_')
            .Where(part => part.Length > 0)
            .Select(part => char.ToUpperInvariant(part[0]) + part[1..]));
        if (System.Enum.TryParse<TEnum>(pascal, ignoreCase: false, out var value))
        {
            return value;
        }

        throw new System.FormatException(
            $"Field '{path}' has value '{token}', which is not a valid {typeof(TEnum).Name}.");
    }

    /// <summary>Convert a generated PascalCase enum member to its snake_case YAML token.</summary>
    internal static string EnumToYaml<TEnum>(TEnum value) where TEnum : struct, System.Enum
    {
        var name = value.ToString();
        var sb = new System.Text.StringBuilder(name.Length + 4);
        for (int i = 0; i < name.Length; i++)
        {
            char c = name[i];
            if (char.IsUpper(c))
            {
                if (i > 0)
                {
                    sb.Append('_');
                }

                sb.Append(char.ToLowerInvariant(c));
            }
            else
            {
                sb.Append(c);
            }
        }

        return sb.ToString();
    }
}
