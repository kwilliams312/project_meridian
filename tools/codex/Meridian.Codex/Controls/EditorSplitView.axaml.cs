using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.VisualTree;

namespace Meridian.Codex.Controls;

/// <summary>
/// Shared responsive editor canvas with a user-resizable, collapsible YAML
/// preview. At laptop widths the preview yields space to the form automatically;
/// the author can always reopen it explicitly.
/// </summary>
public sealed partial class EditorSplitView : UserControl
{
    private const double DefaultPreviewWidth = 340;
    private const double ResponsiveCollapseWidth = 800;
    private bool _autoCollapsed;
    private bool _userOverride;
    private Control? _layoutHost;

    public static readonly StyledProperty<Control?> FormContentProperty =
        AvaloniaProperty.Register<EditorSplitView, Control?>(nameof(FormContent));

    public static readonly StyledProperty<string> PreviewTextProperty =
        AvaloniaProperty.Register<EditorSplitView, string>(nameof(PreviewText), string.Empty);

    public Control? FormContent
    {
        get => GetValue(FormContentProperty);
        set => SetValue(FormContentProperty, value);
    }

    public string PreviewText
    {
        get => GetValue(PreviewTextProperty);
        set => SetValue(PreviewTextProperty, value);
    }

    public bool IsPreviewOpen => PreviewPane.IsVisible;
    private ColumnDefinition SplitterColumn => LayoutGrid.ColumnDefinitions[1];
    private ColumnDefinition PreviewColumn => LayoutGrid.ColumnDefinitions[2];
    internal double PreviewWidth => PreviewColumn.Width.Value;

    public EditorSplitView()
    {
        InitializeComponent();
        SizeChanged += OnSizeChanged;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        UpdateResponsiveState(availableSize.Width);
        return base.MeasureOverride(availableSize);
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        _layoutHost = Parent as Control;
        if (_layoutHost is not null)
        {
            _layoutHost.SizeChanged += OnLayoutHostSizeChanged;
            UpdateResponsiveState(_layoutHost.Bounds.Width);
        }
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        if (_layoutHost is not null) _layoutHost.SizeChanged -= OnLayoutHostSizeChanged;
        _layoutHost = null;
        base.OnDetachedFromVisualTree(e);
    }

    public void SetPreviewOpen(bool isOpen)
    {
        PreviewPane.IsVisible = isOpen;
        PreviewSplitter.IsVisible = isOpen;
        ShowPreviewButton.IsVisible = !isOpen;
        SplitterColumn.Width = new GridLength(isOpen ? 5 : 0);
        PreviewColumn.Width = new GridLength(isOpen ? DefaultPreviewWidth : 44);
        PreviewColumn.MaxWidth = isOpen ? 560 : 44;
    }

    private void OnSizeChanged(object? sender, SizeChangedEventArgs e)
        => UpdateResponsiveState(ConstrainToLayoutHost(e.NewSize.Width));

    private void OnLayoutHostSizeChanged(object? sender, SizeChangedEventArgs e)
        => UpdateResponsiveState(e.NewSize.Width);

    private double ConstrainToLayoutHost(double width) =>
        _layoutHost is { Bounds: { Width: > 0 } } ? Math.Min(width, _layoutHost.Bounds.Width) : width;

    private void UpdateResponsiveState(double width)
    {
        if (_userOverride || double.IsInfinity(width)) return;
        if (width < ResponsiveCollapseWidth && IsPreviewOpen)
        {
            _autoCollapsed = true;
            SetPreviewOpen(false);
        }
        else if (width >= ResponsiveCollapseWidth && _autoCollapsed && !IsPreviewOpen)
        {
            _autoCollapsed = false;
            SetPreviewOpen(true);
        }
    }

    private void CollapsePreview(object? sender, RoutedEventArgs e)
    {
        _userOverride = true;
        _autoCollapsed = false;
        SetPreviewOpen(false);
    }

    private void ExpandPreview(object? sender, RoutedEventArgs e)
    {
        _userOverride = true;
        _autoCollapsed = false;
        SetPreviewOpen(true);
    }
}
