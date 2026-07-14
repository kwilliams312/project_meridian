using System.Text;

namespace Meridian.Yaml.Cst;

/// <summary>
/// A CST-preserving YAML document (tools-sad.md §6.2). Loads a YAML file into a concrete
/// syntax tree anchored to the original bytes, exposes surgical edits addressed by path,
/// and serializes so that every untouched region is emitted byte-for-byte — only edited
/// spans change.
/// </summary>
/// <remarks>
/// The edit model is a set of non-overlapping text splices against the original source.
/// A splice is a (span, replacement) pair. Serialization walks the original text, copying
/// verbatim outside spliced spans and substituting the replacement inside them. With zero
/// splices, output is byte-identical to input (guarantee (2): byte identity for untouched
/// regions — here the whole document). Each edit adds exactly one minimal splice, so all
/// comments, key order, quoting, blank lines and unrelated values survive unchanged.
/// </remarks>
public sealed class YamlDocument
{
    private readonly string _source;
    private readonly CstNode _root;
    private readonly List<Splice> _splices = new();
    private readonly HashSet<(CstNode Mapping, string Key)> _addedKeys = new();
    private int _nextSpliceOrder;

    private YamlDocument(string source, CstNode root)
    {
        _source = source;
        _root = root;
    }

    /// <summary>The root CST node (typically a mapping for content files).</summary>
    public CstNode Root => _root;

    /// <summary>Parse <paramref name="source"/> into a CST-preserving document.</summary>
    /// <exception cref="YamlCstException">The document cannot be faithfully represented.</exception>
    public static YamlDocument Parse(string source)
    {
        try
        {
            var root = CstParser.Parse(source);
            return new YamlDocument(source, root);
        }
        catch (YamlCstException)
        {
            throw;
        }
        catch (Exception ex)
        {
            throw new YamlCstException($"CST parse failed: {ex.Message}", ex);
        }
    }

    /// <summary>Load a document from a file (UTF-8, no BOM added).</summary>
    public static YamlDocument Load(string path)
    {
        var text = File.ReadAllText(path, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        return Parse(text);
    }

    /// <summary>
    /// The original source text, unmodified. Distinct from <see cref="ToText"/>, which
    /// applies pending edits.
    /// </summary>
    public string OriginalText => _source;

    /// <summary>
    /// Render a logical string as a YAML scalar whose tag cannot be implicitly
    /// resolved as a number, boolean, or null. Used by new-document emitters
    /// that do not have an original scalar style to preserve.
    /// </summary>
    public static string RenderString(string value) =>
        ScalarWriter.Render(value, ScalarStyle.DoubleQuoted);

    /// <summary>Serialize the document with all pending edits applied.</summary>
    public string ToText()
    {
        if (_splices.Count == 0)
        {
            return _source; // guarantee (2): untouched document is byte-identical.
        }

        var ordered = _splices
            .OrderBy(s => s.Span.Start)
            .ThenBy(s => s.Span.Length == 0 ? 0 : 1)
            // A final descendant mapping can share its insertion offset with
            // every ancestor up to the root. Emit deepest mappings first so
            // their indentation remains inside the owning mapping.
            .ThenByDescending(s => s.MappingDepth)
            .ThenByDescending(s => s.MappingSpan?.Start ?? -1)
            // Multiple additions to one mapping preserve API call order.
            .ThenBy(s => s.Order)
            .ToList();
        var sb = new StringBuilder(_source.Length + 16);
        int cursor = 0;
        foreach (var splice in ordered)
        {
            if (splice.Span.Start < cursor)
            {
                throw new YamlCstException("Overlapping edits detected; splices must be disjoint.");
            }

            sb.Append(_source, cursor, splice.Span.Start - cursor);
            sb.Append(splice.Replacement);
            cursor = splice.Span.End;
        }

        sb.Append(_source, cursor, _source.Length - cursor);
        return sb.ToString();
    }

    /// <summary>Save the document (with pending edits) to <paramref name="path"/> as UTF-8, no BOM.</summary>
    public void Save(string path)
    {
        File.WriteAllText(path, ToText(), new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    // ---- navigation --------------------------------------------------------

    /// <summary>Resolve a dotted/indexed path to a node, or null if it does not exist.</summary>
    /// <remarks>
    /// Path grammar: mapping keys separated by <c>.</c>, sequence indices as <c>[n]</c>.
    /// Example: <c>stats.damage.min</c>, <c>ai.abilities[0].priority</c>.
    /// </remarks>
    public CstNode? Resolve(string path) => ResolveInternal(path, out _, out _);

    /// <summary>Get the logical scalar value at <paramref name="path"/>, or null if absent/not a scalar.</summary>
    public string? GetValue(string path)
    {
        var node = Resolve(path);
        return node is { Kind: CstKind.Scalar } ? node.ScalarValue : null;
    }

    // ---- edits -------------------------------------------------------------

    /// <summary>
    /// Surgically set the scalar value at <paramref name="path"/>. Only the value's own
    /// byte span changes; the key, its formatting, and everything else stay byte-identical.
    /// </summary>
    /// <exception cref="YamlCstException">The path is absent or does not point at a scalar.</exception>
    public void SetValue(string path, string value)
    {
        var node = Resolve(path)
            ?? throw new YamlCstException($"SetValue: path '{path}' does not exist.");
        if (node.Kind != CstKind.Scalar)
        {
            throw new YamlCstException($"SetValue: path '{path}' is not a scalar.");
        }

        var rendered = ScalarWriter.Render(value, node.Style, node.ScalarValue);
        AddSplice(new Splice(node.Span, rendered));
    }

    /// <summary>
    /// Surgically remove the mapping key at <paramref name="path"/>. The whole physical
    /// entry line (indentation through trailing newline) is deleted; the surrounding
    /// layout — comments, blank lines, sibling keys — is preserved.
    /// </summary>
    /// <exception cref="YamlCstException">The path is absent, or its parent is not a block mapping.</exception>
    public void RemoveKey(string path)
    {
        var (parent, lastSegment, _) = SplitPath(path);
        var parentNode = parent is null ? _root : Resolve(parent);
        if (parentNode is not { Kind: CstKind.Mapping } || parentNode.IsFlow)
        {
            throw new YamlCstException($"RemoveKey: parent of '{path}' is not a block mapping.");
        }

        var entry = parentNode.FindEntry(lastSegment)
            ?? throw new YamlCstException($"RemoveKey: key '{path}' does not exist.");
        AddSplice(new Splice(entry.EntrySpan, string.Empty));
    }

    /// <summary>
    /// Surgically add a new <paramref name="key"/>: <paramref name="value"/> pair to the
    /// block mapping at <paramref name="parentPath"/> (or the root when null/empty).
    /// The new line is inserted after the mapping's last entry, matching its indentation;
    /// all existing entries and their formatting are preserved byte-for-byte.
    /// </summary>
    /// <exception cref="YamlCstException">The parent is absent, not a block mapping, or the key already exists.</exception>
    public void AddKey(string? parentPath, string key, string value)
    {
        var parentNode = string.IsNullOrEmpty(parentPath) ? _root : Resolve(parentPath);
        if (parentNode is not { Kind: CstKind.Mapping } || parentNode.IsFlow)
        {
            throw new YamlCstException($"AddKey: parent '{parentPath}' is not a block mapping.");
        }

        if (parentNode.FindEntry(key) is not null)
        {
            throw new YamlCstException($"AddKey: key '{key}' already exists under '{parentPath}'.");
        }

        if (parentNode.Entries.Count == 0)
        {
            throw new YamlCstException(
                "AddKey: adding to an empty block mapping is not supported by surgical edit; " +
                "reformat with mcc fmt to edit.");
        }
        if (!_addedKeys.Add((parentNode, key)))
        {
            throw new YamlCstException($"AddKey: key '{key}' already exists under '{parentPath}'.");
        }

        var lastEntry = parentNode.Entries[^1];
        int indent = ColumnOf(lastEntry.KeySpan.Start);
        int insertAt = lastEntry.EntrySpan.End;

        // The last entry's span ends at the start of the next line (or mapping end). Match
        // its indentation and terminate the new line with a newline so following content
        // (or EOF) is untouched.
        string pad = new string(' ', indent);
        string rendered = ScalarWriter.Render(value, ScalarStyle.Plain);
        string insertion = InsertionNeedsLeadingNewline(insertAt)
            ? $"\n{pad}{key}: {rendered}"
            : $"{pad}{key}: {rendered}\n";

        AddSplice(MappingInsertion(parentPath, parentNode, insertAt, insertion));
    }

    /// <summary>Replace one complete value node with caller-rendered YAML.</summary>
    /// <remarks>
    /// This is used for collection-valued form fields where a scalar splice is not
    /// sufficient. The replacement is indentation-adjusted to the original node and
    /// every byte outside that node remains verbatim.
    /// </remarks>
    public void ReplaceNode(string path, string rawYaml)
    {
        var node = Resolve(path) ?? throw new YamlCstException($"ReplaceNode: path '{path}' does not exist.");
        int indent = ColumnOf(node.Span.Start);
        string replacement = IndentContinuationLines(rawYaml, indent);
        AddSplice(new Splice(node.Span, replacement));
    }

    /// <summary>Add a mapping key whose value is already-rendered YAML.</summary>
    public void AddRawKey(string? parentPath, string key, string rawYaml)
    {
        var parentNode = string.IsNullOrEmpty(parentPath) ? _root : Resolve(parentPath);
        if (parentNode is not { Kind: CstKind.Mapping } || parentNode.IsFlow || parentNode.Entries.Count == 0)
        {
            throw new YamlCstException($"AddRawKey: parent '{parentPath}' is not a non-empty block mapping.");
        }
        if (parentNode.FindEntry(key) is not null || !_addedKeys.Add((parentNode, key)))
        {
            throw new YamlCstException($"AddRawKey: key '{key}' already exists under '{parentPath}'.");
        }

        var lastEntry = parentNode.Entries[^1];
        int indent = ColumnOf(lastEntry.KeySpan.Start);
        int insertAt = lastEntry.EntrySpan.End;
        string pad = new(' ', indent);
        string value = IndentContinuationLines(rawYaml, indent + 2);
        string rendered = rawYaml.Contains('\n')
            ? $"{pad}{key}:\n{new string(' ', indent + 2)}{value}"
            : $"{pad}{key}: {value}";
        string insertion = InsertionNeedsLeadingNewline(insertAt) ? $"\n{rendered}" : $"{rendered}\n";
        AddSplice(MappingInsertion(parentPath, parentNode, insertAt, insertion));
    }

    private static string IndentContinuationLines(string text, int indent) =>
        text.Replace("\n", "\n" + new string(' ', indent), StringComparison.Ordinal);

    // ---- internals ---------------------------------------------------------

    private void AddSplice(Splice splice)
    {
        splice = splice with { Order = _nextSpliceOrder++ };
        // Guard against two edits touching the same region within one save.
        foreach (var existing in _splices)
        {
            bool disjoint = splice.Span.End <= existing.Span.Start || splice.Span.Start >= existing.Span.End;
            bool bothInsertAtSamePoint = splice.Span.Length == 0 && existing.Span.Length == 0
                && splice.Span.Start == existing.Span.Start;
            if (bothInsertAtSamePoint)
            {
                if (splice.MappingPath is not null && existing.MappingPath is not null)
                {
                    continue;
                }
                throw new YamlCstException("Conflicting edit: two edits target the same insertion point.");
            }
            if (!disjoint)
            {
                throw new YamlCstException("Conflicting edit: two edits target overlapping spans.");
            }
        }

        _splices.Add(splice);
    }

    private static Splice MappingInsertion(string? parentPath, CstNode mapping, int insertAt, string insertion)
    {
        List<PathSegment> segments = string.IsNullOrEmpty(parentPath) ? [] : PathParser.Parse(parentPath);
        var canonicalPath = segments.Count == 0 ? "$" : PathParser.Render(segments);
        return new(new Span(insertAt, insertAt), insertion, canonicalPath, mapping.Span, segments.Count);
    }

    private CstNode? ResolveInternal(string path, out CstNode? parent, out string? lastSegment)
    {
        parent = null;
        lastSegment = null;
        if (string.IsNullOrEmpty(path))
        {
            return _root;
        }

        var segments = PathParser.Parse(path);
        CstNode current = _root;
        CstNode? prev = null;

        foreach (var seg in segments)
        {
            prev = current;
            if (seg.IsIndex)
            {
                if (current.Kind != CstKind.Sequence || seg.Index < 0 || seg.Index >= current.Items.Count)
                {
                    return null;
                }

                current = current.Items[seg.Index];
            }
            else
            {
                if (current.Kind != CstKind.Mapping)
                {
                    return null;
                }

                var entry = current.FindEntry(seg.Key!);
                if (entry is null)
                {
                    return null;
                }

                lastSegment = seg.Key;
                current = entry.Value;
            }
        }

        parent = prev;
        return current;
    }

    private (string? parentPath, string lastSegment, bool lastIsIndex) SplitPath(string path)
    {
        var segments = PathParser.Parse(path);
        if (segments.Count == 0)
        {
            throw new YamlCstException("Empty path.");
        }

        var last = segments[^1];
        string? parentPath = segments.Count == 1 ? null : PathParser.Render(segments.Take(segments.Count - 1));
        return (parentPath, last.Key ?? last.Index.ToString(), last.IsIndex);
    }

    private int ColumnOf(int index)
    {
        int col = 0;
        int i = index;
        while (i > 0 && _source[i - 1] != '\n')
        {
            i--;
            col++;
        }

        return col;
    }

    private bool InsertionNeedsLeadingNewline(int insertAt)
    {
        // If the character just before the insertion point is not a newline (e.g. the file
        // ends without a trailing newline), prepend one so the new entry starts its own line.
        return insertAt > 0 && _source[insertAt - 1] != '\n';
    }

    private readonly record struct Splice(
        Span Span,
        string Replacement,
        string? MappingPath = null,
        Span? MappingSpan = null,
        int MappingDepth = -1,
        int Order = 0);
}
