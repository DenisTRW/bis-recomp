// Recomp Launcher - a small game launcher for recompiled PC ports.
//
// Games are described in games.json next to the executable (see README). For each game the
// launcher checks the user's own ROM (SHA-1), copies it to where the port expects it, runs
// optional one-time setup steps (e.g. extracting game data) and starts the game.
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
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shell;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("Recomp Launcher")]
[assembly: System.Reflection.AssemblyProduct("Recomp Launcher")]
[assembly: System.Reflection.AssemblyVersion("0.1.0.0")]

namespace RecompLauncher
{
    // ------------------------------------------------------------------ data

    class SetupStep
    {
        public string Label, Exe, Args, SkipIfExists;
    }

    class Game
    {
        public string Id, Title, Subtitle, Platform, Region, Accent, Status, Description;
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
    }

    // Per-user state: which ROM was verified for which game (so we do not hash 128 MB on every start)
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
        public void Set(string key, string value) { Data[key] = value; Save(); }
    }

    // ------------------------------------------------------------------ theme

    static class Theme
    {
        public static readonly Brush Background = Hex("#0E0F13");
        public static readonly Brush Sidebar = Hex("#15171C");
        public static readonly Brush Card = Hex("#1A1D23");
        public static readonly Brush CardBorder = Hex("#262A32");
        public static readonly Brush Text = Hex("#ECEEF2");
        public static readonly Brush TextDim = Hex("#9AA1AD");
        public static readonly Brush TextFaint = Hex("#646B77");
        public static readonly Brush Good = Hex("#3DD68C");
        public static readonly Brush Warn = Hex("#F5B544");
        public static readonly Brush Bad = Hex("#F2555A");
        public static readonly FontFamily Font = new FontFamily("Segoe UI Variable Display, Segoe UI");

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

        // Flat button with rounded corners and hover/press states
        const string ButtonTemplate =
            "<ControlTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' " +
            " xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='Button'>" +
            "<Border x:Name='b' Background='{TemplateBinding Background}' BorderBrush='{TemplateBinding BorderBrush}'" +
            " BorderThickness='{TemplateBinding BorderThickness}' CornerRadius='{TemplateBinding Tag}' Padding='{TemplateBinding Padding}'>" +
            "<ContentPresenter HorizontalAlignment='Center' VerticalAlignment='Center'/></Border>" +
            "<ControlTemplate.Triggers>" +
            "<Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Opacity' Value='0.88'/></Trigger>" +
            "<Trigger Property='IsPressed' Value='True'><Setter TargetName='b' Property='Opacity' Value='0.72'/></Trigger>" +
            "<Trigger Property='IsEnabled' Value='False'><Setter TargetName='b' Property='Opacity' Value='0.35'/></Trigger>" +
            "</ControlTemplate.Triggers></ControlTemplate>";
        static ControlTemplate buttonTemplate;

        public static Button MakeButton(string text, Brush bg, Brush fg, double fontSize, Thickness padding, double radius)
        {
            if (buttonTemplate == null) buttonTemplate = (ControlTemplate)XamlReader.Parse(ButtonTemplate);
            var b = new Button
            {
                Content = text,
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
            return b;
        }

        public static TextBlock Label(string text, double size, Brush fg, FontWeight weight)
        {
            return new TextBlock { Text = text, FontSize = size, Foreground = fg, FontWeight = weight, FontFamily = Font, TextWrapping = TextWrapping.Wrap };
        }
    }

    // ------------------------------------------------------------------ window

    class LauncherWindow : Window
    {
        readonly string baseDir;
        readonly List<Game> games = new List<Game>();
        readonly Settings settings = Settings.Load();
        Game current;
        bool busy;

        ListBox library;
        Grid hero;
        TextBlock heroTitle, heroSubtitle, heroPlatform, statusText, aboutText, romBadge, buildBadge, footer;
        Border romBadgeBox, buildBadgeBox;
        Button playButton, romButton, folderButton;
        ProgressBar progress;
        StackPanel controlsPanel;

        public LauncherWindow(string baseDir)
        {
            this.baseDir = baseDir;
            Title = "Recomp Launcher";
            Width = 1060; Height = 700; MinWidth = 880; MinHeight = 600;
            Background = Theme.Background;
            FontFamily = Theme.Font;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            WindowStyle = WindowStyle.None;
            WindowChrome.SetWindowChrome(this, new WindowChrome { CaptionHeight = 44, ResizeBorderThickness = new Thickness(6), GlassFrameThickness = new Thickness(0), CornerRadius = new CornerRadius(0) });

            LoadGames();
            Content = BuildUi();
            Loaded += delegate
            {
                if (games.Count > 0) library.SelectedIndex = 0;
                else ShowStatus("No games found. Add entries to games.json next to the launcher.", Theme.Warn);
            };
        }

        // -------- games.json

        void LoadGames()
        {
            string path = Path.Combine(baseDir, "games.json");
            if (!File.Exists(path)) path = Path.Combine(baseDir, "launcher", "games.json");
            if (!File.Exists(path)) return;
            try
            {
                var root = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(path));
                foreach (var o in Json.Arr(root, "games"))
                {
                    var d = o as Dictionary<string, object>;
                    if (d == null) continue;
                    var g = new Game
                    {
                        Id = Json.Str(d, "id") ?? Guid.NewGuid().ToString("N"),
                        Title = Json.Str(d, "title") ?? "Untitled",
                        Subtitle = Json.Str(d, "subtitle"),
                        Platform = Json.Str(d, "platform"),
                        Region = Json.Str(d, "region"),
                        Accent = Json.Str(d, "accent"),
                        Status = Json.Str(d, "status"),
                        Description = Json.Str(d, "description")
                    };
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
                    games.Add(g);
                }
            }
            catch (Exception e)
            {
                MessageBox.Show("games.json could not be read:\n" + e.Message, "Recomp Launcher", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        string Full(string relative) { return relative == null ? null : Path.GetFullPath(Path.Combine(baseDir, relative.Replace('/', Path.DirectorySeparatorChar))); }
        string Expand(Game g, string s) { return (s ?? "").Replace("{rom}", Full(g.RomTarget) ?? "").Replace("{base}", baseDir.TrimEnd('\\')); }

        // -------- layout

        UIElement BuildUi()
        {
            var root = new Grid();
            root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(288) });
            root.ColumnDefinitions.Add(new ColumnDefinition());

            // Sidebar
            var side = new Grid { Background = Theme.Sidebar };
            side.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            side.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            side.RowDefinitions.Add(new RowDefinition());
            side.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var brand = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(24, 22, 24, 26) };
            var logo = new Border
            {
                Width = 30, Height = 30, CornerRadius = new CornerRadius(8),
                Background = new LinearGradientBrush(Color.FromRgb(0x7C, 0x5C, 0xFF), Color.FromRgb(0xE5, 0x48, 0x4D), 45),
                Child = new TextBlock { Text = "R", Foreground = Brushes.White, FontWeight = FontWeights.Bold, FontSize = 16, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center }
            };
            brand.Children.Add(logo);
            var brandText = new StackPanel { Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            brandText.Children.Add(Theme.Label("Recomp", 15, Theme.Text, FontWeights.Bold));
            brandText.Children.Add(Theme.Label("LAUNCHER", 9.5, Theme.TextFaint, FontWeights.SemiBold));
            brand.Children.Add(brandText);
            WindowChrome.SetIsHitTestVisibleInChrome(brand, false);
            Grid.SetRow(brand, 0);
            side.Children.Add(brand);

            var libHeader = Theme.Label("LIBRARY", 10.5, Theme.TextFaint, FontWeights.SemiBold);
            libHeader.Margin = new Thickness(26, 0, 24, 8);
            Grid.SetRow(libHeader, 1);
            side.Children.Add(libHeader);

            library = new ListBox
            {
                Background = Brushes.Transparent, BorderThickness = new Thickness(0),
                Margin = new Thickness(12, 0, 12, 0), Padding = new Thickness(0),
                ItemContainerStyle = LibraryItemStyle()
            };
            ScrollViewer.SetHorizontalScrollBarVisibility(library, ScrollBarVisibility.Disabled);
            foreach (var g in games) library.Items.Add(LibraryItem(g));
            library.SelectionChanged += delegate { if (library.SelectedIndex >= 0) Select(games[library.SelectedIndex]); };
            Grid.SetRow(library, 2);
            side.Children.Add(library);

            var hint = Theme.Label("Add more games in games.json", 11, Theme.TextFaint, FontWeights.Normal);
            hint.Margin = new Thickness(26, 12, 24, 20);
            Grid.SetRow(hint, 3);
            side.Children.Add(hint);
            Grid.SetColumn(side, 0);
            root.Children.Add(side);

            // Main area
            var main = new Grid();
            main.RowDefinitions.Add(new RowDefinition { Height = new GridLength(44) });
            main.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            main.RowDefinitions.Add(new RowDefinition());
            main.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Grid.SetColumn(main, 1);
            root.Children.Add(main);

            var caption = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            caption.Children.Add(CaptionButton("", delegate { WindowState = WindowState.Minimized; }, false));
            caption.Children.Add(CaptionButton("", delegate { WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; }, false));
            caption.Children.Add(CaptionButton("", delegate { Close(); }, true));
            Grid.SetRow(caption, 0);
            main.Children.Add(caption);

            hero = BuildHero();
            Grid.SetRow(hero, 1);
            main.Children.Add(hero);

            var scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Hidden, Margin = new Thickness(36, 22, 36, 0) };   // scrolls with the mouse wheel
            var cards = new Grid();
            cards.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(3, GridUnitType.Star) });
            cards.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
            cards.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2, GridUnitType.Star) });
            aboutText = Theme.Label("", 13.5, Theme.TextDim, FontWeights.Normal);
            aboutText.LineHeight = 21;
            var about = CardWith("About", aboutText);
            Grid.SetColumn(about, 0);
            cards.Children.Add(about);
            controlsPanel = new StackPanel();
            var controls = CardWith("Controls", controlsPanel);
            Grid.SetColumn(controls, 2);
            cards.Children.Add(controls);
            scroll.Content = cards;
            Grid.SetRow(scroll, 2);
            main.Children.Add(scroll);

            var bottom = new Grid { Margin = new Thickness(36, 14, 36, 18) };
            bottom.ColumnDefinitions.Add(new ColumnDefinition());
            bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var statusRow = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            progress = new ProgressBar { Width = 120, Height = 4, IsIndeterminate = true, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 0, 12, 0), Foreground = Theme.Text, Background = Theme.CardBorder, BorderThickness = new Thickness(0) };
            statusRow.Children.Add(progress);
            statusText = Theme.Label("", 12.5, Theme.TextDim, FontWeights.Normal);
            statusText.TextWrapping = TextWrapping.NoWrap;
            statusText.TextTrimming = TextTrimming.CharacterEllipsis;
            statusRow.Children.Add(statusText);
            bottom.Children.Add(statusRow);
            footer = Theme.Label("No game data is included. Use your own legally obtained copy.", 11, Theme.TextFaint, FontWeights.Normal);
            footer.TextWrapping = TextWrapping.NoWrap;
            footer.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(footer, 1);
            bottom.Children.Add(footer);
            Grid.SetRow(bottom, 3);
            main.Children.Add(bottom);

            return root;
        }

        Grid BuildHero()
        {
            var g = new Grid { Margin = new Thickness(36, 6, 36, 0), Height = 236 };
            var banner = new Border { CornerRadius = new CornerRadius(16), ClipToBounds = true };
            g.Children.Add(banner);
            // Soft glow circles for some depth
            var glow = new Canvas { IsHitTestVisible = false, ClipToBounds = true };
            g.Children.Add(new Border { CornerRadius = new CornerRadius(16), ClipToBounds = true, Child = glow });

            var content = new Grid { Margin = new Thickness(34, 28, 34, 28) };
            content.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            content.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            content.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            content.RowDefinitions.Add(new RowDefinition());
            content.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var chip = new Border { Background = new SolidColorBrush(Color.FromArgb(0x40, 0, 0, 0)), CornerRadius = new CornerRadius(6), Padding = new Thickness(9, 3, 9, 4), HorizontalAlignment = HorizontalAlignment.Left };
            heroPlatform = Theme.Label("", 11, Brushes.White, FontWeights.SemiBold);
            chip.Child = heroPlatform;
            content.Children.Add(chip);

            heroTitle = Theme.Label("", 30, Brushes.White, FontWeights.Bold);
            heroTitle.Margin = new Thickness(0, 14, 0, 0);
            heroTitle.Effect = new DropShadowEffect { BlurRadius = 18, ShadowDepth = 0, Opacity = 0.35 };
            Grid.SetRow(heroTitle, 1);
            content.Children.Add(heroTitle);

            heroSubtitle = Theme.Label("", 14, new SolidColorBrush(Color.FromArgb(0xD0, 0xFF, 0xFF, 0xFF)), FontWeights.Normal);
            heroSubtitle.Margin = new Thickness(0, 4, 0, 0);
            Grid.SetRow(heroSubtitle, 2);
            content.Children.Add(heroSubtitle);

            var actions = new Grid();
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            actions.ColumnDefinitions.Add(new ColumnDefinition());
            playButton = Theme.MakeButton("▶  PLAY", Brushes.White, Brushes.Black, 15, new Thickness(34, 12, 34, 12), 10);
            playButton.Click += delegate { Play(); };
            var buttons = new StackPanel { Orientation = Orientation.Horizontal };
            buttons.Children.Add(playButton);
            romButton = Theme.MakeButton("Select ROM…", new SolidColorBrush(Color.FromArgb(0x3A, 0, 0, 0)), Brushes.White, 13, new Thickness(18, 12, 18, 12), 10);
            romButton.Margin = new Thickness(10, 0, 0, 0);
            romButton.Click += delegate { SelectRom(); };
            buttons.Children.Add(romButton);
            folderButton = Theme.MakeButton("Open folder", new SolidColorBrush(Color.FromArgb(0x3A, 0, 0, 0)), Brushes.White, 13, new Thickness(18, 12, 18, 12), 10);
            folderButton.Margin = new Thickness(10, 0, 0, 0);
            folderButton.Click += delegate { Process.Start("explorer.exe", "\"" + baseDir + "\""); };
            buttons.Children.Add(folderButton);
            actions.Children.Add(buttons);

            var badges = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
            romBadgeBox = Badge(out romBadge);
            buildBadgeBox = Badge(out buildBadge);
            buildBadgeBox.Margin = new Thickness(8, 0, 0, 0);
            badges.Children.Add(romBadgeBox);
            badges.Children.Add(buildBadgeBox);
            Grid.SetColumn(badges, 1);
            actions.Children.Add(badges);
            Grid.SetRow(actions, 4);
            content.Children.Add(actions);

            g.Children.Add(content);
            g.Tag = new object[] { banner, glow };
            return g;
        }

        Border Badge(out TextBlock text)
        {
            text = Theme.Label("", 11.5, Brushes.White, FontWeights.SemiBold);
            return new Border { Background = new SolidColorBrush(Color.FromArgb(0x48, 0, 0, 0)), CornerRadius = new CornerRadius(14), Padding = new Thickness(11, 5, 12, 6), Child = text };
        }

        Border CardWith(string title, UIElement body)
        {
            var panel = new StackPanel();
            var t = Theme.Label(title.ToUpperInvariant(), 10.5, Theme.TextFaint, FontWeights.SemiBold);
            t.Margin = new Thickness(0, 0, 0, 12);
            panel.Children.Add(t);
            panel.Children.Add(body);
            return new Border { Background = Theme.Card, BorderBrush = Theme.CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(14), Padding = new Thickness(22, 18, 22, 20), Child = panel, Margin = new Thickness(0, 0, 0, 16) };
        }

        Button CaptionButton(string glyph, RoutedEventHandler click, bool danger)
        {
            var b = Theme.MakeButton(glyph, Brushes.Transparent, Theme.TextDim, 10, new Thickness(0), 0);
            b.FontFamily = new FontFamily("Segoe MDL2 Assets, Segoe Fluent Icons");
            b.FontWeight = FontWeights.Normal;
            b.Width = 46; b.Height = 44;
            b.Click += click;
            b.MouseEnter += delegate { b.Background = danger ? Theme.Hex("#C42B1C") : Theme.Hex("#22252C"); b.Foreground = Brushes.White; };
            b.MouseLeave += delegate { b.Background = Brushes.Transparent; b.Foreground = Theme.TextDim; };
            WindowChrome.SetIsHitTestVisibleInChrome(b, true);
            return b;
        }

        Style LibraryItemStyle()
        {
            const string xaml =
                "<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='ListBoxItem'>" +
                "<Setter Property='Margin' Value='0,0,0,4'/><Setter Property='Cursor' Value='Hand'/>" +
                "<Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ListBoxItem'>" +
                "<Border x:Name='b' CornerRadius='10' Padding='10' Background='Transparent'><ContentPresenter/></Border>" +
                "<ControlTemplate.Triggers>" +
                "<Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Background' Value='#1D2027'/></Trigger>" +
                "<Trigger Property='IsSelected' Value='True'><Setter TargetName='b' Property='Background' Value='#252933'/></Trigger>" +
                "</ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>";
            return (Style)XamlReader.Parse(xaml);
        }

        UIElement LibraryItem(Game g)
        {
            var row = new Grid();
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(44) });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            var accent = Theme.ColorOf(g.Accent, "#7C5CFF");
            var tile = new Border
            {
                Width = 44, Height = 44, CornerRadius = new CornerRadius(10),
                Background = new LinearGradientBrush(accent, Darken(accent, 0.45), 45),
                Child = new TextBlock { Text = Initials(g.Title), Foreground = Brushes.White, FontWeight = FontWeights.Bold, FontSize = 15, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center }
            };
            row.Children.Add(tile);
            var text = new StackPanel { Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            var title = Theme.Label(g.Title, 13, Theme.Text, FontWeights.SemiBold);
            title.TextWrapping = TextWrapping.NoWrap;
            title.TextTrimming = TextTrimming.CharacterEllipsis;
            text.Children.Add(title);
            text.Children.Add(Theme.Label(g.Platform ?? "", 11.5, Theme.TextFaint, FontWeights.Normal));
            Grid.SetColumn(text, 1);
            row.Children.Add(text);
            return row;
        }

        static string Initials(string title)
        {
            var words = title.Split(new[] { ' ', ':', '&', '-' }, StringSplitOptions.RemoveEmptyEntries).Where(w => char.IsLetter(w[0])).ToArray();
            if (words.Length == 0) return "?";
            if (words.Length == 1) return words[0].Substring(0, Math.Min(2, words[0].Length)).ToUpperInvariant();
            return (words[0][0].ToString() + words[1][0]).ToUpperInvariant();
        }

        static Color Darken(Color c, double f)
        {
            return Color.FromRgb((byte)(c.R * (1 - f)), (byte)(c.G * (1 - f)), (byte)(c.B * (1 - f)));
        }

        // -------- selection and state

        void Select(Game g)
        {
            current = g;
            var parts = (object[])hero.Tag;
            var banner = (Border)parts[0];
            var glow = (Canvas)parts[1];
            var accent = Theme.ColorOf(g.Accent, "#7C5CFF");
            banner.Background = new LinearGradientBrush(new GradientStopCollection
            {
                new GradientStop(accent, 0), new GradientStop(Darken(accent, 0.55), 0.65), new GradientStop(Darken(accent, 0.8), 1)
            }, new Point(0, 0), new Point(1, 1));
            glow.Children.Clear();
            var circle = new System.Windows.Shapes.Ellipse { Width = 420, Height = 420, Fill = new RadialGradientBrush(Color.FromArgb(0x55, 0xFF, 0xFF, 0xFF), Color.FromArgb(0, 0xFF, 0xFF, 0xFF)) };
            Canvas.SetRight(circle, -120); Canvas.SetTop(circle, -170);
            glow.Children.Add(circle);

            heroPlatform.Text = ((g.Platform ?? "") + (g.Region != null ? "  ·  " + g.Region : "")).ToUpperInvariant();
            heroTitle.Text = g.Title;
            heroSubtitle.Text = g.Subtitle ?? "";
            aboutText.Inlines.Clear();
            if (!string.IsNullOrEmpty(g.Status))
            {
                aboutText.Inlines.Add(new System.Windows.Documents.Run(g.Status) { Foreground = Theme.Text, FontWeight = FontWeights.SemiBold });
                aboutText.Inlines.Add(new System.Windows.Documents.LineBreak());
                aboutText.Inlines.Add(new System.Windows.Documents.LineBreak());
            }
            aboutText.Inlines.Add(new System.Windows.Documents.Run(g.Description ?? ""));
            if (g.RomLabel != null)
            {
                aboutText.Inlines.Add(new System.Windows.Documents.LineBreak());
                aboutText.Inlines.Add(new System.Windows.Documents.LineBreak());
                aboutText.Inlines.Add(new System.Windows.Documents.Run("Supported version: ") { Foreground = Theme.TextFaint });
                aboutText.Inlines.Add(new System.Windows.Documents.Run(g.RomLabel));
            }

            controlsPanel.Children.Clear();
            foreach (var c in g.Controls)
            {
                var row = new Grid { Margin = new Thickness(0, 0, 0, 6) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                var key = new Border { Background = Theme.Hex("#23262E"), BorderBrush = Theme.CardBorder, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(6), Padding = new Thickness(8, 2, 8, 3), HorizontalAlignment = HorizontalAlignment.Left, Child = Theme.Label(c[0], 12, Theme.Text, FontWeights.SemiBold) };
                row.Children.Add(key);
                var action = Theme.Label(c[1], 12.5, Theme.TextDim, FontWeights.Normal);
                action.VerticalAlignment = VerticalAlignment.Center;
                Grid.SetColumn(action, 1);
                row.Children.Add(action);
                controlsPanel.Children.Add(row);
            }
            if (g.Controls.Count == 0) controlsPanel.Children.Add(Theme.Label("No controls listed.", 12.5, Theme.TextFaint, FontWeights.Normal));

            RefreshState();
        }

        enum RomState { Missing, Verified, Unverified, Wrong }

        RomState CheckRom(Game g, bool hashNow)
        {
            string target = Full(g.RomTarget);
            if (target == null || !File.Exists(target)) return RomState.Missing;
            if (string.IsNullOrEmpty(g.RomSha1)) return RomState.Verified;
            var fi = new FileInfo(target);
            string stamp = fi.Length + ":" + fi.LastWriteTimeUtc.Ticks;
            if (settings.Get("verified:" + g.Id) == stamp) return RomState.Verified;
            if (!hashNow) return RomState.Unverified;
            bool ok = string.Equals(Sha1(target), g.RomSha1, StringComparison.OrdinalIgnoreCase);
            if (ok) settings.Set("verified:" + g.Id, stamp);
            return ok ? RomState.Verified : RomState.Wrong;
        }

        bool IsBuilt(Game g) { return g.LaunchExe != null && File.Exists(Full(g.LaunchExe)); }

        void RefreshState()
        {
            if (current == null) return;
            var rom = CheckRom(current, false);
            if (rom == RomState.Unverified)
            {
                // Hash in the background once, then show the result
                SetBadge(romBadge, "•  Checking ROM…", Theme.TextDim);
                var g = current;
                Task.Run(() => CheckRom(g, true)).ContinueWith(t => Dispatcher.Invoke(new Action(() => { if (current == g) RefreshState(); })));
                return;
            }
            switch (rom)
            {
                case RomState.Verified: SetBadge(romBadge, "●  ROM verified", Theme.Good); break;
                case RomState.Wrong: SetBadge(romBadge, "●  Unsupported ROM", Theme.Bad); break;
                default: SetBadge(romBadge, "●  ROM required", Theme.Warn); break;
            }
            bool built = IsBuilt(current);
            SetBadge(buildBadge, built ? "●  Ready" : "●  Not built", built ? Theme.Good : Theme.Warn);

            playButton.IsEnabled = !busy && rom == RomState.Verified && built;
            romButton.IsEnabled = !busy;
            romButton.Content = rom == RomState.Verified ? "Change ROM…" : "Select ROM…";

            if (busy) return;
            if (rom != RomState.Verified) ShowStatus(rom == RomState.Wrong ? "The ROM in place does not match the supported version. Select the correct one." : "Select your own copy of the game to get started.", rom == RomState.Wrong ? Theme.Bad : Theme.TextDim);
            else if (!built) ShowStatus(current.BuildHint ?? "The game executable was not found.", Theme.Warn);
            else
            {
                string last = settings.Get("last:" + current.Id);
                ShowStatus(last ?? "Ready to play.", Theme.TextDim);
            }
        }

        void SetBadge(TextBlock t, string text, Brush color)
        {
            t.Inlines.Clear();
            var parts = text.Split(new[] { "  " }, 2, StringSplitOptions.None);
            if (parts.Length == 2)
            {
                t.Inlines.Add(new System.Windows.Documents.Run(parts[0] + "  ") { Foreground = color });
                t.Inlines.Add(new System.Windows.Documents.Run(parts[1]) { Foreground = Brushes.White });
            }
            else t.Text = text;
        }

        void ShowStatus(string text, Brush color)
        {
            statusText.Text = text;
            statusText.Foreground = color;
        }

        void SetBusy(bool b, string text)
        {
            busy = b;
            progress.Visibility = b ? Visibility.Visible : Visibility.Collapsed;
            if (text != null) ShowStatus(text, Theme.TextDim);
            library.IsEnabled = !b;
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
            var g = current;
            if (g == null || g.RomTarget == null) return;
            var dlg = new OpenFileDialog { Title = "Select your copy of " + g.Title, Filter = g.RomFilter + "|All files (*.*)|*.*" };
            if (dlg.ShowDialog(this) != true) return;
            string source = dlg.FileName, target = Full(g.RomTarget);

            SetBusy(true, "Checking ROM…");
            string error = null;
            try
            {
                string hash = await Task.Run(() => Sha1(source));
                if (!string.IsNullOrEmpty(g.RomSha1) && !string.Equals(hash, g.RomSha1, StringComparison.OrdinalIgnoreCase))
                    error = "This file is not the supported version (" + (g.RomLabel ?? "see games.json") + ").\nSHA-1: " + hash;
                else
                {
                    if (!string.Equals(Path.GetFullPath(source), target, StringComparison.OrdinalIgnoreCase))
                    {
                        ShowStatus("Copying ROM…", Theme.TextDim);
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        await Task.Run(() => File.Copy(source, target, true));
                    }
                    var fi = new FileInfo(target);
                    settings.Set("verified:" + g.Id, fi.Length + ":" + fi.LastWriteTimeUtc.Ticks);
                    error = await RunSetup(g, true);
                }
            }
            catch (Exception e) { error = e.Message; }
            SetBusy(false, null);
            if (error != null)
            {
                ShowStatus(error.Split('\n')[0], Theme.Bad);
                MessageBox.Show(this, error, "Recomp Launcher", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            else ShowStatus("ROM verified. Ready to play.", Theme.Good);
        }

        // Runs the one-time setup steps. Returns an error message or null.
        async Task<string> RunSetup(Game g, bool force)
        {
            foreach (var step in g.Setup)
            {
                // Already done (e.g. data extracted before). A new ROM of the same version yields the same data.
                if (step.SkipIfExists != null && File.Exists(Full(step.SkipIfExists))) continue;
                string exe = Full(step.Exe);
                if (exe == null || !File.Exists(exe))
                    return (step.Label ?? "Setup") + " needs " + step.Exe + ", which was not found.";
                ShowStatus((step.Label ?? "Preparing") + "…", Theme.TextDim);
                var psi = new ProcessStartInfo(exe, Expand(g, step.Args))
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
            var g = current;
            if (g == null || busy) return;
            SetBusy(true, "Preparing…");
            string error = await RunSetup(g, false);
            if (error != null)
            {
                SetBusy(false, null);
                ShowStatus(error.Split('\n')[0], Theme.Bad);
                MessageBox.Show(this, error, "Recomp Launcher", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            string exe = Full(g.LaunchExe);
            string logDir = Path.Combine(baseDir, "build", "logs");
            Directory.CreateDirectory(logDir);
            string logPath = Path.Combine(logDir, g.Id + ".log");
            var psi = new ProcessStartInfo(exe, Expand(g, g.LaunchArgs))
            {
                WorkingDirectory = Path.GetDirectoryName(exe), UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            // Relative arguments are meant relative to the launcher folder
            psi.Arguments = string.Join(" ", SplitArgs(Expand(g, g.LaunchArgs)).Select(a => Quote(MakeAbsoluteIfPath(a))));
            ShowStatus("Running " + g.Title + "…", Theme.Good);
            progress.Visibility = Visibility.Collapsed;
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
            string text = "Last session: " + DateTime.Now.ToString("HH:mm") + (summary != null ? " — " + summary : "") + "  (log: build\\logs\\" + g.Id + ".log)";
            settings.Set("last:" + g.Id, text);
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
        static void Main()
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
            app.Run(new LauncherWindow(baseDir));
        }
    }
}
