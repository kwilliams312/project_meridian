using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Platform.Storage;

namespace Meridian.Codex.Services;

public enum EntityFileKind { Item, Npc }
public enum FolderPickerPurpose { OpenPack, CreatePack }

/// <summary>Portable seam around native desktop selection and confirmation UI.</summary>
public interface IContentDialogService
{
    bool IsNativePickerAvailable { get; }
    Task<string?> PickEntityFileAsync(EntityFileKind kind, string? currentPath, string? workspacePath = null);
    Task<string?> PickFolderAsync(FolderPickerPurpose purpose, string? currentPath);
    Task<bool> ConfirmDiscardChangesAsync(string documentName);
    Task CopyPathAsync(string path);
}

/// <summary>Headless fallback. It intentionally leaves manual paths available to tests and automation.</summary>
public sealed class HeadlessContentDialogService : IContentDialogService
{
    public static HeadlessContentDialogService Instance { get; } = new();
    public bool IsNativePickerAvailable => false;
    public Task<string?> PickEntityFileAsync(EntityFileKind kind, string? currentPath, string? workspacePath = null) =>
        Task.FromResult<string?>(null);
    public Task<string?> PickFolderAsync(FolderPickerPurpose purpose, string? currentPath) => Task.FromResult<string?>(null);
    public Task<bool> ConfirmDiscardChangesAsync(string documentName) => Task.FromResult(false);
    public Task CopyPathAsync(string path) => Task.CompletedTask;
}

/// <summary>Avalonia StorageProvider implementation shared by macOS, Windows, and Linux.</summary>
public sealed class AvaloniaContentDialogService(Func<Window?> ownerProvider) : IContentDialogService
{
    public bool IsNativePickerAvailable => ownerProvider()?.StorageProvider?.CanOpen == true;

    public async Task<string?> PickEntityFileAsync(
        EntityFileKind kind, string? currentPath, string? workspacePath = null)
    {
        var owner = ownerProvider();
        if (owner?.StorageProvider is not { CanOpen: true } storage) return null;

        var suffix = kind == EntityFileKind.Item ? "item" : "npc";
        var start = await ResolveStartFolderAsync(storage, currentPath, workspacePath);
        var selected = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = $"Open Meridian {suffix.ToUpperInvariant()}",
            AllowMultiple = false,
            SuggestedStartLocation = start,
            FileTypeFilter =
            [
                new FilePickerFileType($"Meridian {suffix} YAML")
                {
                    Patterns = [$"*.{suffix}.yaml", $"*.{suffix}.yml"],
                    MimeTypes = ["application/yaml", "text/yaml", "text/x-yaml"],
                },
                new FilePickerFileType("YAML") { Patterns = ["*.yaml", "*.yml"] },
            ],
        });
        owner.Activate();
        return selected.Count == 1 ? selected[0].TryGetLocalPath() : null;
    }

    public async Task<string?> PickFolderAsync(FolderPickerPurpose purpose, string? currentPath)
    {
        var owner = ownerProvider();
        if (owner?.StorageProvider is not { CanPickFolder: true } storage) return null;
        var selected = await storage.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = purpose == FolderPickerPurpose.OpenPack
                ? "Choose a Meridian pack folder"
                : "Choose the content folder for the new pack",
            AllowMultiple = false,
            SuggestedStartLocation = await ResolveStartFolderAsync(storage, currentPath),
        });
        owner.Activate();
        return selected.Count == 1 ? selected[0].TryGetLocalPath() : null;
    }

    public async Task<bool> ConfirmDiscardChangesAsync(string documentName)
    {
        var owner = ownerProvider();
        if (owner is null) return false;

        var discard = new Button { Content = "_Discard changes", IsDefault = false };
        AutomationProperties.SetName(discard, $"Discard unsaved changes to {documentName}");
        var cancel = new Button { Content = "_Cancel", IsCancel = true, IsDefault = true };
        AutomationProperties.SetName(cancel, "Cancel and keep editing");
        var dialog = new Window
        {
            Title = "Unsaved changes",
            Width = 440,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Avalonia.Thickness(24),
                Spacing = 18,
                Children =
                {
                    new TextBlock
                    {
                        Text = $"Discard unsaved changes to {documentName}?",
                        TextWrapping = Avalonia.Media.TextWrapping.Wrap,
                        FontSize = 18,
                        FontWeight = Avalonia.Media.FontWeight.SemiBold,
                    },
                    new TextBlock
                    {
                        Text = "Your edits have not been saved. This action cannot be undone.",
                        TextWrapping = Avalonia.Media.TextWrapping.Wrap,
                    },
                    new StackPanel
                    {
                        Orientation = Orientation.Horizontal,
                        HorizontalAlignment = HorizontalAlignment.Right,
                        Spacing = 8,
                        Children = { cancel, discard },
                    },
                },
            },
        };
        discard.Click += (_, _) => dialog.Close(true);
        cancel.Click += (_, _) => dialog.Close(false);
        var result = await dialog.ShowDialog<bool>(owner);
        owner.Activate();
        return result;
    }

    public async Task CopyPathAsync(string path)
    {
        var owner = ownerProvider();
        if (owner?.Clipboard is not null && !string.IsNullOrWhiteSpace(path))
            await owner.Clipboard.SetTextAsync(path);
        owner?.Activate();
    }

    private static async Task<IStorageFolder?> ResolveStartFolderAsync(
        IStorageProvider storage, params string?[] candidates)
    {
        foreach (var candidate in candidates)
        {
            if (string.IsNullOrWhiteSpace(candidate)) continue;
            var path = File.Exists(candidate) ? Path.GetDirectoryName(candidate) : candidate;
            while (!string.IsNullOrWhiteSpace(path) && !Directory.Exists(path))
                path = Path.GetDirectoryName(path);
            if (string.IsNullOrWhiteSpace(path)) continue;
            var folder = await storage.TryGetFolderFromPathAsync(new Uri(Path.GetFullPath(path)));
            if (folder is not null) return folder;
        }
        return null;
    }
}

public static class PathPresentation
{
    public static string Compact(string? path, int maxLength = 52)
    {
        if (string.IsNullOrWhiteSpace(path)) return "No file selected";
        string full;
        try { full = Path.GetFullPath(path); }
        catch (Exception) { full = path; }
        if (full.Length <= maxLength) return full;
        var file = Path.GetFileName(full);
        var parent = Path.GetFileName(Path.GetDirectoryName(full));
        var tail = string.IsNullOrEmpty(parent) ? file : Path.Combine(parent, file);
        var prefix = $"…{Path.DirectorySeparatorChar}";
        if (prefix.Length + tail.Length <= maxLength) return prefix + tail;
        var keep = Math.Max(1, maxLength - prefix.Length - 1);
        return prefix + "…" + tail[^Math.Min(keep, tail.Length)..];
    }
}
