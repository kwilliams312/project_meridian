using Avalonia;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Meridian.Codex;
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
            .UseHeadless(new AvaloniaHeadlessPlatformOptions());
}

/// <summary>
/// Runtime smoke tests: prove the shell XAML actually loads and binds, which the
/// pure-VM tests cannot catch. Guards against App.axaml / MainWindow.axaml /
/// ViewLocator wiring errors that only surface when Avalonia initialises.
/// </summary>
public class HeadlessShellTests
{
    [AvaloniaFact]
    public void MainWindow_loads_xaml_and_binds_the_viewmodel_title()
    {
        var window = new MainWindow { DataContext = new MainWindowViewModel() };
        window.Show();

        Assert.IsType<MainWindowViewModel>(window.DataContext);
        Assert.Equal("Meridian Codex — Content Editor", window.Title);
    }
}
