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

/// <summary>Authoritative pack-schema and dependency-load validation for Codex.</summary>
public static class WorkspaceValidator
{
    public static WorkspaceValidationResult Validate(PackManifestDocument document, string contentRoot)
    {
        var data = document.Data;
        var diagnostics = PackSchemaValidator.Validate(document.ToYaml()).ToList();

        var seen = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < data.Dependencies.Count; i++)
        {
            var dependency = data.Dependencies[i];
            var prefix = $"Dependencies[{i}]";
            Check(diagnostics, dependency.Namespace != data.Namespace, $"{prefix}.Namespace",
                "A pack cannot depend on itself.");
            Check(diagnostics, seen.Add(dependency.Namespace), $"{prefix}.Namespace",
                "Dependency namespace is listed more than once.");

            if (!IsSafeNamespace(dependency.Namespace)) continue;
            var manifestPath = Path.Combine(contentRoot, dependency.Namespace, "pack.yaml");
            if (!File.Exists(manifestPath))
            {
                diagnostics.Add(new($"{prefix}.Namespace",
                    $"Dependency pack '{dependency.Namespace}' was not found under {contentRoot}."));
                continue;
            }

            try
            {
                var loadedDocument = PackManifestDocument.Load(manifestPath);
                var dependencySchema = PackSchemaValidator.Validate(loadedDocument.ToYaml());
                if (dependencySchema.Count > 0)
                {
                    diagnostics.Add(new($"{prefix}.Namespace",
                        $"Dependency '{dependency.Namespace}' has an invalid manifest: {dependencySchema[0].Message}"));
                    continue;
                }
                var loaded = loadedDocument.Data;
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

    // Path traversal guard only. Validity still comes exclusively from the
    // authoritative schema; unsafe values must not reach Path.Combine.
    private static bool IsSafeNamespace(string value) => value.Length is >= 2 and <= 32 &&
        value[0] is >= 'a' and <= 'z' &&
        value.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_');

    private static void Check(List<WorkspaceDiagnostic> diagnostics, bool condition, string field, string message)
    {
        if (!condition) diagnostics.Add(new(field, message));
    }
}
