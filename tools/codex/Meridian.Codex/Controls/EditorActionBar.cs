using System.Windows.Input;
using Avalonia;
using Avalonia.Controls;

namespace Meridian.Codex.Controls;

/// <summary>
/// Reusable sticky editor footer for a primary commit action and its state
/// explanation. Editors supply their own command, so button and accelerator
/// execution share one CanExecute source of truth.
/// </summary>
public sealed partial class EditorActionBar : UserControl
{
    public static readonly StyledProperty<string> ActionTextProperty =
        AvaloniaProperty.Register<EditorActionBar, string>(nameof(ActionText), "Save");

    public static readonly StyledProperty<ICommand?> ActionCommandProperty =
        AvaloniaProperty.Register<EditorActionBar, ICommand?>(nameof(ActionCommand));

    public static readonly StyledProperty<string> StateDescriptionProperty =
        AvaloniaProperty.Register<EditorActionBar, string>(nameof(StateDescription), string.Empty);

    public static readonly StyledProperty<bool> IsActionEnabledProperty =
        AvaloniaProperty.Register<EditorActionBar, bool>(nameof(IsActionEnabled));

    public string ActionText
    {
        get => GetValue(ActionTextProperty);
        set => SetValue(ActionTextProperty, value);
    }

    public ICommand? ActionCommand
    {
        get => GetValue(ActionCommandProperty);
        set => SetValue(ActionCommandProperty, value);
    }

    public string StateDescription
    {
        get => GetValue(StateDescriptionProperty);
        set => SetValue(StateDescriptionProperty, value);
    }

    public bool IsActionEnabled
    {
        get => GetValue(IsActionEnabledProperty);
        set => SetValue(IsActionEnabledProperty, value);
    }

    public EditorActionBar() => InitializeComponent();
}
