using System.Text.Json;

namespace Meridian.Codex.Services;

public sealed class RecentWorkspaceStore
{
    private readonly string _path;
    private readonly int _capacity;

    public RecentWorkspaceStore(string? path = null, int capacity = 10)
    {
        _path = path ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Meridian", "Codex", "recent-workspaces.json");
        _capacity = capacity;
    }

    public IReadOnlyList<string> Load()
    {
        try
        {
            return File.Exists(_path)
                ? JsonSerializer.Deserialize<List<string>>(File.ReadAllText(_path)) ?? []
                : [];
        }
        catch (JsonException)
        {
            return [];
        }
    }

    public void Add(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var recent = Load().Where(p => !string.Equals(p, fullPath, StringComparison.OrdinalIgnoreCase)).ToList();
        recent.Insert(0, fullPath);
        if (recent.Count > _capacity) recent.RemoveRange(_capacity, recent.Count - _capacity);
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        File.WriteAllText(_path, JsonSerializer.Serialize(recent, new JsonSerializerOptions { WriteIndented = true }));
    }
}
