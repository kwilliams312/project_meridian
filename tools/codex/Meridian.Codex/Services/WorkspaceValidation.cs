using System.Text.RegularExpressions;
using Meridian.Codex.Editing;

namespace Meridian.Codex.Services;

public enum DiagnosticSeverity { Warning, Error }
public enum WorkspaceValidationState { Valid, Invalid }

public sealed record WorkspaceDiagnostic(string Field, string Message, DiagnosticSeverity Severity = DiagnosticSeverity.Error);

public sealed record WorkspaceValidationResult(IReadOnlyList<WorkspaceDiagnostic> Diagnostics)
{
    public WorkspaceValidationState State => Diagnostics.Any(d => d.Severity == DiagnosticSeverity.Error)
        ? WorkspaceValidationState.Invalid : WorkspaceValidationState.Valid;
    public int ErrorCount => Diagnostics.Count(d => d.Severity == DiagnosticSeverity.Error);
}

/// <summary>Field-level pack-schema and dependency-load validation for Codex.</summary>
public static partial class WorkspaceValidator
{
    [GeneratedRegex("^[a-z][a-z0-9_]{1,31}$", RegexOptions.CultureInvariant)]
    private static partial Regex NamespacePattern();
    [GeneratedRegex("^\\d+\\.\\d+\\.\\d+$", RegexOptions.CultureInvariant)]
    private static partial Regex SemverPattern();
    [GeneratedRegex("^4\\.\\d+$", RegexOptions.CultureInvariant)]
    private static partial Regex EnginePattern();

    public static WorkspaceValidationResult Validate(PackManifestData data, string contentRoot)
    {
        var diagnostics = new List<WorkspaceDiagnostic>();
        Check(diagnostics, NamespacePattern().IsMatch(data.Namespace), "Namespace",
            "Use 2–32 lowercase letters, digits, or underscores; start with a letter.");
        Check(diagnostics, !string.IsNullOrWhiteSpace(data.Name) && data.Name.Length <= 120, "Name",
            "Name is required and must be 120 characters or fewer.");
        Check(diagnostics, SemverPattern().IsMatch(data.Version), "Version", "Use semantic version x.y.z.");
        Check(diagnostics, data.ContentSchemaVersion == "1", "ContentSchemaVersion",
            "This Codex build supports content schema version 1.");
        Check(diagnostics, long.TryParse(data.CompatibilityVersion, out var compat) && compat >= 1,
            "CompatibilityVersion", "Compatibility version must be an integer of 1 or greater.");
        Check(diagnostics, EnginePattern().IsMatch(data.GodotVersion), "GodotVersion",
            "Godot engine pin must be a supported 4.x major/minor value (for example 4.6).");
        Check(diagnostics, !string.IsNullOrWhiteSpace(data.License), "License", "A pack license is required.");

        var seen = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < data.Dependencies.Count; i++)
        {
            var dependency = data.Dependencies[i];
            var prefix = $"Dependencies[{i}]";
            Check(diagnostics, NamespacePattern().IsMatch(dependency.Namespace), $"{prefix}.Namespace",
                "Dependency namespace is invalid.");
            Check(diagnostics, SemverPattern().IsMatch(dependency.Version), $"{prefix}.Version",
                "Dependency version must use x.y.z.");
            Check(diagnostics, dependency.Namespace != data.Namespace, $"{prefix}.Namespace",
                "A pack cannot depend on itself.");
            Check(diagnostics, seen.Add(dependency.Namespace), $"{prefix}.Namespace",
                "Dependency namespace is listed more than once.");

            if (!NamespacePattern().IsMatch(dependency.Namespace)) continue;
            var manifestPath = Path.Combine(contentRoot, dependency.Namespace, "pack.yaml");
            if (!File.Exists(manifestPath))
            {
                diagnostics.Add(new($"{prefix}.Namespace",
                    $"Dependency pack '{dependency.Namespace}' was not found under {contentRoot}."));
                continue;
            }

            try
            {
                var loaded = PackManifestDocument.Load(manifestPath).Data;
                if (loaded.Namespace != dependency.Namespace)
                {
                    diagnostics.Add(new($"{prefix}.Namespace",
                        $"Dependency manifest declares namespace '{loaded.Namespace}'."));
                }
                if (loaded.Version != dependency.Version)
                {
                    diagnostics.Add(new($"{prefix}.Version",
                        $"Dependency '{dependency.Namespace}' is version {loaded.Version}, not pinned {dependency.Version}."));
                }
            }
            catch (Exception ex)
            {
                diagnostics.Add(new($"{prefix}.Namespace",
                    $"Dependency '{dependency.Namespace}' could not be loaded: {ex.Message}"));
            }
        }

        return new WorkspaceValidationResult(diagnostics);
    }

    private static void Check(List<WorkspaceDiagnostic> diagnostics, bool condition, string field, string message)
    {
        if (!condition) diagnostics.Add(new(field, message));
    }
}
