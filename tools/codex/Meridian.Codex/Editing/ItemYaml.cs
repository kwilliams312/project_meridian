using System.Collections.Generic;
using System.Linq;
using System.Text;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.Editing;

/// <summary>
/// Reads and writes item YAML for the editor, mirroring <see cref="NpcYaml"/> (#128).
/// Reading probes the CST-preserving document for every canonical leaf path; writing
/// has two modes:
/// <list type="bullet">
///   <item><b>Surgical reconcile</b> (existing files): compute the minimal set of
///     SetValue/AddKey/RemoveKey edits so every untouched byte — comments,
///     key order, quoting, blank lines — survives (#126, tools-sad.md §6.2).</item>
///   <item><b>Canonical emit</b> (new files, or structural changes the surgical
///     layer cannot express — e.g. adding/removing an array entry): render a fresh,
///     schema-ordered document.</item>
/// </list>
/// Either way the output is valid against <c>schema/content/item.schema.yaml</c> and
/// passes <c>mcc check</c> / the reference validator.
/// </summary>
internal static class ItemYaml
{
    /// <summary>Populate a fresh <see cref="ItemData"/> from a CST document.</summary>
    public static ItemData Read(YamlDocument doc)
    {
        var data = new ItemData();
        foreach (var path in ItemData.FixedPaths)
        {
            var value = doc.GetValue(path);
            if (value is not null)
            {
                data.Set(path, value);
            }
        }

        // stats is a variable-length sequence of {stat, amount}; probe until absent.
        for (int i = 0; doc.Resolve($"stats[{i}].stat") is not null; i++)
        {
            foreach (var field in ItemData.StatFields)
            {
                var value = doc.GetValue($"stats[{i}].{field}");
                if (value is not null)
                {
                    data.Set($"stats[{i}].{field}", value);
                }
            }
        }

        // effects.on_equip is a variable-length sequence of bare ability-ref scalars.
        for (int i = 0; doc.Resolve($"effects.on_equip[{i}]") is not null; i++)
        {
            var value = doc.GetValue($"effects.on_equip[{i}]");
            if (value is not null)
            {
                data.Set($"effects.on_equip[{i}]", value);
            }
        }

        return data;
    }

    /// <summary>
    /// Reconcile <paramref name="edited"/> onto the original text surgically, or fall
    /// back to a canonical re-emit when a change is not expressible as scalar
    /// set/add/remove edits (e.g. adding a whole new nested section or array entry).
    /// </summary>
    public static string Save(string originalText, ItemData original, ItemData edited)
    {
        if (TrySurgical(originalText, original, edited, out var surgical))
        {
            return surgical;
        }

        return Emit(edited);
    }

    private static bool TrySurgical(string originalText, ItemData original, ItemData edited, out string result)
    {
        result = string.Empty;
        var doc = YamlDocument.Parse(originalText);

        var origPaths = original.Values.Keys.ToHashSet(System.StringComparer.Ordinal);
        var editPaths = edited.Values.Keys.ToHashSet(System.StringComparer.Ordinal);
        var added = editPaths.Where(p => !origPaths.Contains(p)).ToList();
        var removed = origPaths.Where(p => !editPaths.Contains(p)).ToList();

        // Array membership changes (any add/remove of an indexed leaf) are not
        // expressible as a surgical key edit — re-emit canonically instead.
        if (added.Concat(removed).Any(p => p.Contains('[')))
        {
            return false;
        }

        // Every added leaf needs an existing block-mapping parent to append to.
        foreach (var path in added)
        {
            if (!ParentIsBlockMapping(doc, path))
            {
                return false;
            }
        }

        // Every removed leaf needs a block-mapping parent, and removing it must not
        // empty that mapping (an empty `weapon:` etc. would be lossy / re-emit territory).
        foreach (var path in removed)
        {
            if (!ParentIsBlockMapping(doc, path))
            {
                return false;
            }

            var parent = ParentPath(path);
            bool parentKeepsAChild = editPaths.Any(p => ParentPath(p) == parent);
            if (!parentKeepsAChild)
            {
                return false;
            }
        }

        foreach (var path in editPaths.Intersect(origPaths))
        {
            var newValue = edited.Get(path)!;
            if (original.Get(path) != newValue)
            {
                doc.SetValue(path, newValue);
            }
        }

        foreach (var path in added)
        {
            doc.AddKey(ParentPath(path) is { Length: > 0 } parent ? parent : null, LastKey(path), edited.Get(path)!);
        }

        foreach (var path in removed)
        {
            doc.RemoveKey(path);
        }

        result = doc.ToText();
        return true;
    }

    private static bool ParentIsBlockMapping(YamlDocument doc, string path)
    {
        var parent = ParentPath(path);
        if (parent.Length == 0)
        {
            return true; // root is always a block mapping in content files
        }

        var node = doc.Resolve(parent);
        return node is { Kind: CstKind.Mapping, IsFlow: false };
    }

    /// <summary>The parent path of a leaf: everything before the final <c>.key</c> segment.</summary>
    internal static string ParentPath(string path)
    {
        int dot = path.LastIndexOf('.');
        return dot < 0 ? string.Empty : path[..dot];
    }

    /// <summary>The final mapping-key segment of a leaf path.</summary>
    internal static string LastKey(string path)
    {
        int dot = path.LastIndexOf('.');
        return dot < 0 ? path : path[(dot + 1)..];
    }

    // ---- canonical emit ------------------------------------------------------

    /// <summary>Render <paramref name="data"/> as a fresh, schema-ordered item document.</summary>
    public static string Emit(ItemData data)
    {
        var sb = new StringBuilder();
        sb.Append("schema: ").Append(ItemData.SchemaTag).Append('\n');
        Line(sb, data, 0, "id");
        Line(sb, data, 0, "name");
        Line(sb, data, 0, "flavor_text");
        Line(sb, data, 0, "item_class");
        Line(sb, data, 0, "subclass");
        Line(sb, data, 0, "slot");
        Line(sb, data, 0, "rarity");
        Line(sb, data, 0, "required_level");
        Line(sb, data, 0, "item_level");
        RawLine(sb, data, 0, "unique");
        Line(sb, data, 0, "binding");
        Line(sb, data, 0, "stack_size");

        EmitStats(sb, data);
        EmitWeapon(sb, data);
        Line(sb, data, 0, "armor");
        EmitEffects(sb, data);
        EmitPrice(sb, data);
        EmitVisual(sb, data);

        return sb.ToString();
    }

    private static void EmitStats(StringBuilder sb, ItemData data)
    {
        int count = data.StatCount;
        if (count == 0)
        {
            return;
        }

        sb.Append("stats:\n");
        for (int i = 0; i < count; i++)
        {
            sb.Append("  - stat: ").Append(Render(data.Get($"stats[{i}].stat")!)).Append('\n');
            sb.Append("    amount: ").Append(Render(data.Get($"stats[{i}].amount")!)).Append('\n');
        }
    }

    private static void EmitWeapon(StringBuilder sb, ItemData data)
    {
        bool any = data.Has("weapon.damage.min") || data.Has("weapon.damage.max")
            || data.Has("weapon.speed_ms") || data.Has("weapon.school");
        if (!any)
        {
            return;
        }

        sb.Append("weapon:\n");
        FlowRange(sb, data, 1, "weapon.damage", "damage");
        Line(sb, data, 1, "weapon.speed_ms", "speed_ms");
        Line(sb, data, 1, "weapon.school", "school");
    }

    private static void EmitEffects(StringBuilder sb, ItemData data)
    {
        int onEquipCount = data.OnEquipCount;
        if (!data.Has("effects.on_use") && onEquipCount == 0)
        {
            return;
        }

        sb.Append("effects:\n");
        Line(sb, data, 1, "effects.on_use", "on_use");
        if (onEquipCount > 0)
        {
            sb.Append("  on_equip:\n");
            for (int i = 0; i < onEquipCount; i++)
            {
                sb.Append("    - ").Append(Render(data.Get($"effects.on_equip[{i}]")!)).Append('\n');
            }
        }
    }

    private static void EmitPrice(StringBuilder sb, ItemData data)
    {
        if (!data.Has("price.sell") && !data.Has("price.buy"))
        {
            return;
        }

        sb.Append("price:\n");
        Line(sb, data, 1, "price.sell", "sell");
        Line(sb, data, 1, "price.buy", "buy");
    }

    private static void EmitVisual(StringBuilder sb, ItemData data)
    {
        if (!data.Has("visual.icon"))
        {
            return;
        }

        sb.Append("visual:\n");
        Line(sb, data, 1, "visual.icon", "icon");
        Line(sb, data, 1, "visual.model", "model");
    }

    private static void Line(StringBuilder sb, ItemData data, int indent, string path, string? key = null)
    {
        var value = data.Get(path);
        if (value is null)
        {
            return;
        }

        sb.Append(' ', indent * 2).Append(key ?? path).Append(": ").Append(Render(value)).Append('\n');
    }

    /// <summary>
    /// Emit a scalar that must stay unquoted even though it collides with a YAML
    /// keyword (e.g. <c>unique: true</c>). The value is schema-constrained upstream
    /// (only <c>true</c>/<c>false</c> reach here), so no quoting is ever needed.
    /// </summary>
    private static void RawLine(StringBuilder sb, ItemData data, int indent, string path, string? key = null)
    {
        var value = data.Get(path);
        if (value is null)
        {
            return;
        }

        sb.Append(' ', indent * 2).Append(key ?? path).Append(": ").Append(value).Append('\n');
    }

    private static void FlowRange(StringBuilder sb, ItemData data, int indent, string path, string? key = null)
    {
        var min = data.Get($"{path}.min");
        var max = data.Get($"{path}.max");
        if (min is null || max is null)
        {
            return;
        }

        sb.Append(' ', indent * 2).Append(key ?? path)
          .Append(": { min: ").Append(Render(min)).Append(", max: ").Append(Render(max)).Append(" }\n");
    }

    /// <summary>
    /// Render a scalar for a freshly emitted document: plain when it is unambiguous
    /// as YAML, double-quoted otherwise. IDs (<c>core:item.x</c>) and enum tokens stay
    /// plain — a bare colon not followed by a space is legal in a plain scalar, which
    /// is exactly how the hand-authored content files write them.
    /// </summary>
    internal static string Render(string value)
    {
        return NeedsQuote(value) ? DoubleQuote(value) : value;
    }

    private static bool NeedsQuote(string value)
    {
        if (value.Length == 0)
        {
            return true;
        }

        if (char.IsWhiteSpace(value[0]) || char.IsWhiteSpace(value[^1]))
        {
            return true;
        }

        switch (value)
        {
            case "true" or "false" or "True" or "False" or "yes" or "no"
                or "on" or "off" or "null" or "Null" or "~":
                return true;
        }

        const string leadingIndicators = "-?:,[]{}#&*!|>'\"%@`";
        if (leadingIndicators.IndexOf(value[0]) >= 0)
        {
            return true;
        }

        for (int i = 0; i < value.Length; i++)
        {
            char c = value[i];
            if (c is '\n' or '\t')
            {
                return true;
            }

            // ": " and " #" are the only in-line sequences a plain scalar cannot hold.
            if (c == ':' && i + 1 < value.Length && value[i + 1] == ' ')
            {
                return true;
            }

            if (c == '#' && i > 0 && value[i - 1] == ' ')
            {
                return true;
            }
        }

        return false;
    }

    private static string DoubleQuote(string value)
    {
        var sb = new StringBuilder(value.Length + 2);
        sb.Append('"');
        foreach (var c in value)
        {
            switch (c)
            {
                case '\\': sb.Append("\\\\"); break;
                case '"': sb.Append("\\\""); break;
                case '\n': sb.Append("\\n"); break;
                case '\t': sb.Append("\\t"); break;
                case '\r': sb.Append("\\r"); break;
                default: sb.Append(c); break;
            }
        }

        sb.Append('"');
        return sb.ToString();
    }
}
