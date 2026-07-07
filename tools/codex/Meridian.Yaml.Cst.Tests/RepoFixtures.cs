namespace Meridian.Yaml.Cst.Tests;

/// <summary>
/// Locates the repository's real content YAML files so the CST layer is tested against
/// the exact hand-authored, commented, non-default-formatted content it must protect
/// (tools-sad.md §6.2; the task requires content/core files as fixtures).
/// </summary>
public static class RepoFixtures
{
    /// <summary>The repository root (the directory that contains <c>content/core</c>).</summary>
    public static string RepoRoot { get; } = FindRepoRoot();

    /// <summary>Absolute path to <c>content/core</c>.</summary>
    public static string ContentCore => Path.Combine(RepoRoot, "content", "core");

    /// <summary>All <c>*.yaml</c> files under <c>content/core</c>, sorted for determinism.</summary>
    public static IEnumerable<object[]> AllContentYaml()
    {
        return Directory
            .EnumerateFiles(ContentCore, "*.yaml", SearchOption.AllDirectories)
            .OrderBy(p => p, StringComparer.Ordinal)
            .Select(p => new object[] { Path.GetRelativePath(ContentCore, p) });
    }

    /// <summary>Resolve a content-core-relative path to an absolute path.</summary>
    public static string Absolute(string relative) => Path.Combine(ContentCore, relative);

    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "content", "core")))
            {
                return dir.FullName;
            }

            dir = dir.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not locate repo root (a directory containing content/core) above " +
            AppContext.BaseDirectory);
    }
}
