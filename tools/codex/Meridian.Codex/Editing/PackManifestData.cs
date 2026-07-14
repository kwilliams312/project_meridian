using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Meridian.Codex.Editing;

/// <summary>Editable values from a <c>meridian/pack@1</c> manifest.</summary>
public partial class PackManifestData : ObservableObject
{
    [ObservableProperty] private string _namespace = "my_pack";
    [ObservableProperty] private string _name = "My Pack";
    [ObservableProperty] private string _description = string.Empty;
    [ObservableProperty] private string _version = "0.1.0";
    [ObservableProperty] private string _contentSchemaVersion = "1";
    [ObservableProperty] private string _compatibilityVersion = "1";
    [ObservableProperty] private string _godotVersion = "4.6";
    [ObservableProperty] private string _license = "Apache-2.0";

    public ObservableCollection<PackDependencyData> Dependencies { get; } = [];

    public PackManifestData Copy()
    {
        var copy = new PackManifestData
        {
            Namespace = Namespace,
            Name = Name,
            Description = Description,
            Version = Version,
            ContentSchemaVersion = ContentSchemaVersion,
            CompatibilityVersion = CompatibilityVersion,
            GodotVersion = GodotVersion,
            License = License,
        };
        foreach (var dependency in Dependencies)
        {
            copy.Dependencies.Add(dependency with { });
        }
        return copy;
    }

    public bool ValueEquals(PackManifestData other) =>
        Namespace == other.Namespace && Name == other.Name && Description == other.Description &&
        Version == other.Version && ContentSchemaVersion == other.ContentSchemaVersion &&
        CompatibilityVersion == other.CompatibilityVersion && GodotVersion == other.GodotVersion &&
        License == other.License && Dependencies.SequenceEqual(other.Dependencies);
}

public sealed record PackDependencyData(string Namespace, string Version);
