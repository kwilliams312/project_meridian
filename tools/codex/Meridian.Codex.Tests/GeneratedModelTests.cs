using System.Linq;
using System.Reflection;
using Meridian.Codex.Models;
using Xunit;

namespace Meridian.Codex.Tests;

/// <summary>
/// Asserts the schema-driven C# generator (tools/schema_gen, C# target) produces
/// the expected model shape for known content schema types. These inspect the
/// *actual generated output* the Models project compiles, so they fail if the
/// generator's emitted C# drifts from the schema semantics (field names/types).
/// </summary>
public class GeneratedModelTests
{
    [Fact]
    public void Generator_emits_Item_record_with_expected_members_for_item_schema()
    {
        var t = typeof(Item);

        Assert.Equal("meridian/item@2", Item.SchemaTag);

        // Record type generated from item.schema.yaml.
        Assert.True(t.IsClass);

        // Required scalar id wrapped in the typed ContentId reference struct.
        var id = t.GetProperty("Id");
        Assert.NotNull(id);
        Assert.Equal(typeof(ContentId), id!.PropertyType);

        // Required plain-string name.
        var name = t.GetProperty("Name");
        Assert.NotNull(name);
        Assert.Equal(typeof(string), name!.PropertyType);

        // Required enum field from the schema (item rarity).
        var rarity = t.GetProperty("Rarity");
        Assert.NotNull(rarity);
        Assert.Equal(typeof(ItemRarity), rarity!.PropertyType);

        var equipType = t.GetProperty("EquipType");
        Assert.NotNull(equipType);
        Assert.Equal(typeof(EquipTypeRef?), equipType!.PropertyType);

        // Optional scalar surfaces as nullable (FlavorText: string?).
        var flavor = t.GetProperty("FlavorText");
        Assert.NotNull(flavor);
        Assert.Equal(typeof(string), flavor!.PropertyType); // reference type; nullability via annotations
    }

    [Fact]
    public void Generator_emits_Npc_record_with_typed_id_and_name_for_npc_schema()
    {
        var t = typeof(Npc);

        var id = t.GetProperty("Id");
        Assert.NotNull(id);
        Assert.Equal(typeof(ContentId), id!.PropertyType);

        var name = t.GetProperty("Name");
        Assert.NotNull(name);
        Assert.Equal(typeof(string), name!.PropertyType);
    }

    [Fact]
    public void Typed_id_reference_structs_are_distinct_value_types()
    {
        // The schema README guarantee ("an item: field cannot accept an NPC id")
        // is expressed as distinct wrapper structs, not interchangeable strings.
        Assert.True(typeof(ItemRef).IsValueType);
        Assert.True(typeof(NpcRef).IsValueType);
        Assert.NotEqual(typeof(ItemRef), typeof(NpcRef));
    }

    [Fact]
    public void Generator_emits_the_core_content_types()
    {
        var models = typeof(Item).Assembly
            .GetTypes()
            .Where(t => t.Namespace == "Meridian.Codex.Models")
            .Select(t => t.Name)
            .ToHashSet();

        // Core M0/M1 content types must all be present.
        foreach (var expected in new[] { "Item", "Npc", "Ability", "Quest", "Pack" })
        {
            Assert.Contains(expected, models);
        }
    }
}
