// Launcher for the PC port of Mario & Luigi: Bowser's Inside Story.
//
// The game is described in game.json next to the executable (see README): ROM check (SHA-1),
// one-time setup steps (extracting the game data), how to start the port, texts, controls and
// the screenshot folder. Settings are stored per user and passed to the game as environment
// variables (BIS_SCALE, BIS_FULLSCREEN, BIS_FILTER, BIS_CARD_DELAY).
//
// Built with the C# compiler that ships with Windows (.NET Framework 4.x, WPF), so no SDK is
// required: see build.bat. C# 5 syntax only.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shell;
using System.Windows.Threading;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("Bowser's Inside Story PC Launcher")]
[assembly: System.Reflection.AssemblyProduct("bis-recomp")]
[assembly: System.Reflection.AssemblyVersion("0.2.0.0")]

namespace RecompLauncher
{
    // ------------------------------------------------------------------ data

    class SetupStep
    {
        public string Label, Exe, Args, SkipIfExists;
    }

    class Game
    {
        public string Id, Title, Subtitle, Platform, Region, Accent, Status, About, Port, Screenshots;
        public string RomLabel, RomSha1, RomFilter, RomTarget;
        public string LaunchExe, LaunchArgs, BuildHint;
        public List<SetupStep> Setup = new List<SetupStep>();
        public List<string[]> Controls = new List<string[]>();
    }

    static class Json
    {
        public static string Str(Dictionary<string, object> d, string key)
        {
            object v;
            return d != null && d.TryGetValue(key, out v) && v != null ? v.ToString() : null;
        }
        public static Dictionary<string, object> Obj(Dictionary<string, object> d, string key)
        {
            object v;
            return d != null && d.TryGetValue(key, out v) ? v as Dictionary<string, object> : null;
        }
        public static object[] Arr(Dictionary<string, object> d, string key)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return new object[0];
            var list = v as System.Collections.ArrayList;
            if (list != null) return list.ToArray();
            return v as object[] ?? new object[0];
        }
        // Text fields may be a string or an array of paragraphs
        public static string Text(Dictionary<string, object> d, string key)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return null;
            var list = v as System.Collections.ArrayList;
            if (list != null) return string.Join("\n\n", list.ToArray().Select(o => o.ToString()));
            return v.ToString();
        }
    }

    // Per-user state: verified ROM, last session and the game settings
    class Settings
    {
        static readonly string Dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RecompLauncher");
        static readonly string FilePath = Path.Combine(Dir, "settings.json");
        public Dictionary<string, object> Data = new Dictionary<string, object>();

        public static Settings Load()
        {
            var s = new Settings();
            try
            {
                if (File.Exists(FilePath))
                    s.Data = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(FilePath)) ?? s.Data;
            }
            catch (Exception) { }
            return s;
        }
        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Dir);
                File.WriteAllText(FilePath, new JavaScriptSerializer().Serialize(Data));
            }
            catch (Exception) { }
        }
        public string Get(string key) { object v; return Data.TryGetValue(key, out v) && v != null ? v.ToString() : null; }
        public string Get(string key, string def) { return Get(key) ?? def; }
        public void Set(string key, string value) { Data[key] = value; Save(); }
    }

    // ------------------------------------------------------------------ theme

    static class Theme
    {
        public static readonly Brush Background = Hex("#0D0E12");
        public static readonly Brush Panel = Hex("#14161B");
        public static readonly Brush CardBg = Hex("#181B21");
        public static readonly Brush CardBorder = Hex("#252932");
        public static readonly Brush Control = Hex("#22262E");
        public static readonly Brush Text = Hex("#ECEEF2");
        public static readonly Brush TextDim = Hex("#A0A7B3");
        public static readonly Brush TextFaint = Hex("#666D79");
        public static readonly Brush Good = Hex("#3DD68C");
        public static readonly Brush Warn = Hex("#F5B544");
        public static readonly Brush Bad = Hex("#F2555A");
        public static readonly FontFamily Font = new FontFamily("Segoe UI Variable Display, Segoe UI");
        public static Color Accent = Color.FromRgb(0xE5, 0x48, 0x4D);
        public static Brush AccentBrush = new SolidColorBrush(Accent);

        public static SolidColorBrush Hex(string hex)
        {
            var b = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
            b.Freeze();
            return b;
        }
        public static Color ColorOf(string hex, string fallback)
        {
            try { return (Color)ColorConverter.ConvertFromString(hex ?? fallback); }
            catch (Exception) { return (Color)ColorConverter.ConvertFromString(fallback); }
        }
        public static Color Darken(Color c, double f)
        {
            return Color.FromRgb((byte)(c.R * (1 - f)), (byte)(c.G * (1 - f)), (byte)(c.B * (1 - f)));
        }

        // Flat button with rounded corners and hover/press states
        const string ButtonTemplate =
            "<ControlTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' " +
            " xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='Button'>" +
            "<Border x:Name='b' Background='{TemplateBinding Background}' BorderBrush='{TemplateBinding BorderBrush}'" +
            " BorderThickness='{TemplateBinding BorderThickness}' CornerRadius='{TemplateBinding Tag}' Padding='{TemplateBinding Padding}'>" +
            "<ContentPresenter HorizontalAlignment='Center' VerticalAlignment='Center'/></Border>" +
            "<ControlTemplate.Triggers>" +
            "<Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Opacity' Value='0.86'/></Trigger>" +
            "<Trigger Property='IsPressed' Value='True'><Setter TargetName='b' Property='Opacity' Value='0.70'/></Trigger>" +
            "<Trigger Property='IsEnabled' Value='False'><Setter TargetName='b' Property='Opacity' Value='0.35'/></Trigger>" +
            "</ControlTemplate.Triggers></ControlTemplate>";
        static ControlTemplate buttonTemplate;

        public static Button MakeButton(object content, Brush bg, Brush fg, double fontSize, Thickness padding, double radius)
        {
            if (buttonTemplate == null) buttonTemplate = (ControlTemplate)XamlReader.Parse(ButtonTemplate);
            return new Button
            {
                Content = content,
                Background = bg,
                Foreground = fg,
                BorderThickness = new Thickness(0),
                BorderBrush = Brushes.Transparent,
                FontFamily = Font,
                FontSize = fontSize,
                FontWeight = FontWeights.SemiBold,
                Padding = padding,
                Cursor = Cursors.Hand,
                Template = buttonTemplate,
                Tag = new CornerRadius(radius),
                Focusable = false
            };
        }

        // Toggle switch for CheckBox
        const string ToggleTemplate =
            "<ControlTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' " +
            " xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='CheckBox'>" +
            "<Border x:Name='track' Width='42' Height='24' CornerRadius='12' Background='#2C313B' Cursor='Hand'>" +
            "<Ellipse x:Name='knob' Width='18' Height='18' Fill='#C9CED8' HorizontalAlignment='Left' Margin='3,0,0,0'/></Border>" +
            "<ControlTemplate.Triggers>" +
            "<Trigger Property='IsChecked' Value='True'><Setter TargetName='track' Property='Background' Value='{Binding Background, RelativeSource={RelativeSource TemplatedParent}}'/>" +
            "<Setter TargetName='knob' Property='HorizontalAlignment' Value='Right'/><Setter TargetName='knob' Property='Margin' Value='0,0,3,0'/>" +
            "<Setter TargetName='knob' Property='Fill' Value='White'/></Trigger>" +
            "<Trigger Property='IsEnabled' Value='False'><Setter TargetName='track' Property='Opacity' Value='0.35'/></Trigger>" +
            "</ControlTemplate.Triggers></ControlTemplate>";
        static ControlTemplate toggleTemplate;

        public static CheckBox MakeToggle(bool on)
        {
            if (toggleTemplate == null) toggleTemplate = (ControlTemplate)XamlReader.Parse(ToggleTemplate);
            return new CheckBox { IsChecked = on, Template = toggleTemplate, Background = AccentBrush, Focusable = false, VerticalAlignment = VerticalAlignment.Center };
        }

        public static TextBlock Label(string text, double size, Brush fg, FontWeight weight)
        {
            return new TextBlock { Text = text, FontSize = size, Foreground = fg, FontWeight = weight, FontFamily = Font, TextWrapping = TextWrapping.Wrap };
        }

        public static Border Card(UIElement body)
        {
            return new Border { Background = CardBg, BorderBrush = CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(14), Padding = new Thickness(22, 18, 22, 20), Child = body };
        }

        public static TextBlock Heading(string text)
        {
            var t = Label(text.ToUpperInvariant(), 10.5, TextFaint, FontWeights.SemiBold);
            t.Margin = new Thickness(0, 0, 0, 12);
            return t;
        }
    }

    // ------------------------------------------------------------------ window

    class LauncherWindow : Window
    {
        readonly string baseDir;
        readonly Settings settings = Settings.Load();
        Game game;
        bool busy;

        // header / tabs
        Button tabOverview, tabSettings;
        FrameworkElement overviewPage, settingsPage;
        // play area
        TextBlock statusText, romBadge, buildBadge;
        Button playButton, romButton;
        ProgressBar progress;
        // gallery
        List<string> shots = new List<string>();
        int shotIndex;
        Image shotImage;
        StackPanel dots, thumbs;
        Grid galleryEmpty;
        Button prevButton, nextButton;
        DispatcherTimer galleryTimer;

        public LauncherWindow(string baseDir)
        {
            this.baseDir = baseDir;
            game = LoadGame();
            Theme.Accent = Theme.ColorOf(game.Accent, "#E5484D");
            Theme.AccentBrush = new SolidColorBrush(Theme.Accent);

            Title = game.Title;
            Width = 1180; Height = 760; MinWidth = 1000; MinHeight = 660;
            Background = Theme.Background;
            FontFamily = Theme.Font;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            WindowStyle = WindowStyle.None;
            WindowChrome.SetWindowChrome(this, new WindowChrome { CaptionHeight = 52, ResizeBorderThickness = new Thickness(6), GlassFrameThickness = new Thickness(0), CornerRadius = new CornerRadius(0) });

            Content = BuildUi();
            LoadScreenshots();
            ShowTab(false);
            RefreshState();
            PreviewKeyDown += (s, e) =>
            {
                if (overviewPage.Visibility != Visibility.Visible) return;
                if (e.Key == Key.Left) { Step(-1); e.Handled = true; }
                if (e.Key == Key.Right) { Step(1); e.Handled = true; }
            };
        }

        // -------- game.json

        Game LoadGame()
        {
            var g = new Game { Id = "game", Title = "Game", Platform = "" };
            string path = new[] { "game.json", "launcher\\game.json" }.Select(p => Path.Combine(baseDir, p)).FirstOrDefault(File.Exists);
            if (path == null)
            {
                MessageBox.Show("game.json was not found next to the launcher.", "Launcher", MessageBoxButton.OK, MessageBoxImage.Warning);
                return g;
            }
            try
            {
                var d = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(path));
                g.Id = Json.Str(d, "id") ?? "game";
                g.Title = Json.Str(d, "title") ?? "Game";
                g.Subtitle = Json.Str(d, "subtitle");
                g.Platform = Json.Str(d, "platform");
                g.Region = Json.Str(d, "region");
                g.Accent = Json.Str(d, "accent");
                g.Status = Json.Str(d, "status");
                g.About = Json.Text(d, "about");
                g.Port = Json.Text(d, "port");
                g.Screenshots = Json.Str(d, "screenshots");
                var rom = Json.Obj(d, "rom");
                g.RomLabel = Json.Str(rom, "label");
                g.RomSha1 = Json.Str(rom, "sha1");
                g.RomFilter = Json.Str(rom, "filter") ?? "All files (*.*)|*.*";
                g.RomTarget = Json.Str(rom, "target");
                foreach (var s in Json.Arr(d, "setup"))
                {
                    var sd = s as Dictionary<string, object>;
                    if (sd != null)
                        g.Setup.Add(new SetupStep { Label = Json.Str(sd, "label"), Exe = Json.Str(sd, "exe"), Args = Json.Str(sd, "args"), SkipIfExists = Json.Str(sd, "skipIfExists") });
                }
                var launch = Json.Obj(d, "launch");
                g.LaunchExe = Json.Str(launch, "exe");
                g.LaunchArgs = Json.Str(launch, "args") ?? "";
                g.BuildHint = Json.Str(launch, "buildHint");
                foreach (var c in Json.Arr(d, "controls"))
                {
                    var pair = c as System.Collections.ArrayList;
                    var arr = pair != null ? pair.ToArray() : c as object[];
                    if (arr != null && arr.Length >= 2) g.Controls.Add(new[] { arr[0].ToString(), arr[1].ToString() });
                }
            }
            catch (Exception e)
            {
                MessageBox.Show("game.json could not be read:\n" + e.Message, "Launcher", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            return g;
        }

        string Full(string relative) { return relative == null ? null : Path.GetFullPath(Path.Combine(baseDir, relative.Replace('/', Path.DirectorySeparatorChar))); }
        string Expand(string s) { return (s ?? "").Replace("{rom}", Full(game.RomTarget) ?? "").Replace("{base}", baseDir.TrimEnd('\\')); }

        // -------- layout

        UIElement BuildUi()
        {
            var root = new Grid { Background = Theme.Background };
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(52) });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition());
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            // Soft accent glow behind the header
            var glow = new Border
            {
                IsHitTestVisible = false,
                Background = new RadialGradientBrush(Color.FromArgb(0x38, Theme.Accent.R, Theme.Accent.G, Theme.Accent.B), Colors.Transparent)
                {
                    Center = new Point(0.12, 0), GradientOrigin = new Point(0.12, 0), RadiusX = 0.7, RadiusY = 1.4
                }
            };
            Grid.SetRowSpan(glow, 2);
            root.Children.Add(glow);

            root.Children.Add(BuildTitleBar());

            var header = BuildHeader();
            Grid.SetRow(header, 1);
            root.Children.Add(header);

            overviewPage = BuildOverview();
            settingsPage = BuildSettings();
            Grid.SetRow(overviewPage, 2);
            Grid.SetRow(settingsPage, 2);
            root.Children.Add(overviewPage);
            root.Children.Add(settingsPage);

            var footer = new Grid { Margin = new Thickness(40, 10, 40, 16) };
            footer.ColumnDefinitions.Add(new ColumnDefinition());
            footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var note = Theme.Label("Fan-made port. No game data is included – use your own legally obtained copy. Not affiliated with Nintendo.", 11, Theme.TextFaint, FontWeights.Normal);
            footer.Children.Add(note);
            var link = Theme.MakeButton("github.com/DenisTRW/bis-recomp", Brushes.Transparent, Theme.TextFaint, 11, new Thickness(0), 0);
            link.FontWeight = FontWeights.Normal;
            link.Click += delegate { OpenUrl("https://github.com/DenisTRW/bis-recomp"); };
            Grid.SetColumn(link, 1);
            footer.Children.Add(link);
            Grid.SetRow(footer, 3);
            root.Children.Add(footer);
            return root;
        }

        UIElement BuildTitleBar()
        {
            var bar = new Grid();
            var left = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(40, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            var dot = new Border { Width = 10, Height = 10, CornerRadius = new CornerRadius(5), Background = Theme.AccentBrush, VerticalAlignment = VerticalAlignment.Center };
            left.Children.Add(dot);
            var t = Theme.Label("PC PORT  ·  LAUNCHER", 10.5, Theme.TextFaint, FontWeights.SemiBold);
            t.Margin = new Thickness(10, 0, 0, 0);
            left.Children.Add(t);
            bar.Children.Add(left);

            var caption = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top };
            caption.Children.Add(CaptionButton("", delegate { WindowState = WindowState.Minimized; }, false));
            caption.Children.Add(CaptionButton("", delegate { WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; }, false));
            caption.Children.Add(CaptionButton("", delegate { Close(); }, true));
            bar.Children.Add(caption);
            return bar;
        }

        UIElement BuildHeader()
        {
            var g = new Grid { Margin = new Thickness(40, 6, 40, 18) };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var titles = new StackPanel();
            var chip = new Border { Background = new SolidColorBrush(Color.FromArgb(0x30, Theme.Accent.R, Theme.Accent.G, Theme.Accent.B)), CornerRadius = new CornerRadius(6), Padding = new Thickness(9, 3, 9, 4), HorizontalAlignment = HorizontalAlignment.Left };
            chip.Child = Theme.Label(((game.Platform ?? "") + (game.Region != null ? "  ·  " + game.Region : "")).ToUpperInvariant(), 10.5, Theme.Text, FontWeights.SemiBold);
            titles.Children.Add(chip);
            var title = Theme.Label(game.Title, 32, Theme.Text, FontWeights.Bold);
            title.Margin = new Thickness(0, 12, 0, 0);
            titles.Children.Add(title);
            if (game.Subtitle != null)
            {
                var sub = Theme.Label(game.Subtitle, 14, Theme.TextDim, FontWeights.Normal);
                sub.Margin = new Thickness(0, 4, 0, 0);
                titles.Children.Add(sub);
            }
            g.Children.Add(titles);

            // Tabs
            var tabs = new Border { Background = Theme.Panel, BorderBrush = Theme.CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(12), Padding = new Thickness(4), VerticalAlignment = VerticalAlignment.Bottom };
            var tabRow = new StackPanel { Orientation = Orientation.Horizontal };
            tabOverview = Theme.MakeButton("Overview", Brushes.Transparent, Theme.TextDim, 13, new Thickness(20, 8, 20, 9), 9);
            tabSettings = Theme.MakeButton("Settings", Brushes.Transparent, Theme.TextDim, 13, new Thickness(20, 8, 20, 9), 9);
            tabOverview.Click += delegate { ShowTab(false); };
            tabSettings.Click += delegate { ShowTab(true); };
            tabRow.Children.Add(tabOverview);
            tabRow.Children.Add(tabSettings);
            tabs.Child = tabRow;
            WindowChrome.SetIsHitTestVisibleInChrome(tabs, true);
            Grid.SetColumn(tabs, 1);
            g.Children.Add(tabs);
            return g;
        }

        void ShowTab(bool settingsTab)
        {
            overviewPage.Visibility = settingsTab ? Visibility.Collapsed : Visibility.Visible;
            settingsPage.Visibility = settingsTab ? Visibility.Visible : Visibility.Collapsed;
            tabOverview.Background = settingsTab ? Brushes.Transparent : Theme.Control;
            tabOverview.Foreground = settingsTab ? Theme.TextDim : Theme.Text;
            tabSettings.Background = settingsTab ? Theme.Control : Brushes.Transparent;
            tabSettings.Foreground = settingsTab ? Theme.Text : Theme.TextDim;
            var page = settingsTab ? settingsPage : overviewPage;
            page.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180)));
        }

        // -------- overview

        FrameworkElement BuildOverview()
        {
            var g = new Grid { Margin = new Thickness(40, 0, 40, 0) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(400) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(24) });
            g.ColumnDefinitions.Add(new ColumnDefinition());

            g.Children.Add(BuildGallery());

            var right = new Grid();
            right.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            right.RowDefinitions.Add(new RowDefinition());
            Grid.SetColumn(right, 2);
            g.Children.Add(right);

            right.Children.Add(BuildPlayCard());

            var scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Hidden, Margin = new Thickness(0, 16, 0, 0), Padding = new Thickness(0, 0, 6, 0) };
            var info = new StackPanel();
            info.Children.Add(InfoCard("About the game", game.About));
            info.Children.Add(Spacer(14));
            info.Children.Add(PortCard());
            info.Children.Add(Spacer(14));
            info.Children.Add(ControlsCard());
            info.Children.Add(Spacer(6));
            scroll.Content = info;
            Grid.SetRow(scroll, 1);
            right.Children.Add(scroll);
            return g;
        }

        static FrameworkElement Spacer(double h) { return new Border { Height = h }; }

        UIElement BuildPlayCard()
        {
            var accent = Theme.Accent;
            var card = new Border
            {
                CornerRadius = new CornerRadius(16),
                Padding = new Thickness(24, 20, 24, 20),
                Background = new LinearGradientBrush(new GradientStopCollection
                {
                    new GradientStop(accent, 0), new GradientStop(Theme.Darken(accent, 0.5), 0.7), new GradientStop(Theme.Darken(accent, 0.72), 1)
                }, new Point(0, 0), new Point(1, 1))
            };
            var g = new Grid();
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());

            var playContent = new StackPanel { Orientation = Orientation.Horizontal };
            playContent.Children.Add(new TextBlock { Text = "", FontFamily = new FontFamily("Segoe MDL2 Assets, Segoe Fluent Icons"), FontSize = 16, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 1, 12, 0) });
            playContent.Children.Add(new TextBlock { Text = "LAUNCH GAME", FontSize = 16, FontWeight = FontWeights.Bold, VerticalAlignment = VerticalAlignment.Center });
            playButton = Theme.MakeButton(playContent, Brushes.White, Brushes.Black, 16, new Thickness(34, 14, 36, 14), 12);
            playButton.Effect = new DropShadowEffect { BlurRadius = 24, ShadowDepth = 4, Opacity = 0.35 };
            playButton.Click += delegate { Play(); };
            g.Children.Add(playButton);

            var side = new StackPanel { Margin = new Thickness(18, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            var badges = new StackPanel { Orientation = Orientation.Horizontal };
            badges.Children.Add(Badge(out romBadge));
            var bb = Badge(out buildBadge);
            bb.Margin = new Thickness(8, 0, 0, 0);
            badges.Children.Add(bb);
            side.Children.Add(badges);
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 10, 0, 0) };
            romButton = Theme.MakeButton("Select ROM…", new SolidColorBrush(Color.FromArgb(0x40, 0, 0, 0)), Brushes.White, 12.5, new Thickness(14, 7, 14, 8), 8);
            romButton.Click += delegate { SelectRom(); };
            buttons.Children.Add(romButton);
            var folder = Theme.MakeButton("Open folder", new SolidColorBrush(Color.FromArgb(0x40, 0, 0, 0)), Brushes.White, 12.5, new Thickness(14, 7, 14, 8), 8);
            folder.Margin = new Thickness(8, 0, 0, 0);
            folder.Click += delegate { Process.Start("explorer.exe", "\"" + baseDir + "\""); };
            buttons.Children.Add(folder);
            side.Children.Add(buttons);
            Grid.SetColumn(side, 1);
            g.Children.Add(side);

            var statusRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(2, 16, 0, 0) };
            progress = new ProgressBar { Width = 90, Height = 4, IsIndeterminate = true, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 0, 10, 0), Foreground = Brushes.White, Background = new SolidColorBrush(Color.FromArgb(0x40, 0, 0, 0)), BorderThickness = new Thickness(0), VerticalAlignment = VerticalAlignment.Center };
            statusRow.Children.Add(progress);
            statusText = Theme.Label("", 12.5, new SolidColorBrush(Color.FromArgb(0xE0, 0xFF, 0xFF, 0xFF)), FontWeights.Normal);
            statusText.TextWrapping = TextWrapping.NoWrap;
            statusText.TextTrimming = TextTrimming.CharacterEllipsis;
            statusRow.Children.Add(statusText);
            Grid.SetRow(statusRow, 1);
            Grid.SetColumnSpan(statusRow, 2);
            g.Children.Add(statusRow);

            card.Child = g;
            return card;
        }

        Border Badge(out TextBlock text)
        {
            text = Theme.Label("", 11.5, Brushes.White, FontWeights.SemiBold);
            return new Border { Background = new SolidColorBrush(Color.FromArgb(0x48, 0, 0, 0)), CornerRadius = new CornerRadius(14), Padding = new Thickness(11, 5, 12, 6), Child = text };
        }

        UIElement InfoCard(string title, string body)
        {
            var p = new StackPanel();
            p.Children.Add(Theme.Heading(title));
            AddParagraphs(p, body ?? "");
            return Theme.Card(p);
        }

        static void AddParagraphs(Panel p, string body)
        {
            var paras = body.Split(new[] { "\n\n" }, StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < paras.Length; i++)
            {
                var t = Theme.Label(paras[i].Trim(), 13.5, Theme.TextDim, FontWeights.Normal);
                t.LineHeight = 21;
                if (i > 0) t.Margin = new Thickness(0, 10, 0, 0);
                p.Children.Add(t);
            }
        }

        UIElement PortCard()
        {
            var p = new StackPanel();
            p.Children.Add(Theme.Heading("About this port"));
            if (!string.IsNullOrEmpty(game.Status))
            {
                var st = new Border { Background = new SolidColorBrush(Color.FromArgb(0x26, 0xF5, 0xB5, 0x44)), BorderBrush = new SolidColorBrush(Color.FromArgb(0x50, 0xF5, 0xB5, 0x44)), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(12, 8, 12, 9), Margin = new Thickness(0, 0, 0, 12) };
                st.Child = Theme.Label(game.Status, 12.5, Theme.Warn, FontWeights.SemiBold);
                p.Children.Add(st);
            }
            AddParagraphs(p, game.Port ?? "");
            if (game.RomLabel != null)
            {
                var v = new TextBlock { FontFamily = Theme.Font, FontSize = 12.5, Margin = new Thickness(0, 12, 0, 0), TextWrapping = TextWrapping.Wrap };
                v.Inlines.Add(new Run("Supported version: ") { Foreground = Theme.TextFaint });
                v.Inlines.Add(new Run(game.RomLabel) { Foreground = Theme.Text });
                p.Children.Add(v);
            }
            return Theme.Card(p);
        }

        UIElement ControlsCard()
        {
            var p = new StackPanel();
            p.Children.Add(Theme.Heading("Controls"));
            var grid = new UniformGrid { Columns = 2 };
            foreach (var c in game.Controls)
            {
                var row = new Grid { Margin = new Thickness(0, 0, 12, 8) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(104) });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                var key = new Border { Background = Theme.Control, BorderBrush = Theme.CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(6), Padding = new Thickness(8, 2, 8, 3), HorizontalAlignment = HorizontalAlignment.Left, Child = Theme.Label(c[0], 12, Theme.Text, FontWeights.SemiBold) };
                row.Children.Add(key);
                var action = Theme.Label(c[1], 12.5, Theme.TextDim, FontWeights.Normal);
                action.VerticalAlignment = VerticalAlignment.Center;
                Grid.SetColumn(action, 1);
                row.Children.Add(action);
                grid.Children.Add(row);
            }
            p.Children.Add(grid);
            return Theme.Card(p);
        }

        // -------- gallery

        UIElement BuildGallery()
        {
            var g = new Grid();
            g.RowDefinitions.Add(new RowDefinition());
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var frame = new Border { Background = Hexa("#0A0B0E"), BorderBrush = Theme.CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(16), ClipToBounds = true };
            var stage = new Grid();
            shotImage = new Image { Stretch = Stretch.Uniform, Margin = new Thickness(18) };
            RenderOptions.SetBitmapScalingMode(shotImage, BitmapScalingMode.NearestNeighbor);
            stage.Children.Add(shotImage);

            galleryEmpty = new Grid { Visibility = Visibility.Collapsed };
            var emptyText = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(36) };
            var icon = new TextBlock { Text = "", FontFamily = new FontFamily("Segoe MDL2 Assets, Segoe Fluent Icons"), FontSize = 34, Foreground = Theme.TextFaint, HorizontalAlignment = HorizontalAlignment.Center };
            emptyText.Children.Add(icon);
            var et = Theme.Label("No screenshots yet", 15, Theme.Text, FontWeights.SemiBold);
            et.HorizontalAlignment = HorizontalAlignment.Center;
            et.Margin = new Thickness(0, 12, 0, 6);
            emptyText.Children.Add(et);
            var ed = Theme.Label("Put PNG or JPG images into the screenshots folder. They are created from your own copy of the game and stay on your PC.", 12.5, Theme.TextDim, FontWeights.Normal);
            ed.TextAlignment = TextAlignment.Center;
            emptyText.Children.Add(ed);
            var open = Theme.MakeButton("Open screenshots folder", Theme.Control, Theme.Text, 12.5, new Thickness(14, 7, 14, 8), 8);
            open.Margin = new Thickness(0, 14, 0, 0);
            open.HorizontalAlignment = HorizontalAlignment.Center;
            open.Click += delegate
            {
                string dir = Full(game.Screenshots ?? "screenshots");
                Directory.CreateDirectory(dir);
                Process.Start("explorer.exe", "\"" + dir + "\"");
            };
            emptyText.Children.Add(open);
            galleryEmpty.Children.Add(emptyText);
            stage.Children.Add(galleryEmpty);

            prevButton = ArrowButton("", HorizontalAlignment.Left);
            prevButton.Click += delegate { Step(-1); };
            nextButton = ArrowButton("", HorizontalAlignment.Right);
            nextButton.Click += delegate { Step(1); };
            stage.Children.Add(prevButton);
            stage.Children.Add(nextButton);
            stage.MouseWheel += (s, e) => Step(e.Delta > 0 ? -1 : 1);
            frame.Child = stage;
            g.Children.Add(frame);

            dots = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 12, 0, 0) };
            Grid.SetRow(dots, 1);
            g.Children.Add(dots);

            var thumbScroll = new ScrollViewer { HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled, Margin = new Thickness(0, 10, 0, 0) };
            thumbs = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center };
            thumbScroll.Content = thumbs;
            Grid.SetRow(thumbScroll, 2);
            g.Children.Add(thumbScroll);

            galleryTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(6) };
            galleryTimer.Tick += delegate { if (IsActive && shots.Count > 1) Show(shotIndex + 1, false); };
            return g;
        }

        static Brush Hexa(string hex) { return Theme.Hex(hex); }

        Button ArrowButton(string glyph, HorizontalAlignment side)
        {
            var b = Theme.MakeButton(glyph, new SolidColorBrush(Color.FromArgb(0x90, 0x14, 0x16, 0x1B)), Theme.Text, 13, new Thickness(0), 18);
            b.FontFamily = new FontFamily("Segoe MDL2 Assets, Segoe Fluent Icons");
            b.Width = 36; b.Height = 36;
            b.HorizontalAlignment = side;
            b.VerticalAlignment = VerticalAlignment.Center;
            b.Margin = new Thickness(10);
            return b;
        }

        void LoadScreenshots()
        {
            shots.Clear();
            string dir = Full(game.Screenshots ?? "screenshots");
            if (Directory.Exists(dir))
                shots = Directory.GetFiles(dir)
                    .Where(f => new[] { ".png", ".jpg", ".jpeg" }.Contains(Path.GetExtension(f).ToLowerInvariant()))
                    .OrderBy(f => f, StringComparer.OrdinalIgnoreCase).ToList();

            thumbs.Children.Clear();
            dots.Children.Clear();
            bool any = shots.Count > 0;
            galleryEmpty.Visibility = any ? Visibility.Collapsed : Visibility.Visible;
            shotImage.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
            prevButton.Visibility = nextButton.Visibility = shots.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
            for (int i = 0; i < shots.Count; i++)
            {
                int k = i;
                var img = new Image { Source = LoadBitmap(shots[i], 64), Stretch = Stretch.UniformToFill, Width = 52, Height = 52 };
                RenderOptions.SetBitmapScalingMode(img, BitmapScalingMode.HighQuality);
                var t = new Border { Width = 56, Height = 56, CornerRadius = new CornerRadius(9), BorderThickness = new Thickness(2), BorderBrush = Brushes.Transparent, Margin = new Thickness(4, 0, 4, 0), Cursor = Cursors.Hand, ClipToBounds = true, Child = img, Background = Theme.Panel };
                t.MouseLeftButtonUp += delegate { Show(k, true); };
                thumbs.Children.Add(t);
                dots.Children.Add(new Border { Width = 6, Height = 6, CornerRadius = new CornerRadius(3), Margin = new Thickness(3, 0, 3, 0), Background = Theme.TextFaint });
            }
            if (any) Show(0, false);
            if (shots.Count > 1) galleryTimer.Start(); else galleryTimer.Stop();
        }

        static BitmapImage LoadBitmap(string path, int decodeHeight)
        {
            try
            {
                var bmp = new BitmapImage();
                bmp.BeginInit();
                bmp.CacheOption = BitmapCacheOption.OnLoad;   // do not keep the file locked
                bmp.UriSource = new Uri(path);
                if (decodeHeight > 0) bmp.DecodePixelHeight = decodeHeight;
                bmp.EndInit();
                bmp.Freeze();
                return bmp;
            }
            catch (Exception) { return null; }
        }

        void Step(int delta)
        {
            if (shots.Count == 0) return;
            Show(shotIndex + delta, true);
        }

        void Show(int index, bool manual)
        {
            if (shots.Count == 0) return;
            shotIndex = ((index % shots.Count) + shots.Count) % shots.Count;
            shotImage.Source = LoadBitmap(shots[shotIndex], 0);
            shotImage.BeginAnimation(OpacityProperty, new DoubleAnimation(0.2, 1, TimeSpan.FromMilliseconds(220)));
            for (int i = 0; i < thumbs.Children.Count; i++)
            {
                ((Border)thumbs.Children[i]).BorderBrush = i == shotIndex ? Theme.AccentBrush : Brushes.Transparent;
                ((Border)thumbs.Children[i]).Opacity = i == shotIndex ? 1 : 0.6;
                var d = (Border)dots.Children[i];
                d.Width = i == shotIndex ? 18 : 6;
                d.Background = i == shotIndex ? Theme.AccentBrush : Theme.TextFaint;
            }
            if (manual && galleryTimer.IsEnabled) { galleryTimer.Stop(); galleryTimer.Start(); }   // restart the interval
        }

        // -------- settings

        FrameworkElement BuildSettings()
        {
            var scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Hidden, Margin = new Thickness(40, 0, 40, 0), Padding = new Thickness(0, 0, 6, 0) };
            var g = new Grid();
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            scroll.Content = g;

            var left = new StackPanel();
            g.Children.Add(left);
            var right = new StackPanel();
            Grid.SetColumn(right, 2);
            g.Children.Add(right);

            // Display
            var display = new StackPanel();
            display.Children.Add(Theme.Heading("Display"));
            display.Children.Add(SettingRow("Window size", "Initial size of the game window. It is reduced automatically if it does not fit your screen.",
                Segmented("scale", new[] { "1", "2", "3", "4" }, new[] { "1×", "2×", "3×", "4×" }, "2")));
            display.Children.Add(SettingRow("Start in fullscreen", "You can always switch with F11 while playing.", Toggle("fullscreen", false)));
            display.Children.Add(SettingRow("Pixel filter", "Sharp keeps the original pixels, smooth blurs them slightly when scaled.",
                Segmented("filter", new[] { "sharp", "smooth" }, new[] { "Sharp", "Smooth" }, "sharp")));
            left.Children.Add(Theme.Card(display));
            left.Children.Add(Spacer(14));

            // Experimental
            var exp = new StackPanel();
            exp.Children.Add(Theme.Heading("Experimental"));
            exp.Children.Add(SettingRow("Accurate card timing", "Lets game data arrive from the cartridge with a delay, like on the real console. Needed to get further after starting a new game. May still be unstable.",
                Toggle("cardTiming", false)));
            left.Children.Add(Theme.Card(exp));

            // Coming later
            var later = new StackPanel();
            later.Children.Add(Theme.Heading("Coming later"));
            foreach (var item in new[]
            {
                new[] { "Graphics enhancements", "Higher internal resolution for 3D, widescreen and screen layouts." },
                new[] { "Audio", "Music and sound effects (not implemented yet)." },
                new[] { "Controller and key mapping", "Gamepad support and freely assignable keys." },
                new[] { "Touch screen", "Use the mouse as the stylus on the bottom screen." }
            })
            {
                var row = SettingRow(item[0], item[1], Pill("Planned"));
                row.Opacity = 0.75;
                later.Children.Add(row);
            }
            right.Children.Add(Theme.Card(later));
            right.Children.Add(Spacer(14));

            // Files
            var files = new StackPanel();
            files.Children.Add(Theme.Heading("Files"));
            var logs = Theme.MakeButton("Open log folder", Theme.Control, Theme.Text, 12.5, new Thickness(14, 7, 14, 8), 8);
            logs.Click += delegate { string d = Path.Combine(baseDir, "build", "logs"); Directory.CreateDirectory(d); Process.Start("explorer.exe", "\"" + d + "\""); };
            files.Children.Add(SettingRow("Game log", "Output of the last session, useful for bug reports.", logs));
            var shotsBtn = Theme.MakeButton("Open folder", Theme.Control, Theme.Text, 12.5, new Thickness(14, 7, 14, 8), 8);
            shotsBtn.Click += delegate { string d = Full(game.Screenshots ?? "screenshots"); Directory.CreateDirectory(d); Process.Start("explorer.exe", "\"" + d + "\""); };
            files.Children.Add(SettingRow("Screenshots", "Images shown in the gallery. Reloaded when the launcher starts.", shotsBtn));
            var reset = Theme.MakeButton("Reset settings", Theme.Control, Theme.Text, 12.5, new Thickness(14, 7, 14, 8), 8);
            reset.Click += delegate
            {
                foreach (var k in new[] { "scale", "fullscreen", "filter", "cardTiming" }) settings.Data.Remove(game.Id + ":" + k);
                settings.Save();
                var newPage = BuildSettings();
                var parent = (Grid)settingsPage.Parent;
                Grid.SetRow(newPage, 2);
                parent.Children.Remove(settingsPage);
                parent.Children.Add(newPage);
                settingsPage = newPage;
                ShowTab(true);
            };
            files.Children.Add(SettingRow("Defaults", "Restore all settings on this page.", reset));
            right.Children.Add(Theme.Card(files));

            var hint = Theme.Label("Settings are applied the next time the game starts.", 11.5, Theme.TextFaint, FontWeights.Normal);
            hint.Margin = new Thickness(2, 12, 0, 8);
            left.Children.Add(hint);
            return scroll;
        }

        FrameworkElement SettingRow(string title, string desc, UIElement control)
        {
            var g = new Grid { Margin = new Thickness(0, 4, 0, 14) };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var text = new StackPanel { Margin = new Thickness(0, 0, 18, 0) };
            text.Children.Add(Theme.Label(title, 13.5, Theme.Text, FontWeights.SemiBold));
            var d = Theme.Label(desc, 12, Theme.TextDim, FontWeights.Normal);
            d.Margin = new Thickness(0, 3, 0, 0);
            d.LineHeight = 18;
            text.Children.Add(d);
            g.Children.Add(text);
            var fe = control as FrameworkElement;
            if (fe != null) fe.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(control, 1);
            g.Children.Add(control);
            return g;
        }

        UIElement Pill(string text)
        {
            return new Border { Background = Theme.Control, CornerRadius = new CornerRadius(10), Padding = new Thickness(10, 3, 10, 4), Child = Theme.Label(text, 11, Theme.TextDim, FontWeights.SemiBold) };
        }

        string Setting(string key, string def) { return settings.Get(game.Id + ":" + key, def); }
        void SetSetting(string key, string value) { settings.Set(game.Id + ":" + key, value); }

        UIElement Toggle(string key, bool def)
        {
            var t = Theme.MakeToggle(Setting(key, def ? "1" : "0") == "1");
            t.Checked += delegate { SetSetting(key, "1"); };
            t.Unchecked += delegate { SetSetting(key, "0"); };
            return t;
        }

        UIElement Segmented(string key, string[] values, string[] labels, string def)
        {
            var box = new Border { Background = Theme.Panel, BorderBrush = Theme.CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(9), Padding = new Thickness(3) };
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            var buttons = new List<Button>();
            Action refresh = delegate
            {
                string cur = Setting(key, def);
                for (int i = 0; i < buttons.Count; i++)
                {
                    bool on = values[i] == cur;
                    buttons[i].Background = on ? Theme.AccentBrush : Brushes.Transparent;
                    buttons[i].Foreground = on ? Brushes.White : Theme.TextDim;
                }
            };
            for (int i = 0; i < values.Length; i++)
            {
                string v = values[i];
                var b = Theme.MakeButton(labels[i], Brushes.Transparent, Theme.TextDim, 12.5, new Thickness(12, 5, 12, 6), 7);
                b.MinWidth = 40;
                b.Click += delegate { SetSetting(key, v); refresh(); };
                buttons.Add(b);
                row.Children.Add(b);
            }
            refresh();
            box.Child = row;
            return box;
        }

        Button CaptionButton(string glyph, RoutedEventHandler click, bool danger)
        {
            var b = Theme.MakeButton(glyph, Brushes.Transparent, Theme.TextDim, 10, new Thickness(0), 0);
            b.FontFamily = new FontFamily("Segoe MDL2 Assets, Segoe Fluent Icons");
            b.FontWeight = FontWeights.Normal;
            b.Width = 46; b.Height = 40;
            b.Click += click;
            b.MouseEnter += delegate { b.Background = danger ? Theme.Hex("#C42B1C") : Theme.Hex("#22252C"); b.Foreground = Brushes.White; };
            b.MouseLeave += delegate { b.Background = Brushes.Transparent; b.Foreground = Theme.TextDim; };
            WindowChrome.SetIsHitTestVisibleInChrome(b, true);
            return b;
        }

        static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch (Exception) { }
        }

        // -------- state

        enum RomState { Missing, Verified, Unverified, Wrong }

        RomState CheckRom(bool hashNow)
        {
            string target = Full(game.RomTarget);
            if (target == null || !File.Exists(target)) return RomState.Missing;
            if (string.IsNullOrEmpty(game.RomSha1)) return RomState.Verified;
            var fi = new FileInfo(target);
            string stamp = fi.Length + ":" + fi.LastWriteTimeUtc.Ticks;
            if (settings.Get("verified:" + game.Id) == stamp) return RomState.Verified;
            if (!hashNow) return RomState.Unverified;
            bool ok = string.Equals(Sha1(target), game.RomSha1, StringComparison.OrdinalIgnoreCase);
            if (ok) settings.Set("verified:" + game.Id, stamp);
            return ok ? RomState.Verified : RomState.Wrong;
        }

        bool IsBuilt() { return game.LaunchExe != null && File.Exists(Full(game.LaunchExe)); }

        void RefreshState()
        {
            var rom = CheckRom(false);
            if (rom == RomState.Unverified)
            {
                SetBadge(romBadge, "•  Checking ROM…", Theme.TextDim);
                Task.Run(() => CheckRom(true)).ContinueWith(t => Dispatcher.Invoke(new Action(RefreshState)));
                return;
            }
            switch (rom)
            {
                case RomState.Verified: SetBadge(romBadge, "●  ROM verified", Theme.Good); break;
                case RomState.Wrong: SetBadge(romBadge, "●  Unsupported ROM", Theme.Bad); break;
                default: SetBadge(romBadge, "●  ROM required", Theme.Warn); break;
            }
            bool built = IsBuilt();
            SetBadge(buildBadge, built ? "●  Ready" : "●  Not built", built ? Theme.Good : Theme.Warn);

            playButton.IsEnabled = !busy && rom == RomState.Verified && built;
            romButton.IsEnabled = !busy;
            romButton.Content = rom == RomState.Verified ? "Change ROM…" : "Select ROM…";

            if (busy) return;
            if (rom != RomState.Verified) ShowStatus(rom == RomState.Wrong ? "The ROM in place does not match the supported version. Select the correct one." : "Select your own copy of the game to get started.");
            else if (!built) ShowStatus(game.BuildHint ?? "The game executable was not found.");
            else ShowStatus(settings.Get("last:" + game.Id) ?? "Ready to play.");
        }

        void SetBadge(TextBlock t, string text, Brush color)
        {
            t.Inlines.Clear();
            var parts = text.Split(new[] { "  " }, 2, StringSplitOptions.None);
            if (parts.Length == 2)
            {
                t.Inlines.Add(new Run(parts[0] + "  ") { Foreground = color });
                t.Inlines.Add(new Run(parts[1]) { Foreground = Brushes.White });
            }
            else t.Text = text;
        }

        void ShowStatus(string text) { statusText.Text = text; }

        void SetBusy(bool b, string text)
        {
            busy = b;
            progress.Visibility = b ? Visibility.Visible : Visibility.Collapsed;
            if (text != null) ShowStatus(text);
            RefreshState();
        }

        // -------- actions

        static string Sha1(string path)
        {
            using (var s = File.OpenRead(path))
            using (var sha = SHA1.Create())
                return BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "");
        }

        async void SelectRom()
        {
            if (game.RomTarget == null) return;
            var dlg = new OpenFileDialog { Title = "Select your copy of " + game.Title, Filter = game.RomFilter + "|All files (*.*)|*.*" };
            if (dlg.ShowDialog(this) != true) return;
            string source = dlg.FileName, target = Full(game.RomTarget);

            SetBusy(true, "Checking ROM…");
            string error = null;
            try
            {
                string hash = await Task.Run(() => Sha1(source));
                if (!string.IsNullOrEmpty(game.RomSha1) && !string.Equals(hash, game.RomSha1, StringComparison.OrdinalIgnoreCase))
                    error = "This file is not the supported version (" + (game.RomLabel ?? "see game.json") + ").\nSHA-1: " + hash;
                else
                {
                    if (!string.Equals(Path.GetFullPath(source), target, StringComparison.OrdinalIgnoreCase))
                    {
                        ShowStatus("Copying ROM…");
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        await Task.Run(() => File.Copy(source, target, true));
                    }
                    var fi = new FileInfo(target);
                    settings.Set("verified:" + game.Id, fi.Length + ":" + fi.LastWriteTimeUtc.Ticks);
                    error = await RunSetup();
                }
            }
            catch (Exception e) { error = e.Message; }
            SetBusy(false, null);
            if (error != null)
            {
                ShowStatus(error.Split('\n')[0]);
                MessageBox.Show(this, error, game.Title, MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            else ShowStatus("ROM verified. Ready to play.");
        }

        // Runs the one-time setup steps. Returns an error message or null.
        async Task<string> RunSetup()
        {
            foreach (var step in game.Setup)
            {
                // Already done (e.g. data extracted before). A new ROM of the same version yields the same data.
                if (step.SkipIfExists != null && File.Exists(Full(step.SkipIfExists))) continue;
                string exe = Full(step.Exe);
                if (exe == null || !File.Exists(exe))
                    return (step.Label ?? "Setup") + " needs " + step.Exe + ", which was not found.";
                ShowStatus((step.Label ?? "Preparing") + "…");
                var psi = new ProcessStartInfo(exe, Expand(step.Args))
                {
                    WorkingDirectory = baseDir, UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true
                };
                string output = await Task.Run(() =>
                {
                    using (var p = Process.Start(psi))
                    {
                        var err = p.StandardError.ReadToEndAsync();
                        string o = p.StandardOutput.ReadToEnd();
                        p.WaitForExit();
                        return p.ExitCode == 0 ? null : (o + err.Result).Trim();
                    }
                });
                if (output != null) return (step.Label ?? "Setup") + " failed:\n" + output;
            }
            return null;
        }

        async void Play()
        {
            if (busy) return;
            SetBusy(true, "Preparing…");
            string error = await RunSetup();
            if (error != null)
            {
                SetBusy(false, null);
                ShowStatus(error.Split('\n')[0]);
                MessageBox.Show(this, error, game.Title, MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            string exe = Full(game.LaunchExe);
            string logDir = Path.Combine(baseDir, "build", "logs");
            Directory.CreateDirectory(logDir);
            string logPath = Path.Combine(logDir, game.Id + ".log");
            var psi = new ProcessStartInfo(exe)
            {
                WorkingDirectory = Path.GetDirectoryName(exe), UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            // Relative arguments are meant relative to the launcher folder
            psi.Arguments = string.Join(" ", SplitArgs(Expand(game.LaunchArgs)).Select(a => Quote(MakeAbsoluteIfPath(a))));
            // Settings -> environment of the game
            psi.EnvironmentVariables["BIS_SCALE"] = Setting("scale", "2");
            psi.EnvironmentVariables["BIS_FULLSCREEN"] = Setting("fullscreen", "0");
            psi.EnvironmentVariables["BIS_FILTER"] = Setting("filter", "sharp");
            if (Setting("cardTiming", "0") == "1") psi.EnvironmentVariables["BIS_CARD_DELAY"] = "2048";

            ShowStatus("Running " + game.Title + "…");
            progress.Visibility = Visibility.Collapsed;
            galleryTimer.Stop();
            WindowState = WindowState.Minimized;

            string summary = await Task.Run(() =>
            {
                using (var log = new StreamWriter(logPath, false, new UTF8Encoding(false)))
                using (var p = new Process { StartInfo = psi })
                {
                    object gate = new object();
                    string stop = null;
                    DataReceivedEventHandler sink = (s, e) =>
                    {
                        if (e.Data == null) return;
                        lock (gate)
                        {
                            log.WriteLine(e.Data);
                            if (e.Data.StartsWith("Abbruch:") || e.Data.StartsWith("Stopped:")) stop = e.Data.Substring(e.Data.IndexOf(':') + 1).Trim();
                        }
                    };
                    p.OutputDataReceived += sink;
                    p.ErrorDataReceived += sink;
                    p.Start();
                    p.BeginOutputReadLine();
                    p.BeginErrorReadLine();
                    p.WaitForExit();
                    return stop;
                }
            });

            WindowState = WindowState.Normal;
            Activate();
            if (shots.Count > 1) galleryTimer.Start();
            string text = "Last session: " + DateTime.Now.ToString("HH:mm") + (summary != null ? " — " + summary : "") + "  (log: build\\logs\\" + game.Id + ".log)";
            settings.Set("last:" + game.Id, text);
            SetBusy(false, null);
        }

        string MakeAbsoluteIfPath(string arg)
        {
            if (arg.StartsWith("-") || Path.IsPathRooted(arg)) return arg;
            string full = Full(arg);
            return (Directory.Exists(full) || File.Exists(full)) ? full : arg;
        }

        static IEnumerable<string> SplitArgs(string s)
        {
            var sb = new StringBuilder();
            bool quoted = false;
            foreach (char ch in s)
            {
                if (ch == '"') { quoted = !quoted; continue; }
                if (ch == ' ' && !quoted) { if (sb.Length > 0) { yield return sb.ToString(); sb.Clear(); } continue; }
                sb.Append(ch);
            }
            if (sb.Length > 0) yield return sb.ToString();
        }

        static string Quote(string a) { return a.Contains(" ") ? "\"" + a + "\"" : a; }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
            var win = new LauncherWindow(baseDir);
            // --snapshot <file.png> [settings]: render the window to an image and exit (for the README)
            if (args.Length >= 2 && args[0] == "--snapshot")
            {
                win.ContentRendered += delegate
                {
                    win.Dispatcher.BeginInvoke(new Action(() =>
                    {
                        if (args.Length >= 3 && args[2] == "settings") win.GetType().GetMethod("ShowTab", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance).Invoke(win, new object[] { true });
                        win.UpdateLayout();
                        var rtb = new RenderTargetBitmap((int)win.ActualWidth, (int)win.ActualHeight, 96, 96, PixelFormats.Pbgra32);
                        rtb.Render((Visual)win.Content);
                        var enc = new PngBitmapEncoder();
                        enc.Frames.Add(BitmapFrame.Create(rtb));
                        using (var f = File.Create(args[1])) enc.Save(f);
                        win.Close();
                    }), DispatcherPriority.ApplicationIdle);
                };
            }
            app.Run(win);
        }
    }
}
