using System.Globalization;

namespace Meridian.Yaml.Cst;

/// <summary>
/// Renders a new scalar value as YAML source text. When a value is replaced surgically,
/// only its own span changes; this writer decides how to quote the replacement so that
/// the emitted text re-parses to the intended logical value (semantic identity, §6.2
/// guarantee (1)). It prefers the node's original style when the new value is expressible
/// in it, and otherwise escalates to a style that is safe.
/// </summary>
internal static class ScalarWriter
{
    /// <summary>
    /// Render <paramref name="value"/> for insertion in place of a scalar whose original
    /// style was <paramref name="originalStyle"/>.
    /// </summary>
    /// <remarks>
    /// Style preservation is the guiding rule (§6.2 "preserve … scalar styles"):
    /// <list type="bullet">
    /// <item>An originally <b>quoted</b> scalar was a deliberate string, so the replacement
    ///   stays quoted in the same style — this also guarantees the value re-parses as a
    ///   string even when it looks like a number/bool.</item>
    /// <item>An originally <b>plain</b> (or block) scalar stays plain when the new value is
    ///   syntactically expressible as a plain scalar (a number replacing a number, a word
    ///   replacing a word). Only if plain would break parsing does it escalate to quotes —
    ///   in which case the type-safety check also kicks in so, e.g., <c>true</c> replacing
    ///   plain text is quoted to remain a string.</item>
    /// </list>
    /// </remarks>
    public static string Render(string value, ScalarStyle originalStyle, string? originalValue = null)
    {
        switch (originalStyle)
        {
            case ScalarStyle.SingleQuoted when CanSingleQuote(value):
                return SingleQuote(value);
            case ScalarStyle.SingleQuoted:
            case ScalarStyle.DoubleQuoted:
                return DoubleQuote(value);
            default:
                // Plain / Literal / Folded originals: keep plain when syntactically safe.
                return RenderPlainPreferred(value, originalValue);
        }
    }

    /// <summary>
    /// Render preferring a plain scalar. A plain original that held a number/bool keeps its
    /// unquoted form (so numeric edits stay numeric). We quote when plain would break YAML
    /// syntax, and also when the new value would resolve to a <b>different</b> scalar type
    /// than the original plain value did — e.g. replacing a plain string with digits must be
    /// quoted so it stays a string (type stability).
    /// </summary>
    private static string RenderPlainPreferred(string value, string? originalValue)
    {
        bool typeWouldChange = originalValue is not null
            && ResolvedType(originalValue) != ResolvedType(value);

        if (!typeWouldChange && IsPlainSafe(value))
        {
            return value;
        }

        if (CanSingleQuote(value))
        {
            return SingleQuote(value);
        }

        return DoubleQuote(value);
    }

    /// <summary>The coarse YAML core-schema type a plain scalar resolves to.</summary>
    private enum ResolvedKind
    {
        Str,
        Int,
        Float,
        Bool,
        Null,
    }

    private static ResolvedKind ResolvedType(string value)
    {
        switch (value)
        {
            case "true" or "false" or "True" or "False"
                or "yes" or "no" or "on" or "off":
                return ResolvedKind.Bool;
            case "null" or "Null" or "~" or "":
                return ResolvedKind.Null;
        }

        if (long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _))
        {
            return ResolvedKind.Int;
        }

        if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out _))
        {
            return ResolvedKind.Float;
        }

        return ResolvedKind.Str;
    }

    /// <summary>
    /// True when <paramref name="value"/> is syntactically valid as a plain scalar and does
    /// not read as a *different* type than intended for a plain edit. For plain originals we
    /// allow number/bool-looking text (it's a legitimate plain scalar); we only reject text
    /// that YAML syntax cannot represent unquoted.
    /// </summary>
    private static bool IsPlainSafe(string value)
    {
        if (value.Length == 0)
        {
            return false; // empty must be quoted to stay a string
        }

        if (char.IsWhiteSpace(value[0]) || char.IsWhiteSpace(value[^1]))
        {
            return false; // leading/trailing whitespace is stripped by plain scalars
        }

        const string leadingIndicators = "-?:,[]{}#&*!|>'\"%@`";
        if (leadingIndicators.IndexOf(value[0]) >= 0)
        {
            return false;
        }

        foreach (var c in value)
        {
            if (c == '\n' || c == '\t' || c == '#' || c == ':' || c == '[' || c == ']' ||
                c == '{' || c == '}' || c == ',' || c == '&' || c == '*' || c == '!' ||
                c == '|' || c == '>' || c == '%' || c == '@' || c == '`' || c == '"' || c == '\'')
            {
                return false;
            }
        }

        return true;
    }

    private static bool CanSingleQuote(string value) => !value.Contains('\n');

    private static string SingleQuote(string value) => "'" + value.Replace("'", "''") + "'";

    private static string DoubleQuote(string value)
    {
        var sb = new System.Text.StringBuilder(value.Length + 2);
        sb.Append('"');
        foreach (var c in value)
        {
            switch (c)
            {
                case '\\': sb.Append("\\\\"); break;
                case '"': sb.Append("\\\""); break;
                case '\n': sb.Append("\\n"); break;
                case '\t': sb.Append("\\t"); break;
                case '\r': sb.Append("\\r"); break;
                default: sb.Append(c); break;
            }
        }

        sb.Append('"');
        return sb.ToString();
    }
}
