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
    public void Embedded_form_descriptors_overlay_schema_fields_without_replacing_constraints()
    {
        var npc = _catalog.GetRoot("npc.schema.yaml");
        var visual = npc.Children.Single(field => field.Name == "visual");
        var model = visual.Children.Single(field => field.Name == "model");
        var scale = visual.Children.Single(field => field.Name == "scale");
        Assert.Equal("presentation", model.Ui!.Group);
        Assert.Equal("Creature model", model.Ui.Label);
        Assert.Equal("asset:art", model.Ui.ReferenceType);
        Assert.Equal(["creature_model"], model.Asset!.AllowedClasses);
        Assert.Equal(["meshy"], model.Asset.EligibleGenerators);
        Assert.Equal("scale", scale.Ui!.Unit);
        Assert.True(scale.HasExclusiveMinimum);

        var item = _catalog.GetRoot("item.schema.yaml");
        var icon = item.Children.Single(field => field.Name == "visual").Children.Single(field => field.Name == "icon");
        Assert.Equal(["icon"], icon.Asset!.AllowedClasses);
        Assert.Empty(icon.Asset.EligibleGenerators);

        var ability = _catalog.GetRoot("ability.schema.yaml");
        var castVfx = ability.Children.Single(field => field.Name == "audio_visual").Children.Single(field => field.Name == "cast_vfx");
        Assert.Equal(["vfx"], castVfx.Asset!.AllowedClasses);
        Assert.Empty(castVfx.Asset.EligibleGenerators);
        Assert.Equal("Display name", ability.Children.Single(field => field.Name == "name").Ui!.Label);
    }

    [Fact]
    public void Descriptor_manifest_version_and_duplicate_paths_fail_closed()
    {
        var schemas = new Dictionary<string, JsonObject>
        {
            ["sample.schema.yaml"] = SchemaCatalog.ParseYaml("type: object\nproperties: { name: { type: string } }\n").AsObject(),
            ["common.defs.yaml"] = new(),
            ["skeleton.defs.yaml"] = new(),
        };
        var wrongVersion = JsonNode.Parse("""{"schema":"meridian/codex-form-descriptors@2","schemas":[]}""")!.AsObject();
        Assert.Throws<InvalidDataException>(() => new SchemaCatalog(schemas, wrongVersion));

        var duplicate = JsonNode.Parse("""
            {
              "schema": "meridian/codex-form-descriptors@1",
              "schemas": [{
                "schema_file": "sample.schema.yaml",
                "fields": [{"path":"name","ui":{}},{"path":"name","ui":{}}]
              }]
            }
            """)!.AsObject();
        Assert.Throws<InvalidDataException>(() => new SchemaCatalog(schemas, duplicate));
    }

    [Theory]
    [InlineData("field_unknown", "unknown key(s): surprise")]
    [InlineData("ui_unknown", "unknown key(s): ui.gropu")]
    [InlineData("ui_wrong_type", "ui.group must be a string")]
    [InlineData("documentation_invalid", "ui.documentation must be a repository docs path or HTTPS URL")]
    [InlineData("ui_not_object", "ui must be an object")]
    [InlineData("asset_unknown", "unknown key(s): asset.clas")]
    [InlineData("classes_not_array", "asset.allowed_classes must be an array of strings")]
    [InlineData("classes_nested", "asset.allowed_classes must be an array of strings")]
    [InlineData("generators_not_array", "asset.eligible_generators must be an array of strings")]
    [InlineData("no_metadata", "field must contain ui or asset metadata")]
    public void Descriptor_loader_rejects_unknown_keys_and_wrong_json_types(string scenario, string expected)
    {
        var schemas = DescriptorTestSchemas();
        var field = new JsonObject
        {
            ["path"] = "name",
            ["ui"] = new JsonObject { ["group"] = "identity" },
        };
        switch (scenario)
        {
            case "field_unknown": field["surprise"] = true; break;
            case "ui_unknown": field["ui"]!["gropu"] = "identity"; break;
            case "ui_wrong_type": field["ui"]!["group"] = 42; break;
            case "documentation_invalid": field["ui"]!["documentation"] = "docs/../secret.md"; break;
            case "ui_not_object": field["ui"] = "identity"; break;
            case "asset_unknown":
                field.Remove("ui");
                field["asset"] = new JsonObject { ["allowed_classes"] = new JsonArray("icon"), ["clas"] = "icon" };
                break;
            case "classes_not_array":
                field.Remove("ui");
                field["asset"] = new JsonObject { ["allowed_classes"] = "icon" };
                break;
            case "classes_nested":
                field.Remove("ui");
                field["asset"] = new JsonObject { ["allowed_classes"] = new JsonArray(new JsonObject { ["class"] = "icon" }) };
                break;
            case "generators_not_array":
                field.Remove("ui");
                field["asset"] = new JsonObject { ["allowed_classes"] = new JsonArray("icon"), ["eligible_generators"] = "meshy" };
                break;
            case "no_metadata": field.Remove("ui"); break;
        }
        var manifest = DescriptorManifest(field);

        var error = Assert.Throws<InvalidDataException>(() => new SchemaCatalog(schemas, manifest));

        Assert.Contains("sample.schema.yaml:name", error.Message, StringComparison.Ordinal);
        Assert.Contains(expected, error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Descriptor_loader_accepts_well_typed_custom_metadata()
    {
        var field = new JsonObject
        {
            ["path"] = "name",
            ["ui"] = new JsonObject
            {
                ["group"] = "identity",
                ["label"] = "Display name",
                ["example"] = "Example",
                ["documentation"] = "docs/content-authoring.md",
            },
            ["asset"] = new JsonObject
            {
                ["allowed_classes"] = new JsonArray("icon"),
                ["eligible_generators"] = new JsonArray(),
            },
        };

        var name = new SchemaCatalog(DescriptorTestSchemas(), DescriptorManifest(field))
            .GetRoot("sample.schema.yaml").Children.Single();

        Assert.Equal("identity", name.Ui!.Group);
        Assert.Equal("Display name", name.Ui.Label);
        Assert.Equal("Example", name.Ui.Example!.GetValue<string>());
        Assert.Equal("docs/content-authoring.md", name.Ui.Documentation);
        Assert.Equal(["icon"], name.Asset!.AllowedClasses);
        Assert.Empty(name.Asset.EligibleGenerators);
    }

    private static Dictionary<string, JsonObject> DescriptorTestSchemas() => new()
    {
        ["sample.schema.yaml"] = SchemaCatalog.ParseYaml("type: object\nproperties: { name: { type: string } }\n").AsObject(),
        ["common.defs.yaml"] = new(),
        ["skeleton.defs.yaml"] = new(),
    };

    private static JsonObject DescriptorManifest(JsonObject field) => new()
    {
        ["schema"] = "meridian/codex-form-descriptors@1",
        ["schemas"] = new JsonArray
        {
            new JsonObject
            {
                ["schema_file"] = "sample.schema.yaml",
                ["fields"] = new JsonArray(field),
            },
        },
    };

    [Fact]
    public void Representative_guidance_covers_required_optional_default_units_references_and_docs()
    {
        var pack = _catalog.GetRoot("pack.schema.yaml");
        var packNamespace = pack.Children.Single(field => field.Name == "namespace");
        var description = pack.Children.Single(field => field.Name == "description");
        Assert.True(packNamespace.IsRequired);
        Assert.Equal("emberfall", packNamespace.Ui!.Example!.ToString());
        Assert.Contains("lowercase", packNamespace.Ui.Constraint, StringComparison.OrdinalIgnoreCase);
        Assert.Equal("schema/content/README.md", packNamespace.Ui.Documentation);
        Assert.False(description.IsRequired);
        Assert.Contains("Optional summary", description.Ui!.Help, StringComparison.Ordinal);

        var npc = _catalog.GetRoot("npc.schema.yaml");
        var attackSpeed = npc.Children.Single(field => field.Name == "stats").Children.Single(field => field.Name == "attack_speed_ms");
        var npcModel = npc.Children.Single(field => field.Name == "visual").Children.Single(field => field.Name == "model");
        Assert.Equal("ms", attackSpeed.Ui!.Unit);
        Assert.Contains("500", attackSpeed.Ui.Constraint, StringComparison.Ordinal);
        Assert.Equal(["meshy"], npcModel.Asset!.EligibleGenerators);
        Assert.Contains("Meshy", npcModel.Ui!.Constraint, StringComparison.Ordinal);

        var item = _catalog.GetRoot("item.schema.yaml");
        var requiredLevel = item.Children.Single(field => field.Name == "required_level");
        var slot = item.Children.Single(field => field.Name == "slot");
        var socket = item.Children.Single(field => field.Name == "visual")
            .Children.Single(field => field.Name == "worn")
            .Children.Single(field => field.Name == "attach")
            .Children.Single(field => field.Name == "socket");
        Assert.Equal("1", requiredLevel.Default!.ToString());
        Assert.Contains("Item class is Weapon", slot.ConditionalRequirement, StringComparison.Ordinal);
        Assert.Contains("Item class is Armor", slot.ConditionalRequirement, StringComparison.Ordinal);
        Assert.Contains("character-equipment", socket.Ui!.Documentation, StringComparison.Ordinal);

        var ability = _catalog.GetRoot("ability.schema.yaml");
        var cooldown = ability.Children.Single(field => field.Name == "cooldown_ms");
        Assert.Equal("ms", cooldown.Ui!.Unit);
        Assert.Equal("6000", cooldown.Ui.Example!.ToString());
        Assert.Equal("schema/content/README.md", cooldown.Ui.Documentation);
    }

    [Fact]
    public void View_model_explains_requirement_constraints_defaults_and_branch_availability()
    {
        var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("abilities/cleave_strike.ability.yaml"));
        var form = new SchemaFormViewModel(new SchemaFormDocument(_catalog.GetRoot("ability.schema.yaml"), yaml));
        var name = form.Root.Children.Single(field => field.Path == "name");
        var cooldown = form.Root.Children.Single(field => field.Path == "cooldown_ms");
        var effect = form.Root.Children.Single(field => field.Path == "effects").Children.Single();
        var amount = effect.Children.Single(field => field.Field.Name == "amount");

        Assert.Equal("Display name *", name.Label);
        Assert.Equal("Required", name.RequirementText);
        Assert.True(name.IsRequiredForForm);
        Assert.Contains("Ability name", name.AutomationHelp, StringComparison.Ordinal);
        Assert.Equal("Optional; a default is provided", cooldown.RequirementText);
        Assert.Equal("Unit: milliseconds (ms)", cooldown.UnitText);
        Assert.Equal("Default: 0", cooldown.DefaultText);
        Assert.Equal("Example: 6000", cooldown.ExampleText);
        Assert.Contains("Kind is Damage", amount.RequirementText, StringComparison.Ordinal);
        Assert.Contains("preserved", amount.AvailabilityText, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Field_diagnostic_is_actionable_and_part_of_automation_description()
    {
        var copy = ContentFixtures.CopyToTemp("pack.yaml");
        var file = SchemaFormFileViewModel.TryCreate(["--schema-form", "pack", copy], out _)!;
        var field = file.Form.Root.Children.Single(value => value.Path == "namespace");

        field.TextValue = "Bad Namespace";

        Assert.True(field.HasDiagnostic);
        Assert.Contains("Fix:", field.DiagnosticText, StringComparison.Ordinal);
        Assert.Contains("lowercase", field.DiagnosticText, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Error:", field.AutomationHelp, StringComparison.Ordinal);
        Assert.StartsWith("Invalid:", field.AutomationStatus, StringComparison.Ordinal);
    }

    [Fact]
    public void Raw_extension_keywords_are_non_structural_without_a_descriptor_overlay()
    {
        const string schemaYaml = """
            type: object
            properties:
              name:
                type: string
                x-meridian-ui: { group: identity }
                x-meridian-asset: { allowed_classes: [icon], eligible_generators: [] }
            """;
        var schemas = new Dictionary<string, JsonObject>
        {
            ["sample.schema.yaml"] = SchemaCatalog.ParseYaml(schemaYaml).AsObject(),
            ["common.defs.yaml"] = new(),
            ["skeleton.defs.yaml"] = new(),
        };

        var name = new SchemaCatalog(schemas).GetRoot("sample.schema.yaml").Children.Single();

        Assert.Equal(SchemaFieldKind.String, name.Kind);
        Assert.False(name.IsReadOnly);
        Assert.Null(name.Ui);
        Assert.Null(name.Asset);
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
        Assert.Contains("no unsaved changes", vm.SaveStateDescription, StringComparison.OrdinalIgnoreCase);

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
        Assert.Contains("Fix the field errors", vm.SaveStateDescription, StringComparison.Ordinal);
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

    [Fact]
    public void Integer_edit_stays_numeric_through_invalid_recovery_save_and_reopen()
    {
        var copy = ContentFixtures.CopyToTemp("abilities/cleave_strike.ability.yaml");
        var original = File.ReadAllText(copy);
        var vm = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", copy], out var error)!;
        Assert.Null(error);
        var cooldown = vm.Form.Root.Children.Single(field => field.Path == "cooldown_ms");

        cooldown.NumericValue = -1;

        Assert.False(vm.IsValid);
        Assert.Contains(vm.Diagnostics, diagnostic => diagnostic.Path == "cooldown_ms");
        Assert.True(cooldown.HasDiagnostic);
        Assert.False(vm.SaveCommand.CanExecute(null));
        Assert.False(vm.TrySave());
        Assert.Equal(original, File.ReadAllText(copy));

        cooldown.NumericValue = 7000;

        Assert.True(vm.IsValid);
        Assert.False(cooldown.HasDiagnostic);
        Assert.True(vm.SaveCommand.CanExecute(null));
        Assert.Contains("cooldown_ms: 7000", vm.Document.ToYaml(), StringComparison.Ordinal);
        Assert.DoesNotContain("cooldown_ms: '7000'", vm.Document.ToYaml(), StringComparison.Ordinal);
        Assert.DoesNotContain("cooldown_ms: \"7000\"", vm.Document.ToYaml(), StringComparison.Ordinal);
        var emitted = Assert.IsAssignableFrom<JsonValue>(SchemaCatalog.ParseYaml(vm.Document.ToYaml())["cooldown_ms"]);
        Assert.True(emitted.TryGetValue<decimal>(out var value));
        Assert.Equal(7000, value);
        Assert.True(vm.TrySave());

        var reopened = SchemaFormFileViewModel.TryCreate(["--schema-form", "ability", copy], out error);
        Assert.Null(error);
        Assert.Equal("7000", reopened!.Document.Get("cooldown_ms")!.ToString());
        Assert.True(reopened.IsValid);
    }

    [Fact]
    public void Scalar_edits_render_all_integral_clr_types_as_unquoted_yaml_numbers()
    {
        var values = new (JsonValue Value, string Text)[]
        {
            (JsonValue.Create((sbyte)-2)!, "-2"),
            (JsonValue.Create((byte)2)!, "2"),
            (JsonValue.Create((short)-3)!, "-3"),
            (JsonValue.Create((ushort)3)!, "3"),
            (JsonValue.Create(-4)!, "-4"),
            (JsonValue.Create(4U)!, "4"),
            (JsonValue.Create(-5L)!, "-5"),
            (JsonValue.Create(5UL)!, "5"),
        };

        foreach (var (value, text) in values)
        {
            var yaml = File.ReadAllText(ContentFixtures.ContentCorePath("pack.yaml"));
            var document = new SchemaFormDocument(_catalog.GetRoot("pack.schema.yaml"), yaml);

            document.Set("content_schema_version", value);

            Assert.Contains($"content_schema_version: {text}", document.ToYaml(), StringComparison.Ordinal);
            Assert.DoesNotContain($"content_schema_version: '{text}'", document.ToYaml(), StringComparison.Ordinal);
            Assert.DoesNotContain($"content_schema_version: \"{text}\"", document.ToYaml(), StringComparison.Ordinal);
        }
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
    public void Representative_pack_npc_item_and_ability_forms_expose_field_semantics()
    {
        var fixtures = new[]
        {
            ("pack.schema.yaml", "pack.yaml"),
            ("npc.schema.yaml", "npcs/kobold_miner.npc.yaml"),
            ("item.schema.yaml", "items/rusty_pickaxe.item.yaml"),
            ("ability.schema.yaml", "abilities/cleave_strike.ability.yaml"),
        };
        var catalog = new SchemaCatalog();

        foreach (var (schema, fixture) in fixtures)
        {
            var yaml = File.ReadAllText(ContentFixtures.ContentCorePath(fixture));
            var vm = new SchemaFormViewModel(new SchemaFormDocument(catalog.GetRoot(schema), yaml));
            var view = new SchemaFormView { DataContext = vm };
            var window = new Window { Content = view };
            window.Show();

            var inputs = view.GetVisualDescendants()
                .OfType<Control>()
                .Where(control =>
                    control.IsVisible &&
                    (control is TextBox or NumericUpDown or ComboBox or CheckBox) &&
                    AutomationProperties.GetAutomationId(control)?.StartsWith("SchemaField_", StringComparison.Ordinal) == true)
                .ToArray();
            Assert.NotEmpty(inputs);
            Assert.All(inputs, control =>
            {
                AssertAccessible(control);
                Assert.DoesNotContain("/", AutomationProperties.GetName(control), StringComparison.Ordinal);
                Assert.False(string.IsNullOrWhiteSpace(AutomationProperties.GetAutomationId(control)));
                Assert.False(string.IsNullOrWhiteSpace(AutomationProperties.GetItemStatus(control)));
                Assert.NotNull(AutomationProperties.GetLabeledBy(control));
                var field = Assert.IsType<SchemaFieldViewModel>(control.DataContext);
                Assert.Equal(field.IsRequiredForForm, AutomationProperties.GetIsRequiredForForm(control));
            });
            Assert.Contains(view.GetVisualDescendants().OfType<TextBlock>(), block => block.Text == "Required");
            Assert.Contains(view.GetVisualDescendants().OfType<TextBlock>(), block => block.Text?.StartsWith("Optional", StringComparison.Ordinal) == true);
            window.Close();
        }
    }

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
        Assert.Contains(labels, text => text?.StartsWith("Display name", StringComparison.Ordinal) == true);
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
    public void Invalid_field_exposes_adjacent_live_error_and_invalid_automation_status()
    {
        var copy = ContentFixtures.CopyToTemp("pack.yaml");
        var file = SchemaFormFileViewModel.TryCreate(["--schema-form", "pack", copy], out _)!;
        var field = file.Form.Root.Children.Single(value => value.Path == "namespace");
        field.TextValue = "Bad Namespace";
        var view = new SchemaFormView { DataContext = file.Form };
        var window = new Window { Content = view };
        window.Show();

        var input = view.GetVisualDescendants().OfType<TextBox>()
            .Single(control => control.DataContext is SchemaFieldViewModel value && value.Path == "namespace");
        Assert.Contains("Error:", AutomationProperties.GetHelpText(input), StringComparison.Ordinal);
        Assert.StartsWith("Invalid:", AutomationProperties.GetItemStatus(input), StringComparison.Ordinal);
        Assert.True(AutomationProperties.GetIsRequiredForForm(input));
        var error = view.GetVisualDescendants().OfType<TextBlock>()
            .Single(block => block.Text?.Contains("Fix:", StringComparison.Ordinal) == true);
        Assert.Equal(AutomationLiveSetting.Assertive, AutomationProperties.GetLiveSetting(error));
        window.Close();
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
