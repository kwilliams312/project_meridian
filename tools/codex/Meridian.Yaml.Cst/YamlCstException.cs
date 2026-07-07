namespace Meridian.Yaml.Cst;

/// <summary>
/// Raised when the CST layer cannot faithfully parse or edit a document. The Codex
/// editor treats this as the signal to fall back to the read-only "reformat with
/// mcc fmt to edit" path (tools-sad.md §6.2) rather than risk silent data loss.
/// </summary>
public sealed class YamlCstException : Exception
{
    public YamlCstException(string message) : base(message)
    {
    }

    public YamlCstException(string message, Exception inner) : base(message, inner)
    {
    }
}
