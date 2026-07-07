using System.Collections.Generic;
using System.Linq;
using System.Text;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.Editing;

/// <summary>
/// Reads and writes NPC YAML for the editor. Reading probes the CST-preserving
/// document for every canonical leaf path; writing has two modes:
/// <list type="bullet">
///   <item><b>Surgical reconcile</b> (existing files): compute the minimal set of
///     SetValue/AddKey/RemoveKey edits so every untouched byte — comments,
///     key order, quoting, blank lines — survives (#126, tools-sad.md §6.2).</item>
///   <item><b>Canonical emit</b> (new files, or structural changes the surgical
///     layer cannot express): render a fresh, schema-ordered document.</item>
/// </list>
/// Either way the output is valid against <c>schema/content/npc.schema.yaml</c> and
/// passes <c>mcc check</c> / the reference validator.
/// </summary>
internal static class NpcYaml
{
    /// <summary>Populate a fresh <see cref="NpcData"/> from a CST document.</summary>
    public static NpcData Read(YamlDocument doc)
    {
        var data = new NpcData();
        foreach (var path in NpcData.FixedPaths)
        {
            var value = doc.GetValue(path);
            if (value is not null)
            {
                data.Set(path, value);
            }
        }

        // Abilities are a variable-length sequence; probe until an entry is absent.
        for (int i = 0; doc.Resolve($"ai.abilities[{i}].ability") is not null; i++)
        {
            foreach (var field in NpcData.AbilityFields)
            {
                var value = doc.GetValue($"ai.abilities[{i}].{field}");
                if (value is not null)
                {
                    data.Set($"ai.abilities[{i}].{field}", value);
                }
            }
        }

        return data;
    }

    /// <summary>
    /// Reconcile <paramref name="edited"/> onto the original text surgically, or fall
    /// back to a canonical re-emit when a change is not expressible as scalar
    /// set/add/remove edits (e.g. adding a whole new nested section or array entry).
    /// </summary>
    public static string Save(string originalText, NpcData original, NpcData edited)
    {
        if (TrySurgical(originalText, original, edited, out var surgical))
        {
            return surgical;
        }

        return Emit(edited);
    }

    private static bool TrySurgical(string originalText, NpcData original, NpcData edited, out string result)
    {
        result = string.Empty;
        var doc = YamlDocument.Parse(originalText);

        var origPaths = original.Values.Keys.ToHashSet(System.StringComparer.Ordinal);
        var editPaths = edited.Values.Keys.ToHashSet(System.StringComparer.Ordinal);
        var added = editPaths.Where(p => !origPaths.Contains(p)).ToList();
        var removed = origPaths.Where(p => !editPaths.Contains(p)).ToList();

        // Every added leaf needs an existing block-mapping parent to append to.
        foreach (var path in added)
        {
            if (!ParentIsBlockMapping(doc, path))
            {
                return false;
            }
        }

        // Every removed leaf needs a block-mapping parent, and removing it must not
        // empty that mapping (an empty `ai:` etc. would be lossy / re-emit territory).
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

    /// <summary>Render <paramref name="data"/> as a fresh, schema-ordered NPC document.</summary>
    public static string Emit(NpcData data)
    {
        var sb = new StringBuilder();
        sb.Append("schema: ").Append(NpcData.SchemaTag).Append('\n');
        Line(sb, data, 0, "id");
        Line(sb, data, 0, "name");
        Line(sb, data, 0, "subtitle");
        FlowRange(sb, data, 0, "level");
        Line(sb, data, 0, "creature_type");
        Line(sb, data, 0, "rank");
        Line(sb, data, 0, "faction");

        // stats (required)
        sb.Append("stats:\n");
        Line(sb, data, 1, "stats.health", "health");
        Line(sb, data, 1, "stats.mana", "mana");
        Line(sb, data, 1, "stats.armor", "armor");
        FlowRange(sb, data, 1, "stats.damage", "damage");
        Line(sb, data, 1, "stats.attack_speed_ms", "attack_speed_ms");

        EmitAi(sb, data);
        EmitBlock(sb, data, "movement", [("movement.walk_speed_mps", "walk_speed_mps"), ("movement.run_speed_mps", "run_speed_mps")]);
        EmitBlock(sb, data, "interaction", [("interaction.gossip_text", "gossip_text"), ("interaction.vendor", "vendor")]);
        EmitLoot(sb, data);
        EmitVisual(sb, data);

        return sb.ToString();
    }

    private static void EmitAi(StringBuilder sb, NpcData data)
    {
        int abilityCount = data.AbilityCount;
        var scalars = new (string Path, string Key)[]
        {
            ("ai.behavior", "behavior"),
            ("ai.aggro_radius_m", "aggro_radius_m"),
            ("ai.leash_radius_m", "leash_radius_m"),
            ("ai.call_for_help_radius_m", "call_for_help_radius_m"),
            ("ai.flee_at_health_pct", "flee_at_health_pct"),
        };
        bool any = abilityCount > 0 || scalars.Any(s => data.Has(s.Path));
        if (!any)
        {
            return;
        }

        sb.Append("ai:\n");
        foreach (var (path, key) in scalars)
        {
            Line(sb, data, 1, path, key);
        }

        if (abilityCount > 0)
        {
            sb.Append("  abilities:\n");
            for (int i = 0; i < abilityCount; i++)
            {
                sb.Append("    - ability: ").Append(Render(data.Get($"ai.abilities[{i}].ability")!)).Append('\n');
                AbilityLine(sb, data, i, "priority");
                AbilityLine(sb, data, i, "cooldown_override_ms");
                AbilityLine(sb, data, i, "use_at_health_below_pct");
            }
        }
    }

    private static void EmitBlock(StringBuilder sb, NpcData data, string header, (string Path, string Key)[] fields)
    {
        if (!fields.Any(f => data.Has(f.Path)))
        {
            return;
        }

        sb.Append(header).Append(":\n");
        foreach (var (path, key) in fields)
        {
            Line(sb, data, 1, path, key);
        }
    }

    private static void EmitLoot(StringBuilder sb, NpcData data)
    {
        bool hasMoney = data.Has("loot.money.min") || data.Has("loot.money.max");
        if (!data.Has("loot.table") && !hasMoney)
        {
            return;
        }

        sb.Append("loot:\n");
        Line(sb, data, 1, "loot.table", "table");
        if (hasMoney)
        {
            FlowRange(sb, data, 1, "loot.money", "money");
        }
    }

    private static void EmitVisual(StringBuilder sb, NpcData data)
    {
        if (!data.Has("visual.model"))
        {
            return;
        }

        sb.Append("visual:\n");
        Line(sb, data, 1, "visual.model", "model");
        Line(sb, data, 1, "visual.scale", "scale");
        Line(sb, data, 1, "visual.sound_set", "sound_set");
    }

    private static void AbilityLine(StringBuilder sb, NpcData data, int index, string field)
    {
        var value = data.Get($"ai.abilities[{index}].{field}");
        if (value is not null)
        {
            sb.Append("      ").Append(field).Append(": ").Append(Render(value)).Append('\n');
        }
    }

    private static void Line(StringBuilder sb, NpcData data, int indent, string path, string? key = null)
    {
        var value = data.Get(path);
        if (value is null)
        {
            return;
        }

        sb.Append(' ', indent * 2).Append(key ?? path).Append(": ").Append(Render(value)).Append('\n');
    }

    private static void FlowRange(StringBuilder sb, NpcData data, int indent, string path, string? key = null)
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
    /// as YAML, double-quoted otherwise. IDs (<c>core:npc.x</c>) and enum tokens stay
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
