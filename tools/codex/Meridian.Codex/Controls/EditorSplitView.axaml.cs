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
    private const double CollapsedPreviewWidth = 56;
    private const double ResponsiveCollapseWidth = 800;
    private bool _autoCollapsed;
    private bool _userOverride;
    private double _availableWidth = double.PositiveInfinity;
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
    internal bool IsSinglePanePreview => IsPreviewOpen && !FormPresenter.IsVisible;
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
        ShowPreviewButton.IsVisible = !isOpen;
        ApplyLayout();
    }

    private void OnSizeChanged(object? sender, SizeChangedEventArgs e)
        => UpdateResponsiveState(ConstrainToLayoutHost(e.NewSize.Width));

    private void OnLayoutHostSizeChanged(object? sender, SizeChangedEventArgs e)
        => UpdateResponsiveState(e.NewSize.Width);

    private double ConstrainToLayoutHost(double width) =>
        _layoutHost is { Bounds: { Width: > 0 } } ? Math.Min(width, _layoutHost.Bounds.Width) : width;

    private void UpdateResponsiveState(double width)
    {
        if (double.IsInfinity(width)) return;
        _availableWidth = width;
        if (_userOverride)
        {
            ApplyLayout();
            return;
        }
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
        else
        {
            ApplyLayout();
        }
    }

    private void ApplyLayout()
    {
        if (!IsPreviewOpen)
        {
            FormPresenter.IsVisible = true;
            PreviewSplitter.IsVisible = false;
            Grid.SetColumn(PreviewPane, 2);
            Grid.SetColumnSpan(PreviewPane, 1);
            PreviewPane.BorderThickness = new Thickness(1, 0, 0, 0);
            SplitterColumn.Width = new GridLength(0);
            PreviewColumn.Width = new GridLength(CollapsedPreviewWidth);
            PreviewColumn.MaxWidth = CollapsedPreviewWidth;
            InvalidateLayoutGrid();
            return;
        }

        if (_availableWidth < ResponsiveCollapseWidth)
        {
            // A narrow host cannot fit the form minimum + splitter + desktop
            // preview. Use the entire canvas for YAML until Hide returns to the
            // form, keeping both transitions keyboard reachable.
            FormPresenter.IsVisible = false;
            PreviewSplitter.IsVisible = false;
            Grid.SetColumn(PreviewPane, 0);
            Grid.SetColumnSpan(PreviewPane, 3);
            PreviewPane.BorderThickness = new Thickness(0);
            SplitterColumn.Width = new GridLength(0);
            PreviewColumn.Width = new GridLength(0);
            PreviewColumn.MaxWidth = 0;
            InvalidateLayoutGrid();
            return;
        }

        FormPresenter.IsVisible = true;
        PreviewSplitter.IsVisible = true;
        Grid.SetColumn(PreviewPane, 2);
        Grid.SetColumnSpan(PreviewPane, 1);
        PreviewPane.BorderThickness = new Thickness(1, 0, 0, 0);
        SplitterColumn.Width = new GridLength(5);
        PreviewColumn.Width = new GridLength(DefaultPreviewWidth);
        PreviewColumn.MaxWidth = 560;
        InvalidateLayoutGrid();
    }

    private void InvalidateLayoutGrid()
    {
        LayoutGrid.InvalidateMeasure();
        LayoutGrid.InvalidateArrange();
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
