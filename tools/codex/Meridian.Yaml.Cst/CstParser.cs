using YamlDotNet.Core;
using YamlDotNet.Core.Tokens;

namespace Meridian.Yaml.Cst;

/// <summary>
/// Builds a <see cref="CstNode"/> tree from YamlDotNet's low-level token stream
/// (tools-sad.md §6.2). Every node is anchored to the source text by character span,
/// which is what lets the document re-emit untouched regions byte-for-byte and apply
/// value edits surgically.
/// </summary>
/// <remarks>
/// The parser is a small recursive-descent consumer over the <see cref="Scanner"/>
/// token stream. It handles the constructs that appear in Meridian content YAML:
/// block/flow mappings, block/flow sequences, and plain/quoted/block scalars. It does
/// not attempt to model anchors/aliases or tags — <c>mcc fmt</c> bans anchors (§6.2),
/// and any document the CST cannot faithfully represent surfaces via the read-only
/// escape hatch at a higher layer.
/// </remarks>
internal sealed class CstParser
{
    private readonly string _source;
    private readonly List<Token> _tokens;
    private int _pos;

    private CstParser(string source, List<Token> tokens)
    {
        _source = source;
        _tokens = tokens;
    }

    /// <summary>Parse <paramref name="source"/> into a root CST node.</summary>
    public static CstNode Parse(string source)
    {
        var tokens = ScanTokens(source);
        var parser = new CstParser(source, tokens);
        return parser.ParseDocument();
    }

    /// <summary>
    /// Scan the source into a materialized token list, keeping comment tokens
    /// (<c>skipComments: false</c>) so trivia positions are known. Comments are not
    /// nodes in the tree — they live in the untouched byte regions between node spans
    /// and are preserved implicitly by the verbatim splice model.
    /// </summary>
    private static List<Token> ScanTokens(string source)
    {
        var scanner = new Scanner(new StringReader(source), skipComments: false);
        var tokens = new List<Token>();
        while (scanner.MoveNext())
        {
            tokens.Add(scanner.Current!);
        }

        return tokens;
    }

    private Token Current => _tokens[_pos];

    private bool Is<T>() where T : Token => _pos < _tokens.Count && _tokens[_pos] is T;

    private T Expect<T>() where T : Token
    {
        if (_tokens[_pos] is not T t)
        {
            throw new YamlCstException(
                $"CST parse expected {typeof(T).Name} but found {_tokens[_pos].GetType().Name} " +
                $"at line {_tokens[_pos].Start.Line}, col {_tokens[_pos].Start.Column}.");
        }

        _pos++;
        return t;
    }

    /// <summary>Advance past any comment tokens (they are trivia, preserved verbatim).</summary>
    private void SkipComments()
    {
        while (_pos < _tokens.Count && _tokens[_pos] is Comment)
        {
            _pos++;
        }
    }

    private CstNode ParseDocument()
    {
        Expect<StreamStart>();
        SkipComments();

        // Optional explicit document markers.
        if (Is<DocumentStart>())
        {
            _pos++;
        }

        SkipComments();
        var root = ParseNode();

        return root;
    }

    /// <summary>Parse whatever node begins at the current token.</summary>
    private CstNode ParseNode()
    {
        SkipComments();
        var t = Current;
        return t switch
        {
            BlockMappingStart => ParseBlockMapping(),
            FlowMappingStart => ParseFlowMapping(),
            BlockSequenceStart => ParseBlockSequence(),
            FlowSequenceStart => ParseFlowSequence(),
            // A block sequence whose entries sit at the *same* indent as their parent key
            // is emitted by YamlDotNet without a BlockSequenceStart/BlockEnd pair — the
            // value is followed directly by BlockEntry tokens. Handle that "compact" form.
            BlockEntry => ParseCompactBlockSequence(),
            Scalar => ParseScalar(),
            _ => throw new YamlCstException(
                $"CST parse: unexpected token {t.GetType().Name} at line {t.Start.Line}, col {t.Start.Column}."),
        };
    }

    private CstNode ParseScalar()
    {
        var s = Expect<Scalar>();
        var span = new Span((int)s.Start.Index, (int)s.End.Index);
        return new CstNode(CstKind.Scalar, span)
        {
            ScalarValue = s.Value,
            Style = MapStyle(s.Style),
        };
    }

    private CstNode ParseBlockMapping()
    {
        var start = Expect<BlockMappingStart>();
        var node = new CstNode(CstKind.Mapping, new Span((int)start.Start.Index, (int)start.End.Index))
        {
            IsFlow = false,
        };

        int lastEnd = (int)start.End.Index;
        while (true)
        {
            int? triviaStart = Is<Comment>() ? (int)Current.Start.Index : null;
            SkipComments();
            if (Is<BlockEnd>())
            {
                var end = Expect<BlockEnd>();
                lastEnd = node.Entries.Count == 0
                    ? (int)end.Start.Index
                    : EndBeforeTrivia(lastEnd, triviaStart ?? (int)end.Start.Index);
                break;
            }

            Expect<Key>();
            SkipComments();
            var keyScalar = Expect<Scalar>();
            var keySpan = new Span((int)keyScalar.Start.Index, (int)keyScalar.End.Index);

            SkipComments();
            Expect<Value>();
            SkipComments();
            var value = ParseNode();

            var entry = new MappingEntry(keyScalar.Value, keySpan, value);
            node.AddEntry(entry);
            lastEnd = value.Span.End;
        }

        node.Span = new Span(node.Span.Start, lastEnd);
        ComputeBlockEntrySpans(node);
        return node;
    }

    private CstNode ParseFlowMapping()
    {
        var start = Expect<FlowMappingStart>();
        var node = new CstNode(CstKind.Mapping, new Span((int)start.Start.Index, (int)start.End.Index))
        {
            IsFlow = true,
        };

        int end = (int)start.End.Index;
        while (true)
        {
            SkipComments();
            if (Is<FlowMappingEnd>())
            {
                var e = Expect<FlowMappingEnd>();
                end = (int)e.End.Index;
                break;
            }

            if (Is<FlowEntry>())
            {
                _pos++;
                continue;
            }

            Expect<Key>();
            SkipComments();
            var keyScalar = Expect<Scalar>();
            var keySpan = new Span((int)keyScalar.Start.Index, (int)keyScalar.End.Index);

            SkipComments();
            Expect<Value>();
            SkipComments();
            var value = ParseNode();

            var entry = new MappingEntry(keyScalar.Value, keySpan, value)
            {
                EntrySpan = new Span(keySpan.Start, value.Span.End),
            };
            node.AddEntry(entry);
        }

        node.Span = new Span(node.Span.Start, end);
        return node;
    }

    private CstNode ParseBlockSequence()
    {
        var start = Expect<BlockSequenceStart>();
        var node = new CstNode(CstKind.Sequence, new Span((int)start.Start.Index, (int)start.End.Index))
        {
            IsFlow = false,
        };

        int lastEnd = (int)start.End.Index;
        while (true)
        {
            int? triviaStart = Is<Comment>() ? (int)Current.Start.Index : null;
            SkipComments();
            if (Is<BlockEnd>())
            {
                var end = Expect<BlockEnd>();
                lastEnd = node.Items.Count == 0
                    ? (int)end.Start.Index
                    : EndBeforeTrivia(lastEnd, triviaStart ?? (int)end.Start.Index);
                break;
            }

            Expect<BlockEntry>();
            SkipComments();
            var item = ParseNode();
            node.AddItem(item);
            lastEnd = item.Span.End;
        }

        node.Span = new Span(node.Span.Start, lastEnd);
        return node;
    }

    /// <summary>
    /// Parse a compact block sequence: entries at the same indentation as the parent key,
    /// which YamlDotNet emits as a run of <c>BlockEntry</c> + node with no enclosing
    /// <c>BlockSequenceStart</c>/<c>BlockEnd</c>. The run ends at the first token that is not
    /// another <c>BlockEntry</c> (a sibling <c>Key</c>, a <c>BlockEnd</c> closing the parent
    /// mapping, or a comment).
    /// </summary>
    private CstNode ParseCompactBlockSequence()
    {
        var first = Current; // BlockEntry
        var node = new CstNode(CstKind.Sequence, new Span((int)first.Start.Index, (int)first.Start.Index))
        {
            IsFlow = false,
        };

        int lastEnd = (int)first.Start.Index;
        while (Is<BlockEntry>())
        {
            Expect<BlockEntry>();
            SkipComments();
            var item = ParseNode();
            node.AddItem(item);
            lastEnd = item.Span.End;
            SkipComments();
        }

        node.Span = new Span(node.Span.Start, lastEnd);
        return node;
    }

    private CstNode ParseFlowSequence()
    {
        var start = Expect<FlowSequenceStart>();
        var node = new CstNode(CstKind.Sequence, new Span((int)start.Start.Index, (int)start.End.Index))
        {
            IsFlow = true,
        };

        int end = (int)start.End.Index;
        while (true)
        {
            SkipComments();
            if (Is<FlowSequenceEnd>())
            {
                var e = Expect<FlowSequenceEnd>();
                end = (int)e.End.Index;
                break;
            }

            if (Is<FlowEntry>())
            {
                _pos++;
                continue;
            }

            var item = ParseNode();
            node.AddItem(item);
        }

        node.Span = new Span(node.Span.Start, end);
        return node;
    }

    /// <summary>
    /// Compute per-entry spans for a block mapping that cover the whole physical line(s)
    /// of each entry, from the start of the key's line to the start of the next entry's
    /// line (or the end of the mapping). Line-based (rather than token-adjacent) so that
    /// trailing comments and blank lines between entries fall on the boundary predictably.
    /// This is the range a surgical key removal deletes.
    /// </summary>
    private void ComputeBlockEntrySpans(CstNode mapping)
    {
        var entries = mapping.Entries;
        for (int i = 0; i < entries.Count; i++)
        {
            int lineStart = LineStartOf(entries[i].KeySpan.Start);
            int entryEnd = i + 1 < entries.Count
                ? LineStartOf(entries[i + 1].KeySpan.Start)
                : mapping.Span.End;
            entries[i].EntrySpan = new Span(lineStart, entryEnd);
        }
    }

    /// <summary>Return the index of the first character on the line containing <paramref name="index"/>.</summary>
    private int LineStartOf(int index)
    {
        int i = index;
        while (i > 0 && _source[i - 1] != '\n')
        {
            i--;
        }

        return i;
    }

    private int EndBeforeTrivia(int fallback, int limit)
    {
        int comment = _source.IndexOf('#', fallback, Math.Max(0, limit - fallback));
        int end = comment >= 0 ? comment : limit;
        while (end > fallback && char.IsWhiteSpace(_source[end - 1])) end--;
        return end;
    }

    private static ScalarStyle MapStyle(YamlDotNet.Core.ScalarStyle style) => style switch
    {
        YamlDotNet.Core.ScalarStyle.SingleQuoted => ScalarStyle.SingleQuoted,
        YamlDotNet.Core.ScalarStyle.DoubleQuoted => ScalarStyle.DoubleQuoted,
        YamlDotNet.Core.ScalarStyle.Literal => ScalarStyle.Literal,
        YamlDotNet.Core.ScalarStyle.Folded => ScalarStyle.Folded,
        _ => ScalarStyle.Plain,
    };
}
