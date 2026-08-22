using System.ComponentModel;
using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace RobloxNativeLauncher;

public sealed class Form1 : Form
{
    private static readonly Regex PlaceIdPattern = new("^[0-9]{1,20}$", RegexOptions.Compiled);
    private static readonly Regex ShareCodePattern = new("^[A-Za-z0-9_-]{8,128}$", RegexOptions.Compiled);

    private readonly TextBox _placeIdTextBox = new();
    private readonly TextBox _privateLinkTextBox = new();
    private readonly Button _launchButton = new();
    private readonly RichTextBox _logTextBox = new();
    private readonly Label _statusLabel = new();

    private readonly Color _background = Color.FromArgb(17, 19, 24);
    private readonly Color _surface = Color.FromArgb(29, 33, 41);
    private readonly Color _surfaceBorder = Color.FromArgb(55, 62, 75);
    private readonly Color _primary = Color.FromArgb(37, 99, 235);
    private readonly Color _primaryHover = Color.FromArgb(29, 78, 216);
    private readonly Color _foreground = Color.FromArgb(241, 245, 249);
    private readonly Color _muted = Color.FromArgb(156, 163, 175);

    public Form1()
    {
        SuspendLayout();

        Text = "Roblox Native Launcher";
        ClientSize = new Size(570, 540);
        MinimumSize = MaximumSize = SizeFromClientSize(ClientSize);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        BackColor = _background;
        ForeColor = _foreground;
        Font = new Font("Segoe UI", 10F, FontStyle.Regular, GraphicsUnit.Point);
        AutoScaleMode = AutoScaleMode.Dpi;
        KeyPreview = true;

        BuildInterface();
        AcceptButton = _launchButton;
        Shown += OnFirstShown;
        FormClosed += (_, _) => _privateLinkTextBox.Clear();

        ResumeLayout(false);
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 17763))
        {
            int enabled = 1;
            _ = DwmSetWindowAttribute(Handle, 20, ref enabled, sizeof(int));
        }
    }

    private void BuildInterface()
    {
        var title = new Label
        {
            Text = "Roblox Native Launcher",
            Font = new Font("Segoe UI Semibold", 23F, FontStyle.Bold),
            ForeColor = _foreground,
            AutoSize = false,
            TextAlign = ContentAlignment.MiddleCenter,
            Bounds = new Rectangle(30, 22, 510, 48)
        };

        var subtitle = new Label
        {
            Text = "Inicialização direta, nativa e sem navegador",
            ForeColor = _muted,
            AutoSize = false,
            TextAlign = ContentAlignment.MiddleCenter,
            Bounds = new Rectangle(30, 68, 510, 28)
        };

        var placeLabel = CreateLabel("ID da Experiência (Place ID)", 42, 116, 486, 24);
        ConfigureTextBox(_placeIdTextBox, "Exemplo: 1818", 42, 145, 486, 42);
        _placeIdTextBox.AccessibleName = "ID da Experiência";

        var privateLabel = CreateLabel("Link de servidor privado (opcional)", 42, 204, 486, 24);
        ConfigureTextBox(
            _privateLinkTextBox,
            "https://www.roblox.com/share?code=...&type=Server",
            42, 233, 486, 42);
        _privateLinkTextBox.AccessibleName = "Link de servidor privado";

        var safetyPanel = new RoundedPanel
        {
            BackColor = Color.FromArgb(35, 30, 20),
            BorderColor = Color.FromArgb(120, 83, 25),
            CornerRadius = 10,
            Bounds = new Rectangle(42, 291, 486, 47)
        };
        safetyPanel.Controls.Add(new Label
        {
            Text = "Segurança: nunca cole .ROBLOSECURITY, cookies ou senhas.",
            ForeColor = Color.FromArgb(253, 186, 116),
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleCenter,
            Font = new Font("Segoe UI Semibold", 9.5F, FontStyle.Bold)
        });

        _launchButton.Text = "Iniciar Roblox";
        _launchButton.Bounds = new Rectangle(42, 355, 486, 54);
        _launchButton.FlatStyle = FlatStyle.Flat;
        _launchButton.FlatAppearance.BorderSize = 0;
        _launchButton.BackColor = _primary;
        _launchButton.ForeColor = Color.White;
        _launchButton.Cursor = Cursors.Hand;
        _launchButton.Font = new Font("Segoe UI Semibold", 12F, FontStyle.Bold);
        _launchButton.Click += LaunchButtonClick;
        _launchButton.MouseEnter += (_, _) => _launchButton.BackColor = _primaryHover;
        _launchButton.MouseLeave += (_, _) => _launchButton.BackColor = _primary;
        ApplyRoundedRegion(_launchButton, 12);

        _statusLabel.Text = "Pronto";
        _statusLabel.ForeColor = Color.FromArgb(74, 222, 128);
        _statusLabel.Bounds = new Rectangle(42, 422, 486, 24);
        _statusLabel.Font = new Font("Segoe UI Semibold", 9.5F, FontStyle.Bold);

        _logTextBox.Bounds = new Rectangle(42, 451, 486, 60);
        _logTextBox.ReadOnly = true;
        _logTextBox.BorderStyle = BorderStyle.FixedSingle;
        _logTextBox.BackColor = _surface;
        _logTextBox.ForeColor = _muted;
        _logTextBox.DetectUrls = false;
        _logTextBox.ScrollBars = RichTextBoxScrollBars.Vertical;
        _logTextBox.TabStop = false;
        _logTextBox.Font = new Font("Consolas", 9F, FontStyle.Regular);

        Controls.AddRange([
            title, subtitle, placeLabel, _placeIdTextBox,
            privateLabel, _privateLinkTextBox, safetyPanel,
            _launchButton, _statusLabel, _logTextBox
        ]);
    }

    private Label CreateLabel(string text, int x, int y, int width, int height) => new()
    {
        Text = text,
        ForeColor = _foreground,
        Font = new Font("Segoe UI Semibold", 10F, FontStyle.Bold),
        Bounds = new Rectangle(x, y, width, height)
    };

    private void ConfigureTextBox(TextBox textBox, string placeholder, int x, int y, int width, int height)
    {
        textBox.PlaceholderText = placeholder;
        textBox.Bounds = new Rectangle(x, y, width, height);
        textBox.BackColor = _surface;
        textBox.ForeColor = _foreground;
        textBox.BorderStyle = BorderStyle.FixedSingle;
        textBox.Font = new Font("Segoe UI", 11F, FontStyle.Regular);
        textBox.Margin = new Padding(10);
    }

    private void OnFirstShown(object? sender, EventArgs e)
    {
        _placeIdTextBox.Focus();
        WriteLog("Pronto. Nenhum navegador ou componente WebView foi carregado.");
    }

    private void LaunchButtonClick(object? sender, EventArgs e)
    {
        _launchButton.Enabled = false;
        try
        {
            LaunchTarget target = BuildLaunchTarget(
                _placeIdTextBox.Text.Trim(),
                _privateLinkTextBox.Text.Trim());

            SetStatus("Iniciando Roblox...", Color.FromArgb(96, 165, 250));
            WriteLog(target.Description);

            // Fire-and-forget: o Shell do Windows entrega o URI ao protocolo
            // registrado pelo Roblox. Não há acesso ao processo ou à memória do jogo.
            Process.Start(new ProcessStartInfo
            {
                FileName = target.Uri,
                UseShellExecute = true,
                ErrorDialog = false
            })?.Dispose();

            SetStatus("Comando enviado ao Roblox", Color.FromArgb(74, 222, 128));
            WriteLog("Protocolo entregue ao Windows com sucesso.");
        }
        catch (ArgumentException exception)
        {
            SetStatus("Verifique os dados informados", Color.FromArgb(251, 191, 36));
            WriteLog(exception.Message);
            MessageBox.Show(this, exception.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        catch (Win32Exception)
        {
            ShowRobloxNotInstalledError();
        }
        catch (Exception exception)
        {
            SetStatus("Não foi possível iniciar", Color.FromArgb(248, 113, 113));
            WriteLog($"Erro do Windows: {exception.Message}");
            MessageBox.Show(
                this,
                "O Windows não conseguiu entregar o protocolo ao Roblox Player. Repare ou reinstale o cliente oficial.",
                Text,
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
        finally
        {
            _launchButton.Enabled = true;
        }
    }

    private static LaunchTarget BuildLaunchTarget(string placeId, string privateLink)
    {
        if (!string.IsNullOrWhiteSpace(privateLink))
            return ParsePrivateServerLink(privateLink, placeId);

        ValidatePlaceId(placeId);
        string uri = $"roblox://placeId={Uri.EscapeDataString(placeId)}";
        return new LaunchTarget(uri, $"Solicitando a experiência {placeId}.");
    }

    private static LaunchTarget ParsePrivateServerLink(string input, string fallbackPlaceId)
    {
        if (!Uri.TryCreate(input, UriKind.Absolute, out Uri? link) ||
            link.Scheme != Uri.UriSchemeHttps ||
            !IsOfficialRobloxHost(link.Host))
        {
            throw new ArgumentException(
                "Use somente um link HTTPS oficial de roblox.com para servidores privados.");
        }

        Dictionary<string, string> query = ParseQuery(link.Query);

        // Formato atual: https://www.roblox.com/share?code=...&type=Server
        if (link.AbsolutePath.Equals("/share", StringComparison.OrdinalIgnoreCase))
        {
            if (!query.TryGetValue("code", out string? code) || !ShareCodePattern.IsMatch(code) ||
                !query.TryGetValue("type", out string? type) ||
                !type.Equals("Server", StringComparison.OrdinalIgnoreCase))
            {
                throw new ArgumentException("O link compartilhado de servidor privado é inválido ou incompleto.");
            }

            string uri = $"roblox://navigation/share_links?code={Uri.EscapeDataString(code)}&type=Server";
            return new LaunchTarget(uri, "Solicitando servidor privado pelo link oficial.");
        }

        // Formato legado: /games/PLACE_ID/...?...privateServerLinkCode=CODE
        string[] segments = link.AbsolutePath.Split('/', StringSplitOptions.RemoveEmptyEntries);
        string placeId = fallbackPlaceId;
        if (segments.Length >= 2 && segments[0].Equals("games", StringComparison.OrdinalIgnoreCase))
            placeId = segments[1];

        if (!query.TryGetValue("privateServerLinkCode", out string? linkCode) ||
            !ShareCodePattern.IsMatch(linkCode))
        {
            throw new ArgumentException(
                "O link não possui um código de servidor privado reconhecido pelo Roblox.");
        }

        ValidatePlaceId(placeId);
        string legacyUri =
            $"roblox://placeId={Uri.EscapeDataString(placeId)}&linkCode={Uri.EscapeDataString(linkCode)}";
        return new LaunchTarget(legacyUri, $"Solicitando servidor privado da experiência {placeId}.");
    }

    private static bool IsOfficialRobloxHost(string host) =>
        host.Equals("roblox.com", StringComparison.OrdinalIgnoreCase) ||
        host.EndsWith(".roblox.com", StringComparison.OrdinalIgnoreCase);

    private static Dictionary<string, string> ParseQuery(string query)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (string item in query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] pair = item.Split('=', 2);
            string key = Uri.UnescapeDataString(pair[0].Replace('+', ' '));
            string value = pair.Length == 2
                ? Uri.UnescapeDataString(pair[1].Replace('+', ' '))
                : string.Empty;
            values[key] = value;
        }
        return values;
    }

    private static void ValidatePlaceId(string placeId)
    {
        if (!PlaceIdPattern.IsMatch(placeId))
            throw new ArgumentException("Informe um Place ID válido contendo somente números (até 20 dígitos).");
    }

    private void ShowRobloxNotInstalledError()
    {
        SetStatus("Roblox Player não encontrado", Color.FromArgb(248, 113, 113));
        WriteLog("O protocolo roblox: não está registrado no Windows.");
        MessageBox.Show(
            this,
            "O protocolo oficial do Roblox não está registrado. Instale ou repare o Roblox Player oficial e tente novamente.",
            Text,
            MessageBoxButtons.OK,
            MessageBoxIcon.Error);
    }

    private void SetStatus(string text, Color color)
    {
        _statusLabel.Text = text;
        _statusLabel.ForeColor = color;
    }

    private void WriteLog(string message)
    {
        _logTextBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        _logTextBox.SelectionStart = _logTextBox.TextLength;
        _logTextBox.ScrollToCaret();
    }

    private static void ApplyRoundedRegion(Control control, int radius)
    {
        using var path = new GraphicsPath();
        int diameter = radius * 2;
        var arc = new Rectangle(0, 0, diameter, diameter);
        path.AddArc(arc, 180, 90);
        arc.X = control.Width - diameter;
        path.AddArc(arc, 270, 90);
        arc.Y = control.Height - diameter;
        path.AddArc(arc, 0, 90);
        arc.X = 0;
        path.AddArc(arc, 90, 90);
        path.CloseFigure();
        control.Region = new Region(path);
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

    private sealed record LaunchTarget(string Uri, string Description);

    private sealed class RoundedPanel : Panel
    {
        public Color BorderColor { get; init; } = Color.Transparent;
        public int CornerRadius { get; init; } = 8;

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using var path = CreatePath(ClientRectangle, CornerRadius);
            using var pen = new Pen(BorderColor, 1F);
            e.Graphics.DrawPath(pen, path);
        }

        protected override void OnResize(EventArgs eventargs)
        {
            base.OnResize(eventargs);
            using var path = CreatePath(ClientRectangle, CornerRadius);
            Region = new Region(path);
        }

        private static GraphicsPath CreatePath(Rectangle rectangle, int radius)
        {
            var path = new GraphicsPath();
            if (rectangle.Width <= 0 || rectangle.Height <= 0) return path;
            int diameter = Math.Min(radius * 2, Math.Min(rectangle.Width, rectangle.Height));
            var arc = new Rectangle(rectangle.X, rectangle.Y, diameter, diameter);
            path.AddArc(arc, 180, 90);
            arc.X = rectangle.Right - diameter - 1;
            path.AddArc(arc, 270, 90);
            arc.Y = rectangle.Bottom - diameter - 1;
            path.AddArc(arc, 0, 90);
            arc.X = rectangle.X;
            path.AddArc(arc, 90, 90);
            path.CloseFigure();
            return path;
        }
    }
}
