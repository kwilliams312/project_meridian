using System.Text;

namespace Meridian.Yaml.Cst;

/// <summary>One segment of an access path: either a mapping key or a sequence index.</summary>
internal readonly struct PathSegment
{
    private PathSegment(string? key, int index, bool isIndex)
    {
        Key = key;
        Index = index;
        IsIndex = isIndex;
    }

    public string? Key { get; }
    public int Index { get; }
    public bool IsIndex { get; }

    public static PathSegment ForKey(string key) => new(key, 0, false);
    public static PathSegment ForIndex(int index) => new(null, index, true);
}

/// <summary>
/// Parses the access-path grammar used by <see cref="YamlDocument"/>: mapping keys
/// separated by <c>.</c>, sequence indices as <c>[n]</c>. Examples:
/// <c>stats.damage.min</c>, <c>ai.abilities[0].priority</c>, <c>list[2]</c>.
/// </summary>
internal static class PathParser
{
    public static List<PathSegment> Parse(string path)
    {
        var segments = new List<PathSegment>();
        int i = 0;
        var key = new StringBuilder();

        void FlushKey()
        {
            if (key.Length > 0)
            {
                segments.Add(PathSegment.ForKey(key.ToString()));
                key.Clear();
            }
        }

        while (i < path.Length)
        {
            char c = path[i];
            if (c == '.')
            {
                FlushKey();
                i++;
            }
            else if (c == '[')
            {
                FlushKey();
                int close = path.IndexOf(']', i);
                if (close < 0)
                {
                    throw new YamlCstException($"Malformed path '{path}': unclosed '['.");
                }

                var num = path.Substring(i + 1, close - i - 1);
                if (!int.TryParse(num, out var idx))
                {
                    throw new YamlCstException($"Malformed path '{path}': '[{num}]' is not an index.");
                }

                segments.Add(PathSegment.ForIndex(idx));
                i = close + 1;
            }
            else
            {
                key.Append(c);
                i++;
            }
        }

        FlushKey();
        return segments;
    }

    public static string Render(IEnumerable<PathSegment> segments)
    {
        var sb = new StringBuilder();
        foreach (var seg in segments)
        {
            if (seg.IsIndex)
            {
                sb.Append('[').Append(seg.Index).Append(']');
            }
            else
            {
                if (sb.Length > 0)
                {
                    sb.Append('.');
                }

                sb.Append(seg.Key);
            }
        }

        return sb.ToString();
    }
}
