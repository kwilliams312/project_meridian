using System.Linq;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Presenters;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Interactivity;
using Avalonia.Styling;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Meridian.Codex;
using Meridian.Codex.Controls;
using Meridian.Codex.Tests;
using Meridian.Codex.ViewModels;
using Meridian.Codex.Views;
using Xunit;

// Registers a headless Avalonia application so shell Views/XAML can be loaded and
// shown in tests with no display attached.
[assembly: AvaloniaTestApplication(typeof(TestAppBuilder))]

namespace Meridian.Codex.Tests;

/// <summary>Headless Avalonia bootstrap for the test assembly.</summary>
public sealed class TestAppBuilder
{
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false });
}

/// <summary>
/// Runtime smoke tests: prove the shell XAML actually loads and binds, which the
/// pure-VM tests cannot catch. Guards against App.axaml / MainWindow.axaml /
/// ViewLocator wiring errors that only surface when Avalonia initialises.
/// </summary>
public class HeadlessShellTests
{
    [AvaloniaFact]
    public void Capture_visual_evidence()
    {
        var output = Environment.GetEnvironmentVariable("CODEX_VISUAL_EVIDENCE_DIR");
        if (string.IsNullOrWhiteSpace(output)) return;
        Directory.CreateDirectory(output);
        foreach (var editor in new[] { "Pack", "NPCs", "Items" })
        {
            CaptureShell(editor, 1440, 900, Path.Combine(output, $"shell-{editor.ToLowerInvariant()}-1440x900.png"));
            CaptureShell(editor, 760, 700, Path.Combine(output, $"shell-{editor.ToLowerInvariant()}-520-canvas.png"));
        }
        CaptureSchema("ability", "content/core/abilities/cleave_strike.ability.yaml", 1440, 900,
            Path.Combine(output, "schema-ability-1440x900.png"));
        CaptureSchema("ability", "content/core/abilities/cleave_strike.ability.yaml", 520, 700,
            Path.Combine(output, "schema-ability-520x700.png"));
        CaptureSchema("item", "content/core/items/rusty_pickaxe.item.yaml", 1440, 900,
            Path.Combine(output, "schema-item-1440x900.png"));
        CaptureSchema("item", "content/core/items/rusty_pickaxe.item.yaml", 520, 700,
            Path.Combine(output, "schema-item-520x700.png"));
    }

    private static void CaptureShell(string editor, double width, double height, string output)
    {
        var vm = new MainWindowViewModel();
        vm.SelectedEditor = Assert.Single(vm.Editors, item => item.Name == editor);
        var window = new MainWindow { Width = width, Height = height, DataContext = vm };
        window.Show();
        SaveFrame(window, output);
        window.Close();
    }

    private static void CaptureSchema(string schema, string file, double width, double height, string output)
    {
        var contentRoot = Environment.GetEnvironmentVariable("CODEX_VISUAL_CONTENT_ROOT");
        if (!string.IsNullOrWhiteSpace(contentRoot)) file = Path.Combine(contentRoot, file);
        var vm = SchemaForms.SchemaFormFileViewModel.TryCreate(["--schema-form", schema, file], out var error);
        Assert.Null(error);
        var window = new SchemaFormWindow { Width = width, Height = height, DataContext = vm };
        window.Show();
        SaveFrame(window, output);
        window.Close();
    }

    private static void SaveFrame(Window window, string output)
    {
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick(2);
        window.CaptureRenderedFrame();
        var bitmap = window.GetLastRenderedFrame();
        Assert.NotNull(bitmap);
        using var stream = File.Create(output);
        bitmap!.Save(stream);
    }

    [AvaloniaFact]
    public void Codex_theme_exposes_semantic_AA_palette_and_shared_control_metrics()
    {
        Assert.True(Application.Current!.TryGetResource("CodexCanvasBrush", ThemeVariant.Dark, out var canvas));
        Assert.True(Application.Current.TryGetResource("CodexTextBrush", ThemeVariant.Dark, out var text));
        Assert.True(Application.Current.TryGetResource("CodexAccentBrush", ThemeVariant.Dark, out var accent));
        Assert.True(Application.Current.TryGetResource("CodexErrorBrush", ThemeVariant.Dark, out var error));
        Assert.True(Application.Current.TryGetResource("CodexControlHeight", ThemeVariant.Dark, out var height));

        Assert.Equal(Color.Parse("#090F14"), Assert.IsType<SolidColorBrush>(canvas).Color);
        Assert.Equal(Color.Parse("#F4F8FA"), Assert.IsType<SolidColorBrush>(text).Color);
        Assert.Equal(Color.Parse("#1476A8"), Assert.IsType<SolidColorBrush>(accent).Color);
        Assert.Equal(Color.Parse("#FFAAA8"), Assert.IsType<SolidColorBrush>(error).Color);
        Assert.Equal(38d, height);
    }

    [AvaloniaFact]
    public void Rendered_key_zones_have_distinct_surfaces_and_strong_control_boundaries()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { Width = 1440, Height = 900, DataContext = vm };
        window.Show();
        var navigation = Assert.IsType<Border>(window.FindControl<Border>("NavigationZone"));
        var workspace = Assert.IsAssignableFrom<Panel>(window.FindControl<Panel>("WorkspaceZone"));
        var pack = Assert.Single(window.GetVisualDescendants().OfType<PackWorkspaceView>());
        var card = Assert.IsType<Border>(pack.FindControl<Border>("ManifestCard"));
        var input = Assert.Single(card.GetVisualDescendants().OfType<TextBox>(), box => box.Text == "my_pack");
        var rail = Assert.Single(window.GetVisualDescendants().OfType<ListBox>(), list => list.Classes.Contains("editor-rail"));
        var selected = Assert.IsType<ListBoxItem>(rail.ContainerFromIndex(0));

        Assert.NotEqual(RenderedColor(navigation.Background), RenderedColor(workspace.Background));
        Assert.NotEqual(RenderedColor(workspace.Background), RenderedColor(card.Background));
        Assert.True(ContrastRatio(input.BorderBrush, input.Background) >= 3,
            $"Input boundary contrast was {ContrastRatio(input.BorderBrush, input.Background):F2}:1.");
        Assert.Equal(new Thickness(3, 0, 0, 0), selected.BorderThickness);
        Assert.Equal(Color.Parse("#82D5FA"), RenderedColor(selected.BorderBrush));
        Assert.True(card.CornerRadius.TopLeft >= 8);

        var npc = new NpcEditorView { DataContext = new NpcEditorViewModel() };
        var npcWindow = new Window { Width = 1200, Height = 800, Content = npc };
        npcWindow.Show();
        var sheet = Assert.IsType<Border>(npc.FindControl<Border>("NpcEditorSheet"));
        Assert.NotEqual(RenderedColor(workspace.Background), RenderedColor(sheet.Background));
        Assert.True(sheet.Padding.Left >= 24);
        var form = sheet.GetVisualDescendants().OfType<Grid>().First(grid => grid.Classes.Contains("form-grid"));
        var fields = form.GetVisualDescendants().OfType<TextBox>().Take(2).ToList();
        Assert.Equal(2, fields.Count);
        Assert.True(fields[1].Bounds.Top - fields[0].Bounds.Bottom >= 9,
            "Resolved form rows did not retain the shared vertical rhythm.");
        npcWindow.Close();
        window.Close();
    }

    [AvaloniaFact]
    public void Rendered_semantic_states_meet_contrast_targets()
    {
        var enabledPrimary = new Button { Content = "Save", IsEnabled = true };
        enabledPrimary.Classes.Add("primary");
        var disabledPrimary = new Button { Content = "Save", IsEnabled = false };
        disabledPrimary.Classes.Add("primary");
        var secondary = new Button { Content = "Open", IsEnabled = true };
        var error = new TextBlock { Text = "Invalid value" };
        error.Classes.Add("validation-error");
        var errorSurface = new Border { Classes = { "card" }, Child = error };
        var stack = new StackPanel { Children = { enabledPrimary, disabledPrimary, secondary, errorSurface } };
        var window = new Window { Content = stack };
        window.Show();

        var enabledRatio = ContrastRatio(enabledPrimary.Foreground, RenderedBackground(enabledPrimary));
        var disabledRatio = ContrastRatio(disabledPrimary.Foreground, RenderedBackground(disabledPrimary));
        var secondaryRatio = ContrastRatio(secondary.Foreground, RenderedBackground(secondary));
        var errorRatio = ContrastRatio(error.Foreground, errorSurface.Background);

        Assert.True(enabledRatio >= 4.5, $"Enabled primary contrast was {enabledRatio:F2}:1.");
        Assert.True(disabledRatio >= 4.5, $"Disabled primary contrast was {disabledRatio:F2}:1.");
        Assert.True(secondaryRatio >= 4.5, $"Secondary contrast was {secondaryRatio:F2}:1.");
        Assert.True(errorRatio >= 4.5, $"Error contrast was {errorRatio:F2}:1.");
        Assert.False(disabledPrimary.IsEffectivelyEnabled);
        Assert.Equal(FontWeight.Normal, disabledPrimary.FontWeight);
        Assert.NotEqual(RenderedColor(enabledPrimary.Background), RenderedColor(disabledPrimary.Background));
    }

    private static IBrush? RenderedBackground(Button button) =>
        button.GetVisualDescendants().OfType<ContentPresenter>().FirstOrDefault()?.Background ?? button.Background;

    private static double ContrastRatio(IBrush? foreground, IBrush? background)
    {
        var light = RelativeLuminance(RenderedColor(foreground));
        var dark = RelativeLuminance(RenderedColor(background));
        return (Math.Max(light, dark) + 0.05) / (Math.Min(light, dark) + 0.05);
    }

    private static Color RenderedColor(IBrush? brush) =>
        brush is ISolidColorBrush solid ? solid.Color : throw new Xunit.Sdk.XunitException(
            $"Expected a resolved solid brush, received {brush?.GetType().Name ?? "null"}.");

    private static double RelativeLuminance(Color color) =>
        0.2126 * LinearChannel(color.R) + 0.7152 * LinearChannel(color.G) + 0.0722 * LinearChannel(color.B);

    private static double LinearChannel(byte value)
    {
        var channel = value / 255d;
        return channel <= 0.04045 ? channel / 12.92 : Math.Pow((channel + 0.055) / 1.055, 2.4);
    }

    [AvaloniaFact]
    public void MainWindow_loads_xaml_and_binds_the_viewmodel_title()
    {
        var window = new MainWindow { DataContext = new MainWindowViewModel() };
        window.Show();

        Assert.IsType<MainWindowViewModel>(window.DataContext);
        Assert.Equal("Meridian Codex — Content Editor", window.Title);
    }

    [AvaloniaFact]
    public void Desktop_startup_preserves_default_picker_shell_and_schema_form_mode()
    {
        var shell = App.CreateDesktopWindow(null, out var shellError);
        Assert.Null(shellError);
        Assert.IsType<MainWindow>(shell);
        Assert.IsType<MainWindowViewModel>(shell.DataContext);

        var copy = ContentFixtures.CopyToTemp("abilities/cleave_strike.ability.yaml");
        var schemaForm = App.CreateDesktopWindow(["--schema-form", "ability", copy], out var previewError);
        Assert.Null(previewError);
        Assert.IsType<SchemaFormWindow>(schemaForm);
        Assert.IsType<Meridian.Codex.SchemaForms.SchemaFormFileViewModel>(schemaForm.DataContext);
    }

    [AvaloniaFact]
    public void MainWindow_hosts_the_npc_editor_view_for_the_npcs_selection()
    {
        var vm = new MainWindowViewModel();
        vm.SelectedEditor = vm.Editors.Single(e => e.Name == "NPCs");
        var window = new MainWindow { DataContext = vm };
        window.Show();

        Assert.Same(vm.NpcEditor, vm.ActiveEditor);
        Assert.NotNull(window.GetVisualDescendants().OfType<NpcEditorView>().FirstOrDefault());
    }

    [AvaloniaFact]
    public void MainWindow_opens_on_the_pack_workspace_and_renders_manifest_fields()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        Assert.Same(vm.PackWorkspace, vm.ActiveEditor);
        Assert.NotNull(window.GetVisualDescendants().OfType<PackWorkspaceView>().FirstOrDefault());
        Assert.Contains(window.GetVisualDescendants().OfType<TextBox>(), b => b.Text == "my_pack");
    }

    [AvaloniaFact]
    public void Pack_workspace_renders_one_sticky_bottom_action_using_the_save_command()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        var actionBar = Assert.Single(window.GetVisualDescendants().OfType<EditorActionBar>());
        Assert.Same(vm.PackWorkspace.SaveCommand, actionBar.ActionCommand);
        Assert.Equal("Open or create a pack to save changes.", actionBar.StateDescription);
        var layout = Assert.IsType<Grid>(actionBar.Parent);
        Assert.Equal(1, Grid.GetRow(actionBar));
        Assert.Contains(layout.Children.OfType<ScrollViewer>(), scroll => Grid.GetRow(scroll) == 0);
        var save = Assert.Single(actionBar.GetVisualDescendants().OfType<Button>());
        Assert.Equal("Save manifest", save.Content);
        Assert.Same(vm.PackWorkspace.SaveCommand, save.Command);
        Assert.False(save.Command!.CanExecute(null));
        Assert.False(save.IsEnabled);
        Assert.Equal(HorizontalAlignment.Right, save.HorizontalAlignment);
        Assert.DoesNotContain(window.GetVisualDescendants().OfType<Button>(), button =>
            button != save && button.Content?.ToString() == "Save manifest");
    }

    [AvaloniaFact]
    public void MainWindow_save_shortcuts_share_the_save_commands_can_execute_gate()
    {
        var vm = new MainWindowViewModel();
        var window = new MainWindow { DataContext = vm };
        window.Show();

        var shortcuts = window.KeyBindings
            .Where(binding => binding.Gesture?.Key == Key.S)
            .ToList();
        Assert.Equal(2, shortcuts.Count);
        Assert.Contains(shortcuts, binding => binding.Gesture!.KeyModifiers == KeyModifiers.Control);
        Assert.Contains(shortcuts, binding => binding.Gesture!.KeyModifiers == KeyModifiers.Meta);
        Assert.All(shortcuts, binding =>
        {
            Assert.Same(vm.PackWorkspace.SaveCommand, binding.Command);
            Assert.False(binding.Command!.CanExecute(null));
        });
    }

    [AvaloniaFact]
    public void NpcEditorView_renders_an_ability_row_without_binding_errors()
    {
        // Exercises the ability-rotation ItemsControl DataTemplate — including the
        // compiled RemoveAbility command binding — with a live row present.
        var vm = new NpcEditorViewModel();
        vm.AddAbilityCommand.Execute(null);
        vm.Abilities[0].Ability = "ability.pickaxe_slam";

        var view = new NpcEditorView { DataContext = vm };
        var window = new Window { Content = view };
        window.Show();

        var boxes = view.GetVisualDescendants().OfType<TextBox>().ToList();
        Assert.Contains(boxes, b => b.Text == "ability.pickaxe_slam");
    }

    [AvaloniaFact]
    public void Desktop_editor_layout_keeps_readable_form_and_resizable_preview_at_1440_by_900()
    {
        var view = new NpcEditorView { DataContext = new NpcEditorViewModel() };
        var window = new Window { Width = 1440, Height = 900, Content = view };
        window.Show();

        var split = Assert.Single(view.GetVisualDescendants().OfType<EditorSplitView>());
        // Avalonia.Headless fixes top-levels at 1024px. Arrange the editor at
        // the 1440px shell's post-navigation content width explicitly.
        split.InvalidateMeasure();
        split.Measure(new Size(1200, 760));
        split.Arrange(new Rect(0, 0, 1200, 760));
        var splitter = Assert.Single(split.GetVisualDescendants().OfType<GridSplitter>());
        Assert.True(split.IsPreviewOpen);
        Assert.True(split.Bounds.Width >= 1000);
        Assert.InRange(split.PreviewWidth, 300, 560);
        Assert.True(splitter.IsVisible);

        var fieldLabels = view.GetVisualDescendants().OfType<TextBlock>()
            .Where(label => label.Classes.Contains("field"))
            .ToList();
        Assert.NotEmpty(fieldLabels);
        Assert.All(fieldLabels, label => Assert.True(label.Bounds.Width >= 150,
            $"Field label '{label.Text}' was allocated only {label.Bounds.Width}px."));
    }

    [AvaloniaFact]
    public void Laptop_width_collapses_preview_and_keeps_primary_action_sticky()
    {
        var view = new ItemEditorView { DataContext = new ItemEditorViewModel() };
        var window = new Window { Width = 760, Height = 700, Content = view };
        window.Show();

        var split = Assert.Single(view.GetVisualDescendants().OfType<EditorSplitView>());
        // Model the available editor canvas after the navigation rail at a
        // representative laptop width; headless top-levels themselves are fixed.
        split.InvalidateMeasure();
        split.Measure(new Size(620, 560));
        split.Arrange(new Rect(0, 0, 620, 560));
        Assert.False(split.IsPreviewOpen);
        Assert.InRange(split.PreviewWidth, 0, 56);
        var showPreview = Assert.Single(split.GetVisualDescendants().OfType<Button>(),
            button => button.Content?.ToString() == "YAML");
        Assert.True(showPreview.IsVisible);

        var actionBar = Assert.Single(view.GetVisualDescendants().OfType<EditorActionBar>());
        Assert.Equal("Save item", actionBar.ActionText);
        Assert.Same(((ItemEditorViewModel)view.DataContext!).SaveCommand, actionBar.ActionCommand);
        Assert.True(actionBar.Bounds.Height > 0);
        Assert.True(actionBar.Bounds.Bottom <= view.Bounds.Bottom);
    }

    [AvaloniaFact]
    public void Preview_can_be_collapsed_and_reopened_without_recreating_the_editor()
    {
        var split = new EditorSplitView
        {
            Width = 1100,
            Height = 700,
            PreviewText = "schema: meridian/npc@1",
            FormContent = new TextBox { Text = "Form content" },
        };
        var window = new Window { Width = 1100, Height = 700, Content = split };
        window.Show();
        Assert.True(split.IsPreviewOpen);

        split.SetPreviewOpen(false);
        Assert.False(split.IsPreviewOpen);
        Assert.InRange(split.PreviewWidth, 0, 56);

        split.SetPreviewOpen(true);
        Assert.True(split.IsPreviewOpen);
        Assert.InRange(split.PreviewWidth, 300, 560);
        Assert.Contains(split.GetVisualDescendants().OfType<TextBox>(), box =>
            box.IsReadOnly && box.Text == "schema: meridian/npc@1");
    }

    [AvaloniaFact]
    public void Narrow_user_reopen_uses_reversible_single_pane_then_restores_desktop_split()
    {
        var view = new ItemEditorView { DataContext = new ItemEditorViewModel() };
        var window = new Window { Width = 520, Height = 560, Content = view };
        window.Show();
        var split = Assert.Single(view.GetVisualDescendants().OfType<EditorSplitView>());
        var form = Assert.IsType<ContentPresenter>(split.FindControl<ContentPresenter>("FormPresenter"));
        var pane = Assert.IsType<Border>(split.FindControl<Border>("PreviewPane"));
        var splitter = Assert.IsType<GridSplitter>(split.FindControl<GridSplitter>("PreviewSplitter"));
        var show = Assert.IsType<Button>(split.FindControl<Button>("ShowPreviewButton"));

        Arrange(split, 520, 560);
        Assert.False(split.IsPreviewOpen);
        Assert.True(form.IsVisible);

        show.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Arrange(split, 520, 560);

        Assert.True(split.IsSinglePanePreview);
        Assert.False(form.IsVisible);
        Assert.False(splitter.IsVisible);
        Assert.Equal(0, pane.Bounds.Left);
        Assert.InRange(pane.Bounds.Right, 519.5, 520.5);
        var hide = Assert.Single(pane.GetVisualDescendants().OfType<Button>(),
            button => AutomationProperties.GetName(button) == "Hide YAML preview");
        var hideOrigin = Assert.NotNull(hide.TranslatePoint(new Point(0, 0), split));
        Assert.True(hideOrigin.X >= 0);
        Assert.True(hideOrigin.X + hide.Bounds.Width <= split.Bounds.Width + 0.5);
        Assert.True(hide.Focus(NavigationMethod.Tab));
        Assert.True(hide.IsFocused);

        hide.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Arrange(split, 520, 560);
        Assert.False(split.IsPreviewOpen);
        Assert.True(form.IsVisible);

        show.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        window.Width = 1200;
        window.Height = 760;
        Arrange(split, 1200, 760);
        Assert.True(split.IsPreviewOpen);
        Assert.False(split.IsSinglePanePreview);
        Assert.True(form.IsVisible);
        Assert.True(splitter.IsVisible);
        Assert.InRange(split.PreviewWidth, 300, 560);

        window.Width = 520;
        window.Height = 560;
        Arrange(split, 520, 560);
        Assert.True(split.IsSinglePanePreview);
        hide.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Arrange(split, 520, 560);
        Assert.False(split.IsPreviewOpen);
        Assert.True(form.IsVisible);
        var actionBar = Assert.Single(view.GetVisualDescendants().OfType<EditorActionBar>());
        Assert.True(actionBar.IsVisible);
        Assert.True(actionBar.Bounds.Bottom <= view.Bounds.Bottom);
        window.Close();
    }

    private static void Arrange(Control control, double width, double height)
    {
        control.InvalidateMeasure();
        control.InvalidateArrange();
        control.Measure(new Size(width, height));
        control.Arrange(new Rect(0, 0, width, height));
        if (control is EditorSplitView split)
        {
            // Headless top-levels remain fixed at 1024px, so flush the named
            // production layout root at the modeled host size as well.
            var layout = Assert.IsType<Grid>(split.FindControl<Grid>("LayoutGrid"));
            layout.InvalidateMeasure();
            layout.InvalidateArrange();
            layout.Measure(new Size(width, height));
            layout.Arrange(new Rect(0, 0, width, height));
        }
    }

    [AvaloniaFact]
    public async Task Pack_workspace_renders_external_conflict_after_malformed_watcher_write()
    {
        var contentRoot = Path.Combine(Path.GetTempPath(), "codex-headless-watcher", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(contentRoot);
        var recent = Path.Combine(contentRoot, "recent.json");
        using var vm = new PackWorkspaceViewModel(new Meridian.Codex.Services.RecentWorkspaceStore(recent));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        Assert.True(vm.IsWorkspaceOpen);

        File.WriteAllText(Path.Combine(vm.WorkspacePath, "pack.yaml"), "schema: [unterminated\n");
        for (var i = 0; i < 100 && !vm.HasExternalConflict; i++)
            await Task.Delay(25);

        Assert.True(vm.HasExternalConflict);
        Assert.False(vm.CanSave);
        Assert.Equal("Resolve the external pack.yaml conflict before saving.", vm.SaveStateDescription);
        var view = new PackWorkspaceView { DataContext = vm };
        var window = new Window { Content = view };
        window.Show();
        Assert.Contains(view.GetVisualDescendants().OfType<TextBlock>(),
            text => text.Text?.Contains("External conflict", StringComparison.Ordinal) == true);
    }

    [AvaloniaFact]
    public async Task Clean_external_reload_replaces_manifest_and_keeps_save_disabled()
    {
        var contentRoot = Path.Combine(Path.GetTempPath(), "codex-headless-reload", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(contentRoot);
        using var vm = new PackWorkspaceViewModel(new Meridian.Codex.Services.RecentWorkspaceStore(
            Path.Combine(contentRoot, "recent.json")));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        var manifestPath = Path.Combine(vm.WorkspacePath, "pack.yaml");
        var external = File.ReadAllText(manifestPath).Replace("Moonfall", "Externally Reloaded");

        File.WriteAllText(manifestPath, external);
        for (var i = 0; i < 100 && vm.Manifest.Name != "Externally Reloaded"; i++)
            await Task.Delay(25);

        Assert.Equal("Externally Reloaded", vm.Manifest.Name);
        Assert.False(vm.IsDirty);
        Assert.False(vm.HasExternalConflict);
        Assert.False(vm.CanSave);
        Assert.Equal("No unsaved changes.", vm.SaveStateDescription);
    }

    [AvaloniaFact]
    public async Task Pack_workspace_reenables_save_when_exact_baseline_is_restored()
    {
        var contentRoot = Path.Combine(Path.GetTempPath(), "codex-headless-recovery", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(contentRoot);
        using var vm = new PackWorkspaceViewModel(new Meridian.Codex.Services.RecentWorkspaceStore(
            Path.Combine(contentRoot, "recent.json")));
        vm.WorkspacePath = contentRoot;
        vm.Manifest.Namespace = "moonfall";
        vm.Manifest.Name = "Moonfall";
        vm.CreateCommand.Execute(null);
        Assert.True(vm.IsWorkspaceOpen);
        vm.Manifest.Name = "Local Moonfall edit";
        Assert.True(vm.CanSave);
        var manifestPath = Path.Combine(vm.WorkspacePath, "pack.yaml");
        var original = File.ReadAllText(manifestPath);
        string[] invalidDocuments =
        [
            "schema: [unterminated\n",
            original.Replace("schema: meridian/pack@1", "schema: meridian/pack@9"),
        ];

        foreach (var invalid in invalidDocuments)
        {
            File.WriteAllText(manifestPath, invalid);
            for (var i = 0; i < 100 && !vm.HasExternalConflict; i++) await Task.Delay(25);
            Assert.True(vm.HasExternalConflict);
            Assert.False(vm.CanSave);

            File.WriteAllText(manifestPath, original);
            // FileSystemWatcher refreshes the workspace on its worker thread, then
            // the VM posts diagnostics/command state to Avalonia's UI dispatcher.
            // On Linux the workspace conflict can clear one dispatcher turn before
            // CanSave observes the refreshed diagnostics. Wait for the complete UI
            // state rather than using the background workspace flag as the barrier.
            for (var i = 0; i < 100 && (vm.HasExternalConflict || !vm.CanSave); i++)
                await Task.Delay(25);
            Assert.False(vm.HasExternalConflict);
            Assert.True(vm.CanSave);
        }
    }
}
