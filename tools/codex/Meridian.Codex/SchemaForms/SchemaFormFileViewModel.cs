using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Meridian.Codex.SchemaForms;

/// <summary>Internal, opt-in file host used to exercise the generic renderer before editor migration.</summary>
public sealed partial class SchemaFormFileViewModel : ObservableObject
{
    private string _baseline;

    private SchemaFormFileViewModel(string filePath, SchemaFormDocument document)
    {
        FilePath = filePath;
        Document = document;
        Form = new SchemaFormViewModel(document);
        _baseline = document.ToYaml();
        document.Changed += (_, _) => { IsDirty = document.ToYaml() != _baseline; SaveCommand.NotifyCanExecuteChanged(); };
    }

    public string FilePath { get; }
    public string Header => $"Experimental schema form — {Path.GetFileName(FilePath)}";
    public SchemaFormDocument Document { get; }
    public SchemaFormViewModel Form { get; }

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
                error = $"The file does not satisfy {schemaFile}: {diagnostics[0]}";
                return null;
            }
            var document = new SchemaFormDocument(catalog.GetRoot(schemaFile), yaml);
            return new SchemaFormFileViewModel(Path.GetFullPath(filePath), document);
        }
        catch (Exception ex)
        {
            error = $"Could not open schema form: {ex.Message}";
            return null;
        }
    }

    private bool CanSave() => IsDirty;

    [RelayCommand(CanExecute = nameof(CanSave))]
    private void Save()
    {
        File.WriteAllText(FilePath, Document.ToYaml());
        _baseline = Document.ToYaml();
        IsDirty = false;
        Status = $"Saved {Path.GetFileName(FilePath)}.";
        SaveCommand.NotifyCanExecuteChanged();
    }
}
