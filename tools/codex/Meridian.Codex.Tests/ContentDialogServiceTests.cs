using System.Reflection;
using Avalonia.Platform.Storage;
using Meridian.Codex.Services;
using Xunit;

namespace Meridian.Codex.Tests;

public sealed class ContentDialogServiceTests
{
    [Theory]
    [InlineData(EntityFileKind.Item)]
    [InlineData(EntityFileKind.Npc)]
    public async Task File_open_options_request_the_process_working_directory(EntityFileKind kind)
    {
        var currentDirectory = TempDirectory();
        var folder = DispatchProxy.Create<IStorageFolder, StorageFolderProxy>();
        Uri? requested = null;
        var service = Service(currentDirectory);

        var options = await service.CreateEntityOpenOptionsAsync(kind, uri =>
        {
            requested = uri;
            return Task.FromResult<IStorageFolder?>(folder);
        });

        Assert.Equal(new Uri(Path.GetFullPath(currentDirectory)), requested);
        Assert.Same(folder, options.SuggestedStartLocation);
    }

    [Theory]
    [InlineData(EntityFileKind.Item)]
    [InlineData(EntityFileKind.Npc)]
    public async Task File_save_options_request_the_process_working_directory(EntityFileKind kind)
    {
        var currentDirectory = TempDirectory();
        var folder = DispatchProxy.Create<IStorageFolder, StorageFolderProxy>();
        Uri? requested = null;
        var service = Service(currentDirectory);

        var options = await service.CreateEntitySaveOptionsAsync(kind, uri =>
        {
            requested = uri;
            return Task.FromResult<IStorageFolder?>(folder);
        });

        Assert.Equal(new Uri(Path.GetFullPath(currentDirectory)), requested);
        Assert.Same(folder, options.SuggestedStartLocation);
    }

    [Theory]
    [InlineData(FolderPickerPurpose.OpenPack)]
    [InlineData(FolderPickerPurpose.CreatePack)]
    public async Task Folder_options_request_the_process_working_directory(FolderPickerPurpose purpose)
    {
        var currentDirectory = TempDirectory();
        var folder = DispatchProxy.Create<IStorageFolder, StorageFolderProxy>();
        Uri? requested = null;
        var service = Service(currentDirectory);

        var options = await service.CreateFolderOptionsAsync(purpose, uri =>
        {
            requested = uri;
            return Task.FromResult<IStorageFolder?>(folder);
        });

        Assert.Equal(new Uri(Path.GetFullPath(currentDirectory)), requested);
        Assert.Same(folder, options.SuggestedStartLocation);
    }

    [Fact]
    public async Task Unavailable_provider_mapping_leaves_all_picker_start_locations_unset()
    {
        var service = Service(TempDirectory());
        static Task<IStorageFolder?> Unavailable(Uri _) =>
            Task.FromException<IStorageFolder?>(new IOException("provider cannot map working directory"));

        var open = await service.CreateEntityOpenOptionsAsync(EntityFileKind.Item, Unavailable);
        var save = await service.CreateEntitySaveOptionsAsync(EntityFileKind.Npc, Unavailable);
        var folder = await service.CreateFolderOptionsAsync(FolderPickerPurpose.CreatePack, Unavailable);

        Assert.Null(open.SuggestedStartLocation);
        Assert.Null(save.SuggestedStartLocation);
        Assert.Null(folder.SuggestedStartLocation);
    }

    [Fact]
    public async Task Unresolvable_working_directory_does_not_call_provider_mapping()
    {
        var service = new AvaloniaContentDialogService(
            () => null,
            new WorkingDirectoryStartLocation(() => throw new IOException("working directory unavailable")));
        var mappingCalls = 0;
        Task<IStorageFolder?> Map(Uri _)
        {
            mappingCalls++;
            return Task.FromResult<IStorageFolder?>(null);
        }

        var open = await service.CreateEntityOpenOptionsAsync(EntityFileKind.Item, Map);
        var save = await service.CreateEntitySaveOptionsAsync(EntityFileKind.Npc, Map);
        var folder = await service.CreateFolderOptionsAsync(FolderPickerPurpose.OpenPack, Map);

        Assert.Equal(0, mappingCalls);
        Assert.Null(open.SuggestedStartLocation);
        Assert.Null(save.SuggestedStartLocation);
        Assert.Null(folder.SuggestedStartLocation);
    }

    private static AvaloniaContentDialogService Service(string currentDirectory) =>
        new(() => null, new WorkingDirectoryStartLocation(() => currentDirectory));

    private static string TempDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "codex-dialog-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    private class StorageFolderProxy : DispatchProxy
    {
        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args) =>
            throw new NotSupportedException(targetMethod?.Name);
    }
}
