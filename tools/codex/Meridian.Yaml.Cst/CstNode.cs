namespace Meridian.Yaml.Cst;

/// <summary>
/// A half-open character span <c>[Start, End)</c> into the original document text.
/// All CST nodes anchor themselves to the source by span so that untouched regions
/// can be re-emitted byte-for-byte (tools-sad.md §6.2 "replays untouched tokens verbatim").
/// </summary>
/// <param name="Start">Inclusive start index into the source string.</param>
/// <param name="End">Exclusive end index into the source string.</param>
public readonly record struct Span(int Start, int End)
{
    /// <summary>Length of the span in characters.</summary>
    public int Length => End - Start;

    /// <summary>Extract this span's slice from <paramref name="source"/>.</summary>
    public string Slice(string source) => source.Substring(Start, Length);

    public override string ToString() => $"[{Start}..{End})";
}

/// <summary>The concrete kind of a CST node.</summary>
public enum CstKind
{
    /// <summary>A scalar leaf value (plain, quoted, or block scalar).</summary>
    Scalar,

    /// <summary>A block or flow mapping (key/value pairs).</summary>
    Mapping,

    /// <summary>A block or flow sequence (ordered items).</summary>
    Sequence,
}

/// <summary>
/// The scalar presentation style, mirrored from the source so an edit can preserve
/// the human's chosen quoting when the new value is compatible with it.
/// </summary>
public enum ScalarStyle
{
    Plain,
    SingleQuoted,
    DoubleQuoted,
    Literal,
    Folded,
}

/// <summary>
/// A node in the concrete syntax tree. Unlike a logical YAML value, a <see cref="CstNode"/>
/// remembers exactly where it lives in the original text, so edits can be surgical.
/// </summary>
public sealed class CstNode
{
    internal CstNode(CstKind kind, Span span)
    {
        Kind = kind;
        Span = span;
    }

    /// <summary>What kind of node this is.</summary>
    public CstKind Kind { get; }

    /// <summary>
    /// The full source span of this node's value (for a scalar this includes any quote
    /// characters or block-scalar indicators; for a collection it spans the whole
    /// collection). This is the span a surgical replace of the whole node would target.
    /// </summary>
    public Span Span { get; internal set; }

    // ---- scalar ------------------------------------------------------------

    /// <summary>For <see cref="CstKind.Scalar"/>: the logical (unquoted, resolved) value.</summary>
    public string? ScalarValue { get; internal set; }

    /// <summary>For <see cref="CstKind.Scalar"/>: the presentation style in the source.</summary>
    public ScalarStyle Style { get; internal set; }

    // ---- mapping -----------------------------------------------------------

    /// <summary>For <see cref="CstKind.Mapping"/>: the ordered key/value entries.</summary>
    public IReadOnlyList<MappingEntry> Entries => _entries;
    private readonly List<MappingEntry> _entries = new();

    /// <summary>True when this mapping/sequence uses flow style (<c>{ }</c> / <c>[ ]</c>).</summary>
    public bool IsFlow { get; internal set; }

    // ---- sequence ----------------------------------------------------------

    /// <summary>For <see cref="CstKind.Sequence"/>: the ordered item nodes.</summary>
    public IReadOnlyList<CstNode> Items => _items;
    private readonly List<CstNode> _items = new();

    internal void AddEntry(MappingEntry entry) => _entries.Add(entry);
    internal void AddItem(CstNode item) => _items.Add(item);

    /// <summary>Look up a mapping entry by key, or null if absent (or not a mapping).</summary>
    public MappingEntry? FindEntry(string key)
    {
        if (Kind != CstKind.Mapping)
        {
            return null;
        }

        foreach (var e in _entries)
        {
            if (e.KeyText == key)
            {
                return e;
            }
        }

        return null;
    }
}

/// <summary>
/// One key/value pair inside a mapping. Records the key's own span and text so that
/// key-order and key formatting are preserved, and add/remove-key edits can find the
/// exact byte range for the whole entry.
/// </summary>
public sealed class MappingEntry
{
    internal MappingEntry(string keyText, Span keySpan, CstNode value)
    {
        KeyText = keyText;
        KeySpan = keySpan;
        Value = value;
    }

    /// <summary>The resolved key text (used for path navigation).</summary>
    public string KeyText { get; }

    /// <summary>Source span of the key scalar.</summary>
    public Span KeySpan { get; }

    /// <summary>The value node for this entry.</summary>
    public CstNode Value { get; }

    /// <summary>
    /// Source span covering the whole entry line(s) including leading indentation and
    /// trailing newline, used for surgical key removal. Set by the builder.
    /// </summary>
    public Span EntrySpan { get; internal set; }
}
