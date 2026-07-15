using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Meridian.Codex.SchemaForms;

/// <summary>Internal, opt-in file host used to exercise the generic renderer before editor migration.</summary>
public sealed partial class SchemaFormFileViewModel : ObservableObject
{
    private readonly SchemaCatalog _catalog;
    private readonly string _schemaFile;
    private string _baseline;

    private SchemaFormFileViewModel(string filePath, SchemaCatalog catalog, string schemaFile, SchemaFormDocument document)
    {
        _catalog = catalog;
        _schemaFile = schemaFile;
        FilePath = filePath;
        Document = document;
        Form = new SchemaFormViewModel(document);
        _baseline = document.ToYaml();
        Revalidate();
        document.Changed += (_, _) =>
        {
            IsDirty = document.ToYaml() != _baseline;
            Revalidate();
            Status = IsValid ? "Valid changes are ready to save." : "Fix the validation errors before saving.";
            OnPropertyChanged(nameof(SaveStateDescription));
            SaveCommand.NotifyCanExecuteChanged();
        };
    }

    public string FilePath { get; }
    public string Header => $"Experimental schema form — {Path.GetFileName(FilePath)}";
    public SchemaFormDocument Document { get; }
    public SchemaFormViewModel Form { get; }
    public IReadOnlyList<SchemaDiagnostic> Diagnostics { get; private set; } = [];
    public bool IsValid => Diagnostics.Count == 0;
    public string SaveStateDescription => !IsValid
        ? "Save unavailable. Fix the field errors described in the form."
        : !IsDirty
            ? "Save unavailable. There are no unsaved changes."
            : "Save is available. Valid changes are ready to write.";
    public string? ValidationSummary => IsValid
        ? null
        : $"{Diagnostics.Count} validation error{(Diagnostics.Count == 1 ? string.Empty : "s")}. {Diagnostics[0].Message}";

    [ObservableProperty] private bool _isDirty;
    [ObservableProperty] private string _status = "Loaded. This is the internal story #668 renderer preview.";

    public static SchemaFormFileViewModel? TryCreate(string[]? args, out string? error)
    {
        error = null;
        if (args is not ["--schema-form", var schemaName, var filePath])
        {
            if (args?.Contains("--schema-form", StringComparer.Ordinal) == true)
                error = "Usage: --schema-form <schema-name> <yaml-file>. Opening the default shell.";
            return null;
        }
        var schemaFile = schemaName.EndsWith(".schema.yaml", StringComparison.Ordinal)
            ? schemaName : schemaName + ".schema.yaml";
        if (!File.Exists(filePath)) { error = $"Schema-form file does not exist: {filePath}"; return null; }
        try
        {
            var catalog = new SchemaCatalog();
            var yaml = File.ReadAllText(filePath);
            var diagnostics = catalog.Validate(schemaFile, yaml);
            if (diagnostics.Count > 0)
            {
                error = $"The file does not satisfy {schemaFile}: {diagnostics[0].Message}";
                return null;
            }
            var document = new SchemaFormDocument(catalog.GetRoot(schemaFile), yaml);
            return new SchemaFormFileViewModel(Path.GetFullPath(filePath), catalog, schemaFile, document);
        }
        catch (Exception ex)
        {
            error = $"Could not open schema form: {ex.Message}";
            return null;
        }
    }

    private bool CanSave() => IsDirty && IsValid;

    [RelayCommand(CanExecute = nameof(CanSave))]
    private void Save() => TrySave();

    public bool TrySave()
    {
        Revalidate();
        if (!IsValid)
        {
            Status = "Save blocked: fix the validation errors first.";
            SaveCommand.NotifyCanExecuteChanged();
            return false;
        }
        File.WriteAllText(FilePath, Document.ToYaml());
        _baseline = Document.ToYaml();
        IsDirty = false;
        Status = $"Saved {Path.GetFileName(FilePath)}.";
        OnPropertyChanged(nameof(SaveStateDescription));
        SaveCommand.NotifyCanExecuteChanged();
        return true;
    }

    private void Revalidate()
    {
        Diagnostics = _catalog.Validate(_schemaFile, Document.ToYaml());
        Form.SetDiagnostics(Diagnostics);
        OnPropertyChanged(nameof(Diagnostics));
        OnPropertyChanged(nameof(IsValid));
        OnPropertyChanged(nameof(ValidationSummary));
        OnPropertyChanged(nameof(SaveStateDescription));
        SaveCommand.NotifyCanExecuteChanged();
    }
}
