using System.Text;
using Meridian.Codex.Editing;
using Meridian.Yaml.Cst;

namespace Meridian.Codex.Services;

public enum ExternalChangeResult { None, Reloaded, Recovered, Conflict }
public enum WorkspaceBuildState { NotRun, Succeeded, Failed }

public sealed record WorkspaceAggregateStatus(
    WorkspaceValidationResult Validation,
    WorkspaceBuildState BuildState,
    string BuildMessage,
    bool IsDirty,
    bool HasExternalConflict,
    string Summary);

public sealed class WorkspaceConflictException(string message) : IOException(message);

/// <summary>
/// One opened <c>content/&lt;namespace&gt;</c> directory, including manifest,
/// dependency loading, external-change protection, and aggregate status.
/// </summary>
public sealed class ContentWorkspace : IDisposable
{
    private static readonly UTF8Encoding Utf8 = new(false);
    private readonly FileSystemWatcher _watcher;
    private string _lastDiskText;
    private readonly object _refreshGate = new();
    private IReadOnlyList<WorkspaceDiagnostic> _externalDiagnostics = [];

    private ContentWorkspace(string rootPath, PackManifestDocument manifest)
    {
        RootPath = Path.GetFullPath(rootPath);
        ContentRoot = Directory.GetParent(RootPath)?.FullName
            ?? throw new DirectoryNotFoundException("A pack directory must have a content-root parent.");
        ManifestPath = Path.Combine(RootPath, "pack.yaml");
        Manifest = manifest;
        _lastDiskText = File.ReadAllText(ManifestPath, Utf8);
        Validation = Validate();
        _watcher = new FileSystemWatcher(RootPath, "pack.yaml")
        {
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.Size | NotifyFilters.FileName,
            EnableRaisingEvents = true,
        };
        _watcher.Changed += OnManifestChanged;
        _watcher.Created += OnManifestChanged;
        _watcher.Renamed += OnManifestChanged;
        _watcher.Deleted += OnManifestChanged;
    }

    public string RootPath { get; }
    public string ContentRoot { get; }
    public string ManifestPath { get; }
    public PackManifestDocument Manifest { get; private set; }
    public WorkspaceValidationResult Validation { get; private set; }
    public bool IsDirty => Manifest.IsDirty;
    public bool HasExternalConflict { get; private set; }
    public WorkspaceBuildState BuildState { get; private set; }
    public string BuildMessage { get; private set; } = "Not run";

    public event EventHandler<ExternalChangeResult>? ExternalChange;

    public static ContentWorkspace Open(string packDirectory)
    {
        var root = Path.GetFullPath(packDirectory);
        var manifestPath = Path.Combine(root, "pack.yaml");
        if (!File.Exists(manifestPath))
            throw new FileNotFoundException("The selected directory does not contain pack.yaml.", manifestPath);
        return new ContentWorkspace(root, PackManifestDocument.Load(manifestPath));
    }

    public static ContentWorkspace Create(string contentRoot, string namespace_, string name)
        => Create(contentRoot, new PackManifestData { Namespace = namespace_, Name = name });

    public static ContentWorkspace Create(string contentRoot, PackManifestData values)
    {
        var fullContentRoot = Path.GetFullPath(contentRoot);
        var manifest = PackManifestDocument.New(values);
        var validation = WorkspaceValidator.Validate(manifest, fullContentRoot);
        if (validation.State == WorkspaceValidationState.Invalid)
            throw new ArgumentException(validation.Diagnostics[0].Message, nameof(values));

        var root = Path.Combine(fullContentRoot, values.Namespace);
        if (Directory.Exists(root) && Directory.EnumerateFileSystemEntries(root).Any())
            throw new IOException($"Pack directory already exists and is not empty: {root}");
        Directory.CreateDirectory(root);
        var manifestPath = Path.Combine(root, "pack.yaml");
        manifest.Save(manifestPath);
        return new ContentWorkspace(root, PackManifestDocument.Load(manifestPath));
    }

    public WorkspaceValidationResult Validate()
    {
        var local = WorkspaceValidator.Validate(Manifest, ContentRoot);
        Validation = _externalDiagnostics.Count == 0
            ? local
            : new WorkspaceValidationResult(local.Diagnostics.Concat(_externalDiagnostics).ToArray());
        return Validation;
    }

    public void Save()
    {
        // Close the race between the last watcher notification and Save: compare
        // the disk bytes synchronously before writing anything.
        RefreshFromDisk();
        if (HasExternalConflict)
            throw new WorkspaceConflictException("pack.yaml changed on disk while local edits were unsaved. Reopen or resolve before saving.");
        var validation = Validate();
        if (validation.State == WorkspaceValidationState.Invalid)
            throw new InvalidOperationException("The pack manifest has validation errors and was not saved.");

        _watcher.EnableRaisingEvents = false;
        try
        {
            Manifest.Save(ManifestPath);
            _lastDiskText = File.ReadAllText(ManifestPath, Utf8);
            Manifest = PackManifestDocument.Load(ManifestPath);
        }
        finally
        {
            _watcher.EnableRaisingEvents = true;
        }
        HasExternalConflict = false;
        _externalDiagnostics = [];
        Validation = Validate();
    }

    public ExternalChangeResult RefreshFromDisk()
    {
        lock (_refreshGate)
        {
            if (!File.Exists(ManifestPath))
            {
                SetExternalConflict("pack.yaml was deleted outside Codex.");
                return ExternalChangeResult.Conflict;
            }
            var disk = File.ReadAllText(ManifestPath, Utf8);
            if (disk == _lastDiskText)
            {
                if (!HasExternalConflict) return ExternalChangeResult.None;
                HasExternalConflict = false;
                _externalDiagnostics = [];
                Validation = Validate();
                return ExternalChangeResult.Recovered;
            }
            if (IsDirty)
            {
                SetExternalConflict("pack.yaml changed outside Codex while local edits are unsaved.");
                return ExternalChangeResult.Conflict;
            }
            PackManifestDocument candidate;
            try
            {
                candidate = PackManifestDocument.Parse(disk, ManifestPath);
            }
            catch (YamlCstException ex)
            {
                SetExternalConflict($"External pack.yaml is malformed: {ex.Message}");
                return ExternalChangeResult.Conflict;
            }
            var candidateValidation = WorkspaceValidator.Validate(candidate, ContentRoot);
            if (candidateValidation.State == WorkspaceValidationState.Invalid)
            {
                HasExternalConflict = true;
                _externalDiagnostics = candidateValidation.Diagnostics;
                Validation = Validate();
                return ExternalChangeResult.Conflict;
            }
            Manifest = candidate;
            _lastDiskText = disk;
            HasExternalConflict = false;
            _externalDiagnostics = [];
            Validation = Validate();
            return ExternalChangeResult.Reloaded;
        }
    }

    private void SetExternalConflict(string message)
    {
        HasExternalConflict = true;
        _externalDiagnostics = [new WorkspaceDiagnostic("pack.yaml", message)];
        Validation = Validate();
    }

    public void RecordBuildResult(bool succeeded, string message)
    {
        BuildState = succeeded ? WorkspaceBuildState.Succeeded : WorkspaceBuildState.Failed;
        BuildMessage = message;
    }

    public WorkspaceAggregateStatus GetAggregateStatus()
    {
        var validation = Validate();
        var validationText = validation.State == WorkspaceValidationState.Valid
            ? "Validation: valid"
            : $"Validation: {validation.ErrorCount} error{(validation.ErrorCount == 1 ? "" : "s")}";
        var buildText = BuildState switch
        {
            WorkspaceBuildState.Succeeded => "Build: succeeded",
            WorkspaceBuildState.Failed => "Build: failed",
            _ => "Build: not run",
        };
        return new(validation, BuildState, BuildMessage, IsDirty, HasExternalConflict,
            $"{validationText} · {buildText}{(IsDirty ? " · Unsaved changes" : "")}{(HasExternalConflict ? " · External conflict" : "")}");
    }

    private void OnManifestChanged(object? sender, FileSystemEventArgs e)
    {
        try
        {
            var result = RefreshFromDisk();
            if (result != ExternalChangeResult.None) ExternalChange?.Invoke(this, result);
        }
        catch (IOException)
        {
            // Editors commonly save via atomic replace. A subsequent watcher event,
            // or the next explicit refresh, observes the stable file.
        }
    }

    public void Dispose() => _watcher.Dispose();
}
