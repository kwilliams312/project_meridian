using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Meridian.Codex.Editing;
using Meridian.Codex.Services;

namespace Meridian.Codex.ViewModels;

public partial class PackWorkspaceViewModel : ViewModelBase, IDisposable
{
    private readonly RecentWorkspaceStore _recentStore;
    private readonly PackManifestData _draft = new();
    private ContentWorkspace? _workspace;

    [ObservableProperty] private string _workspacePath = string.Empty;
    [ObservableProperty] private string _statusMessage = "Choose a content root to create a pack, or a pack folder to open.";
    [ObservableProperty] private string _newDependencyNamespace = string.Empty;
    [ObservableProperty] private string _newDependencyVersion = "0.1.0";
    [ObservableProperty] private PackDependencyData? _selectedDependency;

    public ObservableCollection<string> RecentWorkspaces { get; } = [];
    public ObservableCollection<WorkspaceDiagnostic> Diagnostics { get; } = [];

    public PackManifestData Manifest => _workspace?.Manifest.Data ?? _draft;
    public bool IsWorkspaceOpen => _workspace is not null;
    public bool IsDirty => _workspace?.IsDirty ?? false;
    public bool HasExternalConflict => _workspace?.HasExternalConflict ?? false;
    public bool CanSave => _workspace is not null && !HasExternalConflict && Diagnostics.All(d => d.Severity != DiagnosticSeverity.Error);
    public string WorkspaceHeader => _workspace is null ? "No pack open" : $"{Manifest.Namespace} · {Manifest.Version}";
    public string AggregateStatus => _workspace?.GetAggregateStatus().Summary ?? "Validation: not run · Build: not run";

    public string NamespaceError => ErrorFor("Namespace");
    public string NameError => ErrorFor("Name");
    public string VersionError => ErrorFor("Version");
    public string ContentSchemaVersionError => ErrorFor("ContentSchemaVersion");
    public string CompatibilityVersionError => ErrorFor("CompatibilityVersion");
    public string GodotVersionError => ErrorFor("GodotVersion");
    public string LicenseError => ErrorFor("License");

    public PackWorkspaceViewModel() : this(new RecentWorkspaceStore()) { }

    public PackWorkspaceViewModel(RecentWorkspaceStore recentStore)
    {
        _recentStore = recentStore;
        foreach (var path in recentStore.Load()) RecentWorkspaces.Add(path);
        SubscribeManifest(_draft);
    }

    [RelayCommand]
    private void Create()
    {
        try
        {
            if (string.IsNullOrWhiteSpace(WorkspacePath))
                throw new InvalidOperationException("Enter the content root where the namespace folder should be created.");
            ApplyWorkspace(ContentWorkspace.Create(WorkspacePath, Manifest));
            StatusMessage = $"Created {WorkspacePath}.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Create failed: {ex.Message}";
        }
    }

    [RelayCommand]
    private void Open()
    {
        try
        {
            ApplyWorkspace(ContentWorkspace.Open(WorkspacePath));
            StatusMessage = "Pack opened.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Open failed: {ex.Message}";
        }
    }

    [RelayCommand]
    private void OpenRecent(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return;
        WorkspacePath = path;
        Open();
    }

    [RelayCommand(CanExecute = nameof(CanSave))]
    private void Save()
    {
        if (_workspace is null) return;
        try
        {
            UnsubscribeManifest(_workspace.Manifest.Data);
            _workspace.Save();
            SubscribeManifest(_workspace.Manifest.Data);
            StatusMessage = "pack.yaml saved without reformatting untouched YAML.";
            RefreshState();
        }
        catch (Exception ex)
        {
            SubscribeManifest(_workspace.Manifest.Data);
            StatusMessage = $"Save failed: {ex.Message}";
            RefreshState();
        }
    }

    [RelayCommand]
    private void AddDependency()
    {
        if (string.IsNullOrWhiteSpace(NewDependencyNamespace)) return;
        Manifest.Dependencies.Add(new(NewDependencyNamespace.Trim(), NewDependencyVersion.Trim()));
        NewDependencyNamespace = string.Empty;
        NewDependencyVersion = "0.1.0";
        RefreshState();
    }

    [RelayCommand]
    private void RemoveDependency()
    {
        if (SelectedDependency is null) return;
        Manifest.Dependencies.Remove(SelectedDependency);
        SelectedDependency = null;
        RefreshState();
    }

    private void ApplyWorkspace(ContentWorkspace workspace)
    {
        if (_workspace is not null)
        {
            UnsubscribeManifest(_workspace.Manifest.Data);
            _workspace.ExternalChange -= OnExternalChange;
            _workspace.Dispose();
        }
        _workspace = workspace;
        WorkspacePath = workspace.RootPath;
        SubscribeManifest(workspace.Manifest.Data);
        workspace.ExternalChange += OnExternalChange;
        _recentStore.Add(workspace.RootPath);
        ReloadRecents();
        OnPropertyChanged(nameof(Manifest));
        OnPropertyChanged(nameof(IsWorkspaceOpen));
        RefreshState();
    }

    private void SubscribeManifest(PackManifestData data)
    {
        data.PropertyChanged -= OnManifestPropertyChanged;
        data.PropertyChanged += OnManifestPropertyChanged;
        data.Dependencies.CollectionChanged -= OnDependenciesChanged;
        data.Dependencies.CollectionChanged += OnDependenciesChanged;
    }

    private void UnsubscribeManifest(PackManifestData data)
    {
        data.PropertyChanged -= OnManifestPropertyChanged;
        data.Dependencies.CollectionChanged -= OnDependenciesChanged;
    }

    private void OnManifestPropertyChanged(object? sender, PropertyChangedEventArgs e) => RefreshState();
    private void OnDependenciesChanged(object? sender, NotifyCollectionChangedEventArgs e) => RefreshState();

    private void OnExternalChange(object? sender, ExternalChangeResult result)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (_workspace is null) return;
            if (result == ExternalChangeResult.Reloaded)
            {
                SubscribeManifest(_workspace.Manifest.Data);
                OnPropertyChanged(nameof(Manifest));
                StatusMessage = "pack.yaml was reloaded after an external edit.";
            }
            else if (result == ExternalChangeResult.Recovered)
            {
                StatusMessage = "External edit conflict cleared after pack.yaml was restored.";
            }
            else
            {
                StatusMessage = "External edit conflict: local changes were kept and saving is blocked.";
            }
            RefreshState();
        });
    }

    private void RefreshState()
    {
        Diagnostics.Clear();
        if (_workspace is not null)
        {
            foreach (var diagnostic in _workspace.Validate().Diagnostics) Diagnostics.Add(diagnostic);
        }
        OnPropertyChanged(nameof(IsDirty));
        OnPropertyChanged(nameof(HasExternalConflict));
        OnPropertyChanged(nameof(CanSave));
        OnPropertyChanged(nameof(WorkspaceHeader));
        OnPropertyChanged(nameof(AggregateStatus));
        OnPropertyChanged(nameof(NamespaceError));
        OnPropertyChanged(nameof(NameError));
        OnPropertyChanged(nameof(VersionError));
        OnPropertyChanged(nameof(ContentSchemaVersionError));
        OnPropertyChanged(nameof(CompatibilityVersionError));
        OnPropertyChanged(nameof(GodotVersionError));
        OnPropertyChanged(nameof(LicenseError));
        SaveCommand.NotifyCanExecuteChanged();
    }

    private string ErrorFor(string field) => string.Join(" ", Diagnostics
        .Where(d => d.Field == field && d.Severity == DiagnosticSeverity.Error)
        .Select(d => d.Message));

    private void ReloadRecents()
    {
        RecentWorkspaces.Clear();
        foreach (var path in _recentStore.Load()) RecentWorkspaces.Add(path);
    }

    public void Dispose()
    {
        UnsubscribeManifest(_draft);
        if (_workspace is not null)
        {
            UnsubscribeManifest(_workspace.Manifest.Data);
            _workspace.ExternalChange -= OnExternalChange;
            _workspace.Dispose();
        }
    }
}
