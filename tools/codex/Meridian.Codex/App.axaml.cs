using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Data.Core.Plugins;
using Avalonia.Markup.Xaml;
using Meridian.Codex.Services;
using Meridian.Codex.ViewModels;
using Meridian.Codex.Views;
using Meridian.Codex.SchemaForms;

namespace Meridian.Codex;

/// <summary>
/// Application root. Wires the main window to its ViewModel on desktop startup.
/// </summary>
public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Avalonia's built-in binding validation duplicates the CommunityToolkit
            // DataAnnotation validation; remove it to avoid double reports (docs).
            DisableAvaloniaDataAnnotationValidation();

            desktop.MainWindow = CreateDesktopWindow(desktop.Args, out var previewError);
            if (previewError is not null)
                System.Diagnostics.Trace.TraceWarning(previewError);
        }

        base.OnFrameworkInitializationCompleted();
    }

    internal static Window CreateDesktopWindow(string[]? args, out string? previewError)
    {
        var preview = SchemaFormFileViewModel.TryCreate(args, out previewError);
        if (preview is not null)
            return new SchemaFormWindow { DataContext = preview };

        MainWindow? window = null;
        var dialogs = new AvaloniaContentDialogService(() => window);
        window = new MainWindow { DataContext = new MainWindowViewModel(dialogs) };
        return window;
    }

    private static void DisableAvaloniaDataAnnotationValidation()
    {
        var dataValidationPluginsToRemove =
            BindingPlugins.DataValidators.OfType<DataAnnotationsValidationPlugin>().ToArray();

        foreach (var plugin in dataValidationPluginsToRemove)
        {
            BindingPlugins.DataValidators.Remove(plugin);
        }
    }
}
