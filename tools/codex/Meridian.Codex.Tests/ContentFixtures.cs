using System;
using System.Diagnostics;
using System.IO;

namespace Meridian.Codex.Tests;

/// <summary>
/// Locates the repository's real <c>content/core</c> NPC files and copies them into
/// per-test temp directories. The editor's round-trip guarantees are only meaningful
/// against the exact hand-authored bytes it must protect, but tests must never mutate
/// the committed fixtures — so every test operates on a throwaway copy.
/// </summary>
public static class ContentFixtures
{
    /// <summary>Repository root (the directory that contains <c>content/core</c>).</summary>
    public static string RepoRoot { get; } = FindRepoRoot();

    /// <summary>Absolute path to a committed <c>content/core</c> NPC, e.g. <c>npcs/kobold_miner.npc.yaml</c>.</summary>
    public static string ContentCorePath(string relative) =>
        Path.Combine(RepoRoot, "content", "core", relative);

    /// <summary>
    /// Copy a committed NPC file into a fresh temp directory and return the copy's path.
    /// The caller edits/saves the copy; the committed original is never touched.
    /// </summary>
    public static string CopyToTemp(string relative)
    {
        var dir = Path.Combine(Path.GetTempPath(), "codex-npc-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var dest = Path.Combine(dir, Path.GetFileName(relative));
        File.Copy(ContentCorePath(relative), dest);
        return dest;
    }

    /// <summary>Create a fresh temp file path (not yet written) for new-NPC save tests.</summary>
    public static string NewTempPath(string fileName)
    {
        var dir = Path.Combine(Path.GetTempPath(), "codex-npc-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, fileName);
    }

    /// <summary>Create a throwaway repo-shaped root for validator parity tests.</summary>
    public static string NewValidatorRoot()
    {
        var root = Path.Combine(Path.GetTempPath(), "codex-validator-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "schema"));
        CopyDirectory(Path.Combine(RepoRoot, "schema", "content"), Path.Combine(root, "schema", "content"));
        Directory.CreateDirectory(Path.Combine(root, "content"));
        return root;
    }

    /// <summary>Run TLS-07's reference validator and return its exit code and output.</summary>
    public static (int ExitCode, string Output) RunAuthoritativeValidator(string root)
    {
        var start = new ProcessStartInfo("uv")
        {
            WorkingDirectory = RepoRoot,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        start.ArgumentList.Add("run");
        start.ArgumentList.Add("tools/validate_content.py");
        start.ArgumentList.Add("--root");
        start.ArgumentList.Add(root);
        using var process = Process.Start(start) ?? throw new InvalidOperationException("Could not start validate_content.py.");
        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit();
        return (process.ExitCode, stdout + stderr);
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.EnumerateFiles(source))
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)));
    }

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
            "Could not locate repo root (a directory containing content/core) above " + AppContext.BaseDirectory);
    }
}
