using System.Text.Json.Nodes;
using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.VisualTree;
using Meridian.Codex.SchemaForms;
using Meridian.Codex.Views;
using Xunit;

namespace Meridian.Codex.Tests;

public sealed class SchemaFormTests
{
    private readonly SchemaCatalog _catalog = new();

    [Theory]
    [InlineData("pack.schema.yaml", "pack.yaml")]
    [InlineData("npc.schema.yaml", "npcs/kobold_miner.npc.yaml")]
    [InlineData("item.schema.yaml", "items/rusty_pickaxe.item.yaml")]
    [InlineData("ability.schema.yaml", "abilities/cleave_strike.ability.yaml")]
    public void Representative_documents_render_and_round_trip_byte_identically(string schema, string fixture)
    {
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath(fixture));
        var root = _catalog.GetRoot(schema);
        var document = new SchemaFormDocument(root, yaml);
        var form = new SchemaFormViewModel(document);

        Assert.NotEmpty(form.Root.Children);
        Assert.Equal(yaml, document.ToYaml());
        Assert.NotNull(SchemaCatalog.ParseYaml(document.ToYaml()));
    }

    [Fact]
    public void Optional_schema_property_appears_without_a_hand_written_view()
    {
        const string schemaYaml = """
            type: object
            properties:
              existing: { type: string }
              future_field: { type: boolean, default: true }
            """;
        var schemas = new Dictionary<string, JsonObject>
        {
            ["sample.schema.yaml"] = SchemaCatalog.ParseYaml(schemaYaml).AsObject(),
            ["common.defs.yaml"] = new(),
            ["skeleton.defs.yaml"] = new(),
        };

        var field = new SchemaCatalog(schemas).GetRoot("sample.schema.yaml");

        var optional = Assert.Single(field.Children, child => child.Name == "future_field");
        Assert.False(optional.IsRequired);
        Assert.Equal(SchemaFieldKind.Boolean, optional.Kind);
        Assert.True(optional.Default!.GetValue<bool>());
    }

    [Fact]
    public void Scalar_edit_is_cst_surgical_and_keeps_comments()
    {
        const string yaml = "schema: meridian/pack@1\nnamespace: core # keep\nname: Meridian Core\nversion: 0.1.0\ncontent_schema_version: 1\nengine:\n  godot: \"4.6\"\nlicense: Apache-2.0\n";
        var document = new SchemaFormDocument(_catalog.GetRoot("pack.schema.yaml"), yaml);

        document.Set("name", JsonValue.Create("Meridian Next"));

        Assert.Contains("namespace: core # keep", document.ToYaml());
        Assert.Contains("name: Meridian Next", document.ToYaml());
    }

    [Fact]
    public void Nested_optional_default_can_be_added_without_rewriting_siblings()
    {
        const string yaml = "schema: meridian/ability@1\nid: core:ability.test\nname: Test\ntarget: self\nschool: physical\n# effects stay here\neffects:\n  - kind: damage\n    amount: { min: 1, max: 2 }\n";
        var document = new SchemaFormDocument(_catalog.GetRoot("ability.schema.yaml"), yaml);

        document.Set("cast", new JsonObject { ["time_ms"] = 0 });

        Assert.Contains("# effects stay here", document.ToYaml());
        Assert.Contains("cast:\n  time_ms: 0", document.ToYaml());
    }

    [Fact]
    public void Array_reorder_changes_only_the_array_node()
    {
        const string yaml = "schema: meridian/ability@1\nid: core:ability.test\nname: Test\ntarget: enemy\nschool: physical\n# keep before effects\neffects:\n  - kind: damage\n    amount: { min: 1, max: 2 }\n  - kind: heal\n    amount: { min: 3, max: 4 }\n# keep after effects\n";
        var document = new SchemaFormDocument(_catalog.GetRoot("ability.schema.yaml"), yaml);

        document.MoveArrayItem("effects", 0, 1);

        Assert.Contains("# keep before effects", document.ToYaml());
        Assert.Contains("# keep after effects", document.ToYaml());
        Assert.Equal("heal", document.Get("effects[0].kind")!.ToString());
        Assert.Equal("damage", document.Get("effects[1].kind")!.ToString());
        Assert.NotNull(SchemaCatalog.ParseYaml(document.ToYaml()));
    }

    [Fact]
    public void OneOf_requires_confirmation_and_restores_cached_branch_data()
    {
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("abilities/cleave_strike.ability.yaml"));
        var root = _catalog.GetRoot("ability.schema.yaml");
        var effect = root.Children.Single(field => field.Name == "effects").Item!;
        Assert.Equal(SchemaFieldKind.OneOf, effect.Kind);
        var document = new SchemaFormDocument(root, yaml);

        var warning = document.SelectBranch("effects[0]", effect, "cc");
        Assert.False(warning.Changed);
        Assert.Contains("amount", warning.DestructiveFields);

        Assert.True(document.SelectBranch("effects[0]", effect, "cc", true).Changed);
        Assert.Equal("cc", document.Get("effects[0].kind")!.ToString());
        Assert.Equal("stun", document.Get("effects[0].type")!.ToString());
        Assert.Equal("100", document.Get("effects[0].duration_ms")!.ToString());
        Assert.True(document.SelectBranch("effects[0]", effect, "damage", true).Changed);
        Assert.Equal("12", document.Get("effects[0].amount.min")!.ToString());
    }

    [Fact]
    public void Array_item_children_bind_to_concrete_index_paths()
    {
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("abilities/cleave_strike.ability.yaml"));
        var form = new SchemaFormViewModel(new SchemaFormDocument(_catalog.GetRoot("ability.schema.yaml"), yaml));

        var effect = form.Root.Children.Single(field => field.Field.Name == "effects").Children.Single();

        Assert.Equal("effects[0]", effect.Path);
        Assert.All(effect.Children, child => Assert.StartsWith("effects[0].", child.Path));
    }

    [Fact]
    public void Unsupported_construct_is_read_only_and_actionable()
    {
        const string schemaYaml = "type: object\nproperties:\n  dynamic:\n    type: object\n    patternProperties: { '.*': { type: string } }\n";
        var schemas = new Dictionary<string, JsonObject>
        {
            ["sample.schema.yaml"] = SchemaCatalog.ParseYaml(schemaYaml).AsObject(),
            ["common.defs.yaml"] = new(),
            ["skeleton.defs.yaml"] = new(),
        };

        var dynamic = new SchemaCatalog(schemas).GetRoot("sample.schema.yaml").Children.Single();

        Assert.True(dynamic.IsReadOnly);
        Assert.Contains("Edit this object in YAML", dynamic.UnsupportedReason);
    }

    [Fact]
    public void Property_style_corpus_round_trips_arbitrary_scalar_edits()
    {
        var values = new[] { "plain", "with spaces", "true", "007", "colon: value", "quote \" value", "line\\nbreak" };
        foreach (var value in values)
        {
            var document = new SchemaFormDocument(_catalog.GetRoot("pack.schema.yaml"), "schema: meridian/pack@1\nnamespace: core\nname: Start\nversion: 1.0.0\ncontent_schema_version: 1\nengine: { godot: \"4.6\" }\nlicense: Apache-2.0\n");
            document.Set("name", JsonValue.Create(value));
            Assert.Equal(value, document.Get("name")!.ToString());
        }
    }

    [Fact]
    public void Experimental_file_host_validates_args_and_saves_a_copied_fixture()
    {
        var copy = ContentFixtures.CopyToTemp("abilities/cleave_strike.ability.yaml");
        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", copy], out var error);
        Assert.Null(error);
        Assert.NotNull(vm);
        Assert.False(vm!.SaveCommand.CanExecute(null));

        vm.Document.Set("name", JsonValue.Create("Changed in form"));
        Assert.True(vm.SaveCommand.CanExecute(null));
        vm.SaveCommand.Execute(null);

        var reopened = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", copy], out error);
        Assert.Null(error);
        Assert.Equal("Changed in form", reopened!.Document.Get("name")!.ToString());
        Assert.False(reopened.SaveCommand.CanExecute(null));
    }

    [Fact]
    public void Experimental_file_host_rejects_missing_files_without_replacing_shell_state()
    {
        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", "/missing/fixture.yaml"], out var error);
        Assert.Null(vm);
        Assert.Contains("does not exist", error);
    }

    [Fact]
    public void Experimental_file_host_rejects_schema_invalid_content_before_creating_state()
    {
        var copy = ContentFixtures.NewTempPath("invalid.ability.yaml");
        File.WriteAllText(copy, "schema: meridian/ability@9\nname: Missing required fields\n");

        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", copy], out var error);

        Assert.Null(vm);
        Assert.Contains("does not satisfy ability.schema.yaml", error);
    }
}

public sealed class SchemaFormHeadlessTests
{
    [AvaloniaFact]
    public void Generic_view_renders_labels_controls_and_unsupported_message()
    {
        var catalog = new SchemaCatalog();
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("abilities/cleave_strike.ability.yaml"));
        var vm = new SchemaFormViewModel(new SchemaFormDocument(catalog.GetRoot("ability.schema.yaml"), yaml));
        var view = new SchemaFormView { DataContext = vm };
        var window = new Window { Content = view };
        window.Show();

        var labels = view.GetVisualDescendants().OfType<TextBlock>().Select(block => block.Text).ToArray();
        Assert.Contains(labels, text => text?.StartsWith("Name", StringComparison.Ordinal) == true);
        Assert.NotEmpty(view.GetVisualDescendants().OfType<TextBox>());
        Assert.NotEmpty(view.GetVisualDescendants().OfType<ComboBox>());
        Assert.NotEmpty(view.GetVisualDescendants().OfType<NumericUpDown>());
    }

    [AvaloniaFact]
    public void Experimental_window_renders_a_real_file_and_disabled_save_button()
    {
        var copy = ContentFixtures.CopyToTemp("abilities/cleave_strike.ability.yaml");
        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", copy], out _)!;
        var window = new SchemaFormWindow { DataContext = vm };
        window.Show();

        Assert.Contains(window.GetVisualDescendants().OfType<TextBlock>(), block => block.Text?.Contains(Path.GetFileName(copy)) == true);
        var save = Assert.Single(window.GetVisualDescendants().OfType<Button>(), button => button.Content?.ToString() == "Save");
        Assert.False(save.Command!.CanExecute(save.CommandParameter));
    }
}
