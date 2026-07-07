using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Meridian.Codex.Models;

namespace Meridian.Codex.Editing;

/// <summary>
/// The editor-facing value of an item, held as the flat set of canonical schema
/// leaf paths (e.g. <c>rarity</c>, <c>weapon.damage.min</c>, <c>stats[0].amount</c>)
/// mapped to their string values. This mirrors exactly the addressing the
/// CST-preserving YAML layer (<see cref="Meridian.Yaml.Cst.YamlDocument"/>) uses, so a
/// save can be reconciled path-by-path and untouched bytes preserved.
/// </summary>
/// <remarks>
/// The design is deliberately identical to <see cref="NpcData"/> (#128): a flat path
/// store is the natural currency of surgical YAML edits, round-trips optional fields
/// without null-vs-absent ambiguity (absent = key not in the map), and still projects
/// to and from the generated <see cref="Item"/> record via <see cref="ToModel"/> /
/// <see cref="FromModel"/> so the editor stays "bound to the generated C# model".
/// </remarks>
public sealed class ItemData
{
    private readonly Dictionary<string, string> _values = new(System.StringComparer.Ordinal);

    /// <summary>The schema const every item file carries.</summary>
    public const string SchemaTag = "meridian/item@1";

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

    /// <summary>Remove every <c>stats[i].*</c> entry (used when the editor rebuilds the list).</summary>
    public void RemoveStats()
    {
        foreach (var key in _values.Keys.Where(k => k.StartsWith("stats[", System.StringComparison.Ordinal)).ToList())
        {
            _values.Remove(key);
        }
    }

    /// <summary>Remove every <c>effects.on_equip[i]</c> entry (used when the editor rebuilds the list).</summary>
    public void RemoveOnEquip()
    {
        foreach (var key in _values.Keys.Where(k => k.StartsWith("effects.on_equip[", System.StringComparison.Ordinal)).ToList())
        {
            _values.Remove(key);
        }
    }

    /// <summary>Number of <c>stats[i]</c> entries currently present (contiguous from 0).</summary>
    public int StatCount
    {
        get
        {
            int i = 0;
            while (_values.ContainsKey($"stats[{i}].stat"))
            {
                i++;
            }

            return i;
        }
    }

    /// <summary>Number of <c>effects.on_equip[i]</c> entries currently present (contiguous from 0).</summary>
    public int OnEquipCount
    {
        get
        {
            int i = 0;
            while (_values.ContainsKey($"effects.on_equip[{i}]"))
            {
                i++;
            }

            return i;
        }
    }

    /// <summary>The fixed (non-array) scalar leaf paths, in canonical schema order.</summary>
    public static readonly string[] FixedPaths =
    [
        "id", "name", "flavor_text",
        "item_class", "subclass", "slot", "rarity",
        "required_level", "item_level", "unique", "binding", "stack_size",
        "weapon.damage.min", "weapon.damage.max", "weapon.speed_ms", "weapon.school",
        "armor",
        "effects.on_use",
        "price.sell", "price.buy",
        "visual.icon", "visual.model",
    ];

    /// <summary>The per-stat sub-field leaf names, in canonical schema order.</summary>
    public static readonly string[] StatFields = ["stat", "amount"];

    // ---- projection to/from the generated model -----------------------------

    /// <summary>
    /// Project to the generated <see cref="Item"/> record. Throws
    /// <see cref="System.FormatException"/> if a required field is missing or a
    /// numeric/enum field is malformed — the same failures schema validation would
    /// report, surfaced early for the editor's live-validation banner.
    /// </summary>
    public Item ToModel()
    {
        return new Item
        {
            Id = new ContentId(Require("id")),
            Name = Require("name"),
            FlavorText = Get("flavor_text"),
            ItemClass = EnumFromYaml<ItemClass>(Require("item_class"), "item_class"),
            Subclass = Get("subclass"),
            Slot = Has("slot") ? EnumFromYaml<ItemSlot>(Get("slot")!, "slot") : null,
            Rarity = EnumFromYaml<ItemRarity>(Require("rarity"), "rarity"),
            RequiredLevel = OptLong("required_level"),
            ItemLevel = OptLong("item_level"),
            Unique = OptBool("unique"),
            Binding = Has("binding") ? EnumFromYaml<ItemBinding>(Get("binding")!, "binding") : null,
            StackSize = OptLong("stack_size"),
            Stats = BuildStats(),
            Weapon = BuildWeapon(),
            Armor = OptLong("armor"),
            Effects = BuildEffects(),
            Price = BuildPrice(),
            Visual = BuildVisual(),
        };
    }

    /// <summary>Replace all fields with the flat projection of <paramref name="item"/>.</summary>
    public static ItemData FromModel(Item item)
    {
        var d = new ItemData();
        d.Set("id", item.Id.Id);
        d.Set("name", item.Name);
        d.Set("flavor_text", item.FlavorText);
        d.Set("item_class", EnumToYaml(item.ItemClass));
        d.Set("subclass", item.Subclass);
        d.Set("slot", item.Slot is { } slot ? EnumToYaml(slot) : null);
        d.Set("rarity", EnumToYaml(item.Rarity));
        d.Set("required_level", Num(item.RequiredLevel));
        d.Set("item_level", Num(item.ItemLevel));
        d.Set("unique", item.Unique is { } uniq ? (uniq ? "true" : "false") : null);
        d.Set("binding", item.Binding is { } binding ? EnumToYaml(binding) : null);
        d.Set("stack_size", Num(item.StackSize));

        if (item.Stats is { } stats)
        {
            for (int i = 0; i < stats.Count; i++)
            {
                d.Set($"stats[{i}].stat", EnumToYaml(stats[i].Stat));
                d.Set($"stats[{i}].amount", Num(stats[i].Amount));
            }
        }

        if (item.Weapon is { } weapon)
        {
            d.Set("weapon.damage.min", Num(weapon.Damage.Min));
            d.Set("weapon.damage.max", Num(weapon.Damage.Max));
            d.Set("weapon.speed_ms", Num(weapon.SpeedMs));
            d.Set("weapon.school", weapon.School is { } school ? EnumToYaml(school) : null);
        }

        d.Set("armor", Num(item.Armor));

        if (item.Effects is { } effects)
        {
            d.Set("effects.on_use", effects.OnUse?.Id);
            if (effects.OnEquip is { } onEquip)
            {
                for (int i = 0; i < onEquip.Count; i++)
                {
                    d.Set($"effects.on_equip[{i}]", onEquip[i].Id);
                }
            }
        }

        if (item.Price is { } price)
        {
            d.Set("price.sell", Num(price.Sell));
            d.Set("price.buy", Num(price.Buy));
        }

        d.Set("visual.icon", item.Visual.Icon.Id);
        d.Set("visual.model", item.Visual.Model?.Id);

        return d;
    }

    private IReadOnlyList<ItemStat>? BuildStats()
    {
        int count = StatCount;
        if (count == 0)
        {
            return null;
        }

        var list = new List<ItemStat>(count);
        for (int i = 0; i < count; i++)
        {
            list.Add(new ItemStat
            {
                Stat = EnumFromYaml<StatKey>(Require($"stats[{i}].stat"), $"stats[{i}].stat"),
                Amount = RequireLong($"stats[{i}].amount"),
            });
        }

        return list;
    }

    private ItemWeapon? BuildWeapon()
    {
        bool any = Has("weapon.damage.min") || Has("weapon.damage.max")
            || Has("weapon.speed_ms") || Has("weapon.school");
        if (!any)
        {
            return null;
        }

        return new ItemWeapon
        {
            Damage = RequireRange("weapon.damage"),
            SpeedMs = RequireLong("weapon.speed_ms"),
            School = Has("weapon.school") ? EnumFromYaml<School>(Get("weapon.school")!, "weapon.school") : null,
        };
    }

    private ItemEffects? BuildEffects()
    {
        int onEquipCount = OnEquipCount;
        if (!Has("effects.on_use") && onEquipCount == 0)
        {
            return null;
        }

        List<AbilityRef>? onEquip = null;
        if (onEquipCount > 0)
        {
            onEquip = new List<AbilityRef>(onEquipCount);
            for (int i = 0; i < onEquipCount; i++)
            {
                onEquip.Add(new AbilityRef(Require($"effects.on_equip[{i}]")));
            }
        }

        return new ItemEffects
        {
            OnUse = Has("effects.on_use") ? new AbilityRef(Get("effects.on_use")!) : null,
            OnEquip = onEquip,
        };
    }

    private ItemPrice? BuildPrice()
    {
        if (!Has("price.sell") && !Has("price.buy"))
        {
            return null;
        }

        return new ItemPrice
        {
            Sell = OptLong("price.sell"),
            Buy = OptLong("price.buy"),
        };
    }

    private ItemVisual BuildVisual() => new()
    {
        Icon = new ArtRef(Require("visual.icon")),
        Model = Has("visual.model") ? new ArtRef(Get("visual.model")!) : null,
    };

    // ---- scalar helpers -----------------------------------------------------

    private string Require(string path) =>
        Get(path) ?? throw new System.FormatException($"Item is missing required field '{path}'.");

    private long RequireLong(string path) => ParseLong(Require(path), path);

    private IntRange RequireRange(string path) => new()
    {
        Min = RequireLong($"{path}.min"),
        Max = RequireLong($"{path}.max"),
    };

    private long? OptLong(string path) => Has(path) ? ParseLong(Get(path)!, path) : null;

    private bool? OptBool(string path) => Has(path) ? ParseBool(Get(path)!, path) : null;

    private static long ParseLong(string value, string path) =>
        long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var n)
            ? n
            : throw new System.FormatException($"Field '{path}' must be an integer, got '{value}'.");

    private static bool ParseBool(string value, string path) => value switch
    {
        "true" => true,
        "false" => false,
        _ => throw new System.FormatException($"Field '{path}' must be true or false, got '{value}'."),
    };

    private static string Num(long value) => value.ToString(CultureInfo.InvariantCulture);

    private static string? Num(long? value) => value is { } v ? Num(v) : null;

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
