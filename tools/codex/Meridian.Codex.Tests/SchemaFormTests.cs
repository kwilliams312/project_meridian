using System.Text.Json.Nodes;
using Avalonia.Controls;
using Avalonia.Automation;
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

    [Fact]
    public void Invalid_edit_blocks_command_and_direct_save_then_recovers_and_reopens()
    {
        var copy = ContentFixtures.CopyToTemp("pack.yaml");
        var original = File.ReadAllText(copy);
        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "pack", copy], out var error)!;
        Assert.Null(error);

        vm.Document.Set("name", JsonValue.Create(string.Empty));

        Assert.True(vm.IsDirty);
        Assert.False(vm.IsValid);
        Assert.False(vm.SaveCommand.CanExecute(null));
        Assert.Contains(vm.Diagnostics, diagnostic => diagnostic.Path == "name");
        Assert.Contains("validation error", vm.ValidationSummary, StringComparison.OrdinalIgnoreCase);
        var nameField = vm.Form.Root.Children.Single(field => field.Path == "name");
        Assert.True(nameField.HasDiagnostic);
        Assert.False(vm.TrySave());
        vm.SaveCommand.Execute(null);
        Assert.Equal(original, File.ReadAllText(copy));

        vm.Document.Set("name", JsonValue.Create("Recovered Pack"));

        Assert.True(vm.IsValid);
        Assert.False(nameField.HasDiagnostic);
        Assert.True(vm.SaveCommand.CanExecute(null));
        Assert.True(vm.TrySave());
        var reopened = SchemaFormFileViewModel.TryCreate(["--schema-form", "pack", copy], out error);
        Assert.Null(error);
        Assert.Equal("Recovered Pack", reopened!.Document.Get("name")!.ToString());
    }

    [Theory]
    [InlineData("name", "", "name")]
    [InlineData("name", "This pack name is deliberately longer than eighty characters so maxLength remains enforced by schema", "name")]
    [InlineData("namespace", "Bad-Namespace", "namespace")]
    public void String_constraints_are_authoritatively_validated_and_field_adjacent(string path, string value, string diagnosticPath)
    {
        var copy = ContentFixtures.CopyToTemp("pack.yaml");
        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "pack", copy], out _)!;

        vm.Document.Set(path, JsonValue.Create(value));

        Assert.Contains(vm.Diagnostics, diagnostic => diagnostic.Path == diagnosticPath);
        Assert.True(vm.Form.Root.Children.Single(field => field.Path == diagnosticPath).HasDiagnostic);
        Assert.False(vm.SaveCommand.CanExecute(null));
    }

    [Fact]
    public void Exclusive_numeric_boundary_is_preserved_in_widget_metadata()
    {
        var schema = new SchemaCatalog().GetRoot("npc.schema.yaml");
        var scale = schema.Children.Single(field => field.Name == "visual").Children.Single(field => field.Name == "scale");
        Assert.True(scale.HasExclusiveMinimum);
        Assert.Equal(0, scale.Minimum);
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("npcs/kobold_miner.npc.yaml"));
        var form = new SchemaFormViewModel(new SchemaFormDocument(schema, yaml));
        var scaleVm = form.Root.Children.Single(field => field.Field.Name == "visual").Children.Single(field => field.Field.Name == "scale");

        Assert.Equal(0.1m, scaleVm.NumericMinimum);
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
    public void Recursive_inputs_groups_and_actions_have_accessible_names_and_help()
    {
        var catalog = new SchemaCatalog();
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("abilities/cleave_strike.ability.yaml"));
        var vm = new SchemaFormViewModel(new SchemaFormDocument(catalog.GetRoot("ability.schema.yaml"), yaml));
        var view = new SchemaFormView { DataContext = vm };
        var window = new Window { Content = view };
        window.Show();

        Assert.All(view.GetVisualDescendants().OfType<SchemaFieldView>(), AssertAccessible);

        var name = view.GetVisualDescendants().OfType<TextBox>()
            .First(control => control.DataContext is SchemaFieldViewModel field && field.Path == "name");
        var range = view.GetVisualDescendants().OfType<NumericUpDown>()
            .First(control => control.DataContext is SchemaFieldViewModel field && field.Path == "range_m");
        var target = view.GetVisualDescendants().OfType<ComboBox>()
            .First(control => control.DataContext is SchemaFieldViewModel field && field.Path == "target");
        var gcd = view.GetVisualDescendants().OfType<CheckBox>()
            .First(control => control.DataContext is SchemaFieldViewModel field && field.Path == "triggers_gcd");
        AssertAccessible(name);
        AssertAccessible(range);
        AssertAccessible(target);
        AssertAccessible(gcd);

        var actionLabels = new HashSet<string> { "Add optional field", "Add item", "Move up", "Move down", "Remove" };
        var actions = view.GetVisualDescendants().OfType<Button>()
            .Where(button => actionLabels.Contains(button.Content?.ToString() ?? string.Empty))
            .ToArray();
        Assert.NotEmpty(actions);
        Assert.All(actions, AssertAccessible);
    }

    private static void AssertAccessible(Control control)
    {
        Assert.False(string.IsNullOrWhiteSpace(control.GetValue(AutomationProperties.NameProperty)));
        Assert.False(string.IsNullOrWhiteSpace(control.GetValue(AutomationProperties.HelpTextProperty)));
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
