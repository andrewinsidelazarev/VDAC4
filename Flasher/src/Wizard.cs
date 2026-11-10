// SPDX-License-Identifier: Apache-2.0
// Окно «Первая прошивка VDAC4»: мастер по шагам. Начало (прошивки скачиваются с GitHub сразу при запуске;
// что будет записано, предупреждение о питании) → 1 ESP32-S3 Zero → 2 BL616 модуля Tang → 3 flash ПЛИС Tang →
// готово. На каждом шаге — что подключить, найдено ли устройство (проверка раз в секунду), кнопка «Прошить»,
// ход и журнал. Шаг сначала сохраняет прежнюю прошивку в папку Backup, потом стирает и пишет.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace Vdac4Flasher
{
    public class Wizard : Form
    {
        enum Mark { Pending, Current, Done, Skipped, Failed }

        const int Welcome = 0, StepEsp = 1, StepBl616 = 2, StepTang = 3, Finish = 4;
        static readonly Color Accent = Color.FromArgb(37, 99, 235), Good = Color.FromArgb(22, 128, 61),
                              Bad = Color.FromArgb(185, 28, 28), Muted = Color.FromArgb(100, 100, 110),
                              WarnBack = Color.FromArgb(254, 243, 199), WarnText = Color.FromArgb(120, 70, 0),
                              Rail = Color.FromArgb(243, 244, 246);

        readonly string folder;
        FirmwareSet fw;
        int page;
        readonly Mark[] marks = new Mark[5];
        readonly string[] results = new string[5];

        Label header, subheader, pageTitle, pageText, warn, device, stage, message;
        LinkLabel langLink, logLink;
        RailPanel rail;
        FlowLayoutPanel options;
        ProgressBar bar;
        Button main, skip, back, stop, fetchAgain;
        TextBox log;
        System.Windows.Forms.Timer timer;
        CheckBox tangSlot0, tangSlot1, tangSlot3, tangFonts;
        bool busy, detecting, found;
        UiReport report;

        // загрузка с GitHub: идёт / итог (null — ещё не было) / ошибка
        readonly IFetch fetch = new HttpFetch();
        bool downloading;
        DownloadResult downloaded;
        string downloadError;
        // бэкап прежних прошивок: папка Backup рядом с программой (создаётся при запуске, если её нет)
        readonly Backup backup = new Backup(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Backup"));

        readonly bool snapshot;

        public Wizard(string folder) : this(folder, false) { }

        public Wizard(string folder, bool snapshot)
        {
            this.snapshot = snapshot;
            this.folder = folder;
            fw = FirmwareSet.Load(folder);
            report = new UiReport(this);
            if (!snapshot) Shown += (s, e) => StartDownload();
            Font = new Font("Segoe UI", 10f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(980, 660);
            MinimumSize = new Size(880, 620);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.White;
            Build();
            ShowPage(Welcome);
            timer = new System.Windows.Forms.Timer { Interval = 1000 };
            timer.Tick += (s, e) => Detect();
            if (!snapshot) timer.Start();
        }

        /// <summary>Страница для картинки: прежние шаги — сделаны, устройство — найдено, запись — на середине.
        /// crowded — ещё и длинная ошибка и открытый журнал (проверка, что текст не наползает на полосу хода).</summary>
        public void ShowSnapshot(int p, bool crowded = false)
        {
            ShowSnapshotPage(p);
            if (!crowded) return;
            // подпись хода меняется на ходу: короткая, потом длиннее (10.11.2026 на плате «Стирание раздела
            // прошивки» обрезалась до «Стирание разде»)
            report.Progress(25, Lang.T("Бэкап", "Backup"));
            Application.DoEvents();
            report.Progress(26, Lang.T("Стирание раздела прошивки", "Erasing the firmware partition"));
            Application.DoEvents();
            message.ForeColor = Bad;
            message.Text = "✗  " + Lang.T(
                "ESP32-S3 пропал с USB (перезапустился или отошёл кабель): Присоединенное к системе устройство не работает. Повторите шаг.",
                "the ESP32-S3 dropped off USB (restarted or the cable came loose): A device attached to the system is not functioning. Retry the step.");
            log.Visible = true;
            ApplyLogLink();
            for (int i = 0; i < 12; i++) report.Log(Lang.T("строка журнала ", "log line ") + i);
        }

        void ShowSnapshotPage(int p)
        {
            for (int i = 0; i < p; i++) { marks[i] = Mark.Done; results[i] = Lang.T("записан и сверен", "written and verified"); }
            ShowPage(p);
            if (p >= StepEsp && p <= StepTang) {
                string[] dev = { "", Lang.T("Найден ESP32-S3: COM5", "ESP32-S3 found: COM5"),
                                 Lang.T("Найден BL616 в режиме прошивки: COM14", "BL616 in flashing mode found: COM14"),
                                 Lang.T("Найден USB-JTAG Tang: JTAG Debugger A", "Tang USB-JTAG found: JTAG Debugger A") };
                found = true;
                device.ForeColor = Good;
                device.Text = "●  " + dev[p];
                bar.Value = 64;
                stage.Text = Lang.T("Запись — 64 %", "Writing — 64 %");
                UpdateButtons();
            }
        }

        // ---- раскладка ------------------------------------------------------------------------

        void Build()
        {
            var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3 };
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 230));
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(root);

            var top = new Panel { Dock = DockStyle.Fill, Height = 72, BackColor = Accent, Padding = new Padding(18, 10, 18, 8) };
            header = new Label { AutoSize = true, ForeColor = Color.White, Font = new Font("Segoe UI Semibold", 17f), Location = new Point(18, 8) };
            subheader = new Label { AutoSize = true, ForeColor = Color.FromArgb(219, 234, 254), Location = new Point(21, 44) };
            langLink = new LinkLabel { AutoSize = true, LinkColor = Color.White, ActiveLinkColor = Color.White,
                                       Anchor = AnchorStyles.Top | AnchorStyles.Right };
            langLink.LinkClicked += (s, e) => { if (busy) return; Lang.Russian = !Lang.Russian; fw = FirmwareSet.Load(folder); ShowPage(page); };
            top.Controls.Add(header);
            top.Controls.Add(subheader);
            top.Controls.Add(langLink);
            top.Resize += (s, e) => langLink.Location = new Point(top.Width - langLink.Width - 18, 14);
            root.Controls.Add(top, 0, 0);
            root.SetColumnSpan(top, 2);

            rail = new RailPanel { Dock = DockStyle.Fill, BackColor = Rail, Marks = marks };
            rail.Font = new Font("Segoe UI", 10.5f);
            root.Controls.Add(rail, 0, 1);

            // Страница — две части (10.11.2026: при открытом журнале строка «Найден …» и сообщение наползали на
            // полосу хода — строки таблицы не помещались, а Label не обрезается своей ячейкой). Сверху — текст шага
            // (заголовок, инструкция, предупреждение, выбор слотов): не хватает места — он прокручивается. Снизу —
            // то, что должно быть видно всегда: устройство, сообщение, полоса хода и кнопки.
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Padding = new Padding(26, 18, 26, 10) };
            body.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            var content = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true, Margin = new Padding(0) };
            for (int i = 0; i < 4; i++) content.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            content.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            pageTitle = new Label { AutoSize = true, Font = new Font("Segoe UI Semibold", 15f), Margin = new Padding(0, 0, 0, 8) };
            pageText = new Label { AutoSize = true, Font = new Font("Segoe UI", 11f), MaximumSize = new Size(680, 0), Margin = new Padding(0, 0, 0, 10) };
            warn = new Label { AutoSize = true, BackColor = WarnBack, ForeColor = WarnText, Padding = new Padding(12, 9, 12, 9),
                               MaximumSize = new Size(680, 0), Margin = new Padding(0, 0, 0, 10), Font = new Font("Segoe UI", 10f) };
            options = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = new Padding(0, 0, 0, 6) };
            content.Controls.Add(pageTitle, 0, 0);
            content.Controls.Add(pageText, 0, 1);
            content.Controls.Add(warn, 0, 2);
            content.Controls.Add(options, 0, 3);
            body.Controls.Add(content, 0, 0);

            // прокрутка — только по вертикали: ширину текста задаёт окно (ниже), вбок прокручивать нечего
            content.AutoScroll = false;
            content.HorizontalScroll.Enabled = false;
            content.HorizontalScroll.Visible = false;
            content.HorizontalScroll.Maximum = 0;
            content.AutoScroll = true;

            var status = new TableLayoutPanel { AutoSize = true, ColumnCount = 1, RowCount = 5, Dock = DockStyle.Fill, Margin = new Padding(0, 6, 0, 0) };
            status.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            for (int i = 0; i < 5; i++) status.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            device = new Label { AutoSize = true, MaximumSize = new Size(680, 0), Font = new Font("Segoe UI Semibold", 10.5f), Margin = new Padding(0, 0, 0, 4) };
            message = new Label { AutoSize = true, MaximumSize = new Size(680, 0), Font = new Font("Segoe UI", 10.5f), Margin = new Padding(0, 2, 0, 6) };
            bar = new ProgressBar { Width = 660, Height = 18, Minimum = 0, Maximum = 100, Margin = new Padding(0, 4, 0, 2) };
            // Подпись хода — постоянной ширины во всю строку, а не по тексту (10.11.2026 на плате «Стирание раздела
            // прошивки» показалось как «Стирание разде»: ширина по тексту не успевала за частой сменой текста)
            stage = new Label { AutoSize = false, Anchor = AnchorStyles.Left | AnchorStyles.Right, ForeColor = Muted,
                                AutoEllipsis = true, Margin = new Padding(0, 0, 0, 2), Font = new Font("Segoe UI", 10f) };
            stage.Height = stage.PreferredHeight + 4;      // шрифт задан явно: высота — по нему, а не по шрифту по умолчанию
            status.Controls.Add(device, 0, 0);
            status.Controls.Add(message, 0, 1);
            status.Controls.Add(bar, 0, 2);
            status.Controls.Add(stage, 0, 3);
            body.Controls.Add(status, 0, 1);
            // ширина текста — по окну (узкое окно — переносы, а не обрезка), не шире 680
            content.Resize += (s, e) => {
                int w = Math.Max(300, Math.Min(680, content.ClientSize.Width - SystemInformation.VerticalScrollBarWidth - 4));
                pageText.MaximumSize = warn.MaximumSize = message.MaximumSize = device.MaximumSize = new Size(w, 0);
                bar.Width = Math.Min(660, w);
            };

            var buttons = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0, 10, 0, 0) };
            main = MakeButton(true);
            skip = MakeButton(false);
            back = MakeButton(false);
            stop = MakeButton(false);
            fetchAgain = MakeButton(false);
            main.Click += (s, e) => OnMain();
            skip.Click += (s, e) => { marks[page] = Mark.Skipped; results[page] = Lang.T("пропущен", "skipped"); ShowPage(page + 1); };
            back.Click += (s, e) => { if (page > Welcome) ShowPage(page - 1); };
            stop.Click += (s, e) => report.Cancel();
            fetchAgain.Click += (s, e) => StartDownload();
            buttons.Controls.Add(main);
            buttons.Controls.Add(fetchAgain);
            buttons.Controls.Add(skip);
            buttons.Controls.Add(back);
            buttons.Controls.Add(stop);
            status.Controls.Add(buttons, 0, 4);
            root.Controls.Add(body, 1, 1);

            var foot = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, Padding = new Padding(12, 2, 12, 8) };
            logLink = new LinkLabel { AutoSize = true };
            logLink.LinkClicked += (s, e) => { log.Visible = !log.Visible; ApplyLogLink(); };
            log = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Height = 150, Dock = DockStyle.Fill,
                                Font = new Font("Consolas", 9f), Visible = false, BackColor = Color.FromArgb(250, 250, 250) };
            foot.Controls.Add(logLink);
            foot.Controls.Add(log);
            root.Controls.Add(foot, 0, 2);
            root.SetColumnSpan(foot, 2);
        }

        static Button MakeButton(bool primary)
        {
            var b = new Button { AutoSize = true, MinimumSize = new Size(130, 38), Padding = new Padding(10, 2, 10, 2),
                                 Margin = new Padding(0, 0, 10, 0), FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI Semibold", 10.5f) };
            if (primary) {
                b.BackColor = Accent; b.ForeColor = Color.White; b.FlatAppearance.BorderSize = 0;
            } else {
                b.BackColor = Color.White; b.ForeColor = Color.FromArgb(30, 30, 40);
                b.FlatAppearance.BorderColor = Color.FromArgb(200, 200, 210);
            }
            return b;
        }

        void ApplyLogLink() { logLink.Text = log.Visible ? Lang.T("▾ Скрыть журнал", "▾ Hide log") : Lang.T("▸ Показать журнал", "▸ Show log"); }

        // ---- страницы -------------------------------------------------------------------------

        void ShowPage(int p)
        {
            page = p;
            for (int i = 0; i < marks.Length; i++)
                if (marks[i] == Mark.Current) marks[i] = Mark.Pending;
            if (marks[p] == Mark.Pending) marks[p] = Mark.Current;
            Text = Lang.T("Первая прошивка VDAC4", "VDAC4 first flashing");
            header.Text = Lang.T("Первая прошивка VDAC4", "VDAC4 first flashing");
            subheader.Text = Lang.T("ESP32-S3 Zero, BL616 и ПЛИС модуля Tang Nano 20K — без Python и сторонних программ",
                                    "ESP32-S3 Zero, BL616 and the Tang Nano 20K FPGA — no Python, no third-party tools");
            langLink.Text = Lang.T("English", "Русский");
            langLink.Location = new Point(langLink.Parent.ClientSize.Width - langLink.PreferredWidth - 18, 14);
            rail.Titles = new[] {
                Lang.T("Начало", "Start"), "ESP32-S3 Zero", Lang.T("BL616 модуля Tang", "Tang BL616"),
                Lang.T("ПЛИС Tang", "Tang FPGA"), Lang.T("Готово", "Done")
            };
            rail.Invalidate();
            ApplyLogLink();
            options.Controls.Clear();
            message.Text = "";
            warn.Visible = false;
            device.Text = "";
            found = false;
            if (!downloading) {
                bar.Value = 0;
                stage.Text = "";
            }
            bar.Visible = stage.Visible = (p >= StepEsp && p <= StepTang) || (p == Welcome && downloading);
            switch (p) {
            case Welcome: PageWelcome(); break;
            case StepEsp: PageEsp(); break;
            case StepBl616: PageBl616(); break;
            case StepTang: PageTang(); break;
            default: PageFinish(); break;
            }
            UpdateButtons();
            Detect();
        }

        void PageWelcome()
        {
            pageTitle.Text = Lang.T("Что будет записано", "What will be written");
            var sb = new StringBuilder();
            sb.AppendLine(Lang.T("Программа по шагам прошьёт три части платы VDAC4. Каждый шаг можно пропустить.",
                                 "The wizard flashes the three parts of the VDAC4 board step by step. Any step can be skipped."));
            sb.AppendLine(Lang.T("Перед записью шаг сохраняет прежнюю прошивку в папку Backup рядом с программой, затем стирает то, что запишет.",
                                 "Before writing, each step saves the old firmware into the Backup folder next to the program, then erases what it writes."));
            sb.AppendLine();
            if (downloading)
                sb.AppendLine(Lang.T("Скачиваю прошивки с GitHub (", "Downloading the firmware from GitHub (") + Github.Repo + ")…");
            else if (fw.Commit.Length > 0)
                sb.AppendLine(Lang.T("Прошивки с GitHub: ", "Firmware from GitHub: ") + Github.Repo + ", " +
                              (fw.Commit.StartsWith("refs/") ? fw.Commit : Lang.T("коммит ", "commit ") + fw.Commit.Substring(0, Math.Min(7, fw.Commit.Length))) +
                              (fw.Downloaded.Length > 0 ? Lang.T(", скачаны ", ", downloaded ") + fw.Downloaded : ""));
            else
                sb.AppendLine(Lang.T("Прошивки — из папки firmware рядом с программой (с GitHub не скачивались).",
                                     "Firmware from the firmware folder next to the program (not downloaded from GitHub)."));
            sb.AppendLine();
            sb.AppendLine(Line("1. ESP32-S3 Zero", fw.Esp, fw.EspVersion));
            sb.AppendLine(Line(Lang.T("2. BL616 модуля Tang", "2. Tang BL616"), fw.Bl616, fw.Bl616Version));
            var tang = new StringBuilder();
            long bytes = 0;
            foreach (var t in fw.Tang) bytes += t.Data.Length;
            tang.Append(Lang.T("3. ПЛИС Tang Nano 20K", "3. Tang Nano 20K FPGA"));
            if (fw.TangVersion.Length > 0) tang.Append(" " + fw.TangVersion);
            int nt = fw.Tang.Count;
            string images = Lang.Russian ? (nt % 10 == 1 && nt % 100 != 11 ? " образ, " : nt % 10 >= 2 && nt % 10 <= 4 && (nt % 100 < 12 || nt % 100 > 14) ? " образа, " : " образов, ")
                                         : (nt == 1 ? " image, " : " images, ");
            tang.Append(nt == 0 ? Lang.T(" — нет файлов", " — no files") : " — " + nt + images + Util.Size(bytes));
            sb.AppendLine(tang.ToString());
            pageText.Text = sb.ToString();
            warn.Visible = true;
            warn.Text = Lang.T(
                "Внимание: плату VDAC4 выньте из ZX Evolution или выключите Evo. USB к модулям подключайте только к обесточенной плате — " +
                "питание Evo и USB одновременно включать нельзя. Нужен USB-кабель с данными (не только зарядный).",
                "Warning: take the VDAC4 board out of the ZX Evolution or switch the Evo off. Plug USB into the modules only while the board " +
                "is unpowered — never run Evo power and USB at the same time. Use a USB cable with data lines (not a charge-only one).");
            var msg = new StringBuilder();
            if (!downloading && downloadError != null) {
                msg.AppendLine(Lang.T("Не удалось скачать прошивки с GitHub: ", "Could not download the firmware from GitHub: ") + downloadError);
                msg.AppendLine(HasAnyImage()
                    ? Lang.T("Прежние файлы в папке firmware целы — прошить можно ими, или нажмите «Скачать снова».",
                             "The previous files in the firmware folder are intact — you can flash them or press “Download again”.")
                    : Lang.T("Проверьте интернет и нажмите «Скачать снова».", "Check the internet connection and press “Download again”."));
            }
            if (!downloading && fw.Problems.Count > 0) {
                msg.AppendLine(Lang.T("Не всё в папке firmware в порядке:", "Problems in the firmware folder:"));
                msg.AppendLine("  • " + string.Join(Environment.NewLine + "  • ", fw.Problems));
                msg.AppendLine(Lang.T("Папка: ", "Folder: ") + folder);
            }
            if (msg.Length > 0) {
                message.ForeColor = Bad;
                message.Text = msg.ToString().TrimEnd();
            } else if (!downloading && downloaded != null) {
                message.ForeColor = Good;
                message.Text = "✓  " + string.Format(downloaded.GitChecked
                    ? Lang.T("Скачано с GitHub: {0} файлов, {1}; каждый файл сверен с git.",
                             "Downloaded from GitHub: {0} files, {1}; every file checked against git.")
                    : Lang.T("Скачано с GitHub: {0} файлов, {1}; API GitHub не ответил — сверены SHA-256 образа ESP и форматы.",
                             "Downloaded from GitHub: {0} files, {1}; the GitHub API did not answer — ESP SHA-256 and formats checked."),
                    downloaded.Files, Util.Size(downloaded.Bytes));
            }
        }

        bool HasAnyImage() { return fw.Esp != null || fw.Bl616 != null || fw.Tang.Count > 0; }

        // ---- загрузка с GitHub ---------------------------------------------------------------------

        void StartDownload()
        {
            if (busy) return;
            busy = downloading = true;
            downloadError = null;
            report.Reset();
            ShowPage(Welcome);
            report.Log("==== " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " GitHub " + Github.Repo);
            var th = new Thread(() => {
                DownloadResult r = null;
                string err = null;
                try {
                    r = Downloader.Download(fetch, folder, report);
                } catch (OperationCanceledException) {
                    err = Lang.T("загрузка остановлена", "download stopped");
                } catch (FlashException e) {
                    err = e.Message;
                } catch (Exception e) {
                    err = e.GetType().Name + ": " + e.Message;
                }
                try {
                    BeginInvoke((Action)(() => {
                        busy = downloading = false;
                        downloaded = r;
                        downloadError = err;
                        report.Log(err == null ? "OK" : Lang.T("ОШИБКА: ", "ERROR: ") + err);
                        fw = FirmwareSet.Load(folder);
                        if (page == Welcome) ShowPage(Welcome); else UpdateButtons();
                    }));
                } catch (InvalidOperationException) { }
            });
            th.IsBackground = true;
            th.Start();
        }

        static string Line(string name, FlashPart part, string version)
        {
            if (part == null) return name + Lang.T(" — нет файла", " — no file");
            return name + (version.Length > 0 ? " " + version : "") + " — " + Path.GetFileName(part.File) + ", " +
                   Util.Size(part.Data.Length) + Lang.T(", с адреса 0x", ", at 0x") + part.Address.ToString("X");
        }

        void PageEsp()
        {
            pageTitle.Text = Lang.T("Шаг 1. ESP32-S3 Zero", "Step 1. ESP32-S3 Zero");
            pageText.Text = Lang.T(
                "1. Подключите USB-кабель к разъёму USB-C модуля ESP32-S3 Zero (маленький модуль платы VDAC4).\n" +
                "2. Программа сама найдёт порт и переведёт модуль в режим прошивки — нажмите «Прошить».\n" +
                "Если модуль не отвечает: отключите кабель, нажмите и держите кнопку BOOT на модуле, подключите кабель и отпустите кнопку.\n" +
                "Полный образ сбрасывает настройки модуля, в том числе Wi-Fi.",
                "1. Plug a USB cable into the USB-C socket of the ESP32-S3 Zero (the small module on the VDAC4 board).\n" +
                "2. The program finds the port and puts the module into flashing mode — press “Flash”.\n" +
                "If the module does not answer: unplug the cable, hold the BOOT button on the module, plug the cable in and release the button.\n" +
                "The full image resets the module settings, Wi-Fi too.");
            warn.Visible = true;
            warn.Text = Lang.T(
                "Сначала программа читает всю память модуля (8 МБ: прошивка, настройки, файлы) в папку Backup — " +
                "это несколько минут. Потом стирает память целиком и записывает новую прошивку.",
                "First the program reads the whole module memory (8 MB: firmware, settings, files) into the Backup folder — " +
                "this takes a few minutes. Then it erases the whole memory and writes the new firmware.");
            if (fw.Esp == null) { message.ForeColor = Bad; message.Text = Lang.T("Нет образа ESP в папке firmware.", "No ESP image in the firmware folder."); }
        }

        void PageBl616()
        {
            pageTitle.Text = Lang.T("Шаг 2. BL616 модуля Tang Nano 20K", "Step 2. Tang Nano 20K BL616");
            pageText.Text = Lang.T(
                "1. Отключите USB от ESP32-S3.\n" +
                "2. Нажмите и держите кнопку UPDATE на модуле Tang Nano 20K, подключите USB-кабель к разъёму USB-C модуля Tang и отпустите кнопку.\n" +
                "3. Когда программа найдёт BL616 в режиме прошивки (COM-порт), нажмите «Прошить».\n" +
                "Заводская прошивка Sipeed (USB-JTAG для ПЛИС) в начале памяти BL616 остаётся — она нужна для шага 3.",
                "1. Unplug USB from the ESP32-S3.\n" +
                "2. Hold the UPDATE button on the Tang Nano 20K, plug a USB cable into the Tang USB-C socket and release the button.\n" +
                "3. When the program finds the BL616 in flashing mode (a COM port), press “Flash”.\n" +
                "The Sipeed factory firmware (USB-JTAG for the FPGA) at the start of the BL616 memory stays — step 3 needs it.");
            warn.Visible = true;
            warn.Text = Lang.T(
                "Перед записью программа сохраняет раздел прошивки VDAC4 (0x40000…0x7FFFF, 256 КБ) в папку Backup и стирает его целиком. " +
                "Кэш ядер за ним не трогается.",
                "Before writing, the program saves the VDAC4 firmware partition (0x40000…0x7FFFF, 256 KB) into the Backup folder and erases it whole. " +
                "The core cache after it stays.");
            if (fw.Bl616 == null) { message.ForeColor = Bad; message.Text = Lang.T("Нет образа BL616 в папке firmware.", "No BL616 image in the firmware folder."); }
        }

        void PageTang()
        {
            pageTitle.Text = Lang.T("Шаг 3. ПЛИС Tang Nano 20K", "Step 3. Tang Nano 20K FPGA");
            pageText.Text = Lang.T(
                "1. Отключите USB от модуля Tang и подключите его снова — уже без кнопки.\n" +
                "2. Когда программа найдёт USB-JTAG, нажмите «Прошить». Запись и сверка — около минуты; ПЛИС затем загрузится с новой flash.\n" +
                "Настройки Tang в конце его flash не стираются.",
                "1. Unplug the Tang USB and plug it in again — this time without the button.\n" +
                "2. When the program finds the USB-JTAG, press “Flash”. Writing and the read-back check take about a minute; the FPGA then starts from the new flash.\n" +
                "Tang settings at the end of its flash are kept.");
            warn.Visible = true;
            warn.Text = Lang.T(
                "Перед записью программа сохраняет отмеченные слоты flash целиком (по 1 МБ) в папку Backup и стирает их. Прочие слоты не трогаются.",
                "Before writing, the program saves the checked flash slots whole (1 MB each) into the Backup folder and erases them. Other slots stay.");
            tangSlot0 = tangSlot1 = tangSlot3 = tangFonts = null;
            foreach (var t in fw.Tang) {
                var cb = new CheckBox { AutoSize = true, Checked = true, Tag = t,
                                        Text = t.Name + " — " + Path.GetFileName(t.File) + ", " + Util.Size(t.Data.Length) + ", 0x" + t.Address.ToString("X6") };
                if (t.Address == 0) {
                    tangSlot0 = cb;
                    uint? j = FirmwareSet.JumpAddress(t.Data);
                    cb.Text += Lang.T(" (TS-Config, при включении", " (TS-Config, at power-up") + (j.HasValue ? Lang.T("; переход в 0x", "; jumps to 0x") + j.Value.ToString("X6") : "") + ")";
                } else if (t.Address == FirmwareSet.TangSlotBytes) {
                    tangSlot1 = cb;
                    cb.Text += Lang.T(" (ядро FT812)", " (FT812 core)");
                } else if (t.Address == 3 * FirmwareSet.TangSlotBytes) {
                    tangSlot3 = cb;
                    cb.Text += Lang.T(" (ядро видео)", " (video core)");
                } else {
                    tangFonts = cb;
                }
                options.Controls.Add(cb);
            }
            if (fw.Tang.Count == 0) { message.ForeColor = Bad; message.Text = Lang.T("Нет образов ПЛИС в папке firmware.", "No FPGA images in the firmware folder."); }
        }

        void PageFinish()
        {
            pageTitle.Text = Lang.T("Готово", "Done");
            var sb = new StringBuilder();
            string[] names = { "", "ESP32-S3 Zero", Lang.T("BL616 модуля Tang", "Tang BL616"), Lang.T("ПЛИС Tang", "Tang FPGA") };
            for (int i = StepEsp; i <= StepTang; i++)
                sb.AppendLine(names[i] + ": " + (results[i] ?? Lang.T("не прошивался", "not flashed")));
            sb.AppendLine();
            if (backup.Files.Count > 0) {
                sb.AppendLine(Lang.T("Прежние прошивки сохранены в папке ", "The old firmware is saved in the folder ") + backup.Folder +
                              Lang.T(" (что где — в backup.txt):", " (see backup.txt there):"));
                foreach (string f in backup.Files) sb.AppendLine("  " + Path.GetFileName(f));
                sb.AppendLine();
            }
            sb.AppendLine(Lang.T(
                "Отключите USB от модулей и только потом вставьте плату VDAC4 в ZX Evolution и включите Evo. " +
                "Дальнейшие обновления идут по сети, из Wild Commander (USB для них не нужен).",
                "Unplug USB from the modules and only then put the VDAC4 board into the ZX Evolution and switch the Evo on. " +
                "Further updates go over the network from Wild Commander (no USB needed)."));
            pageText.Text = sb.ToString();
            marks[Finish] = Mark.Done;
        }

        void UpdateButtons()
        {
            bool step = page >= StepEsp && page <= StepTang;
            main.Text = page == Welcome ? Lang.T("Начать  →", "Start  →") :
                        page == Finish ? Lang.T("Закрыть", "Close") :
                        marks[page] == Mark.Done ? Lang.T("Далее  →", "Next  →") :
                        marks[page] == Mark.Failed ? Lang.T("Повторить", "Retry") : Lang.T("Прошить", "Flash");
            skip.Text = Lang.T("Пропустить шаг", "Skip step");
            back.Text = Lang.T("←  Назад", "←  Back");
            stop.Text = Lang.T("Стоп", "Stop");
            fetchAgain.Text = Lang.T("Скачать снова", "Download again");
            skip.Visible = step && marks[page] != Mark.Done;
            back.Visible = page > Welcome && page < Finish;
            stop.Visible = busy;
            fetchAgain.Visible = page == Welcome && !snapshot;
            bool ready = page == Welcome ? HasAnyImage() : !step || marks[page] == Mark.Done || (found && HasImage(page));
            main.Enabled = !busy && ready;
            skip.Enabled = back.Enabled = fetchAgain.Enabled = !busy;
            langLink.Enabled = !busy;
        }

        bool HasImage(int p)
        {
            if (p == StepEsp) return fw.Esp != null;
            if (p == StepBl616) return fw.Bl616 != null;
            if (p == StepTang) return SelectedTang().Count > 0;
            return true;
        }

        List<FlashPart> SelectedTang()
        {
            var list = new List<FlashPart>();
            foreach (Control c in options.Controls) {
                var cb = c as CheckBox;
                if (cb != null && cb.Checked && cb.Tag is FlashPart) list.Add((FlashPart)cb.Tag);
            }
            return list;
        }

        void OnMain()
        {
            if (page == Welcome) { ShowPage(StepEsp); return; }
            if (page == Finish) { Close(); return; }
            if (marks[page] == Mark.Done) { ShowPage(page + 1); return; }
            StartFlash();
        }

        // ---- поиск устройства -------------------------------------------------------------------

        void Detect()
        {
            if (snapshot || busy || detecting || page < StepEsp || page > StepTang) return;
            detecting = true;
            int p = page;
            ThreadPool.QueueUserWorkItem(_ => {
                string text;
                bool ok;
                try { ok = Probe(p, out text); } catch (Exception e) { ok = false; text = e.Message; }
                try {
                    BeginInvoke((Action)(() => {
                        detecting = false;
                        if (p != page || busy) return;
                        found = ok;
                        device.ForeColor = ok ? Good : Muted;
                        device.Text = (ok ? "●  " : "○  ") + text;
                        UpdateButtons();
                    }));
                } catch (InvalidOperationException) { }
            });
        }

        static bool Probe(int p, out string text)
        {
            if (p == StepEsp) {
                var port = Ports.FindEsp();
                text = port != null ? Lang.T("Найден ESP32-S3: ", "ESP32-S3 found: ") + port.Com
                                    : Lang.T("ESP32-S3 не найден — подключите USB к модулю", "No ESP32-S3 yet — plug USB into the module");
                return port != null;
            }
            if (p == StepBl616) {
                var port = Ports.FindBl616();
                text = port != null ? Lang.T("Найден BL616 в режиме прошивки: ", "BL616 in flashing mode found: ") + port.Com
                                    : Lang.T("BL616 в режиме прошивки не найден — подключите USB к Tang с нажатой UPDATE",
                                             "No BL616 in flashing mode — plug the Tang USB in with UPDATE held");
                return port != null;
            }
            string why;
            string d = Ftd2xx.Present(out why);
            if (d != null) { text = Lang.T("Найден USB-JTAG Tang: ", "Tang USB-JTAG found: ") + d; return true; }
            text = why == "ftd2xx.dll"
                ? Lang.T("Нет драйвера FTDI: подключите Tang — Windows поставит его сама (нужен интернет) — или установите CDM driver с ftdichip.com",
                         "No FTDI driver: plug the Tang in — Windows installs it by itself (internet needed) — or install the CDM driver from ftdichip.com")
                : Lang.T("USB-JTAG Tang не найден — подключите USB к Tang без кнопки", "No Tang USB-JTAG yet — plug the Tang USB in without the button");
            return false;
        }

        // ---- прошивка -------------------------------------------------------------------------

        void StartFlash()
        {
            int p = page;
            var tang = SelectedTang();
            busy = true;
            report.Reset();
            message.Text = "";
            bar.Value = 0;
            stage.Text = "";
            foreach (Control c in options.Controls) c.Enabled = false;
            UpdateButtons();
            report.Log("==== " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " " + pageTitle.Text);
            var th = new Thread(() => {
                string err = null;
                try {
                    if (p == StepEsp) FlashEsp();
                    else if (p == StepBl616) FlashBl616();
                    else FlashTang(tang);
                } catch (OperationCanceledException) {
                    err = Lang.T("Остановлено. Повторите шаг — прошивка начнётся заново.", "Stopped. Retry the step — flashing starts over.");
                } catch (FlashException e) {
                    err = e.Message;
                } catch (TimeoutException) {
                    err = Lang.T("Устройство перестало отвечать. Проверьте кабель и повторите.", "The device stopped answering. Check the cable and retry.");
                } catch (Exception e) {
                    err = e.GetType().Name + ": " + e.Message;
                }
                try { BeginInvoke((Action)(() => Finished(p, err))); } catch (InvalidOperationException) { }
            });
            th.IsBackground = true;
            th.Start();
        }

        void Finished(int p, string err)
        {
            busy = false;
            foreach (Control c in options.Controls) c.Enabled = true;
            if (err == null) {
                marks[p] = Mark.Done;
                results[p] = Lang.T("записан и сверен", "written and verified");
                message.ForeColor = Good;
                message.Text = "✓  " + (p == StepEsp ? Lang.T("ESP32-S3 записан и сверен, модуль перезапущен.", "ESP32-S3 written and verified, the module restarted.")
                                      : p == StepBl616 ? Lang.T("BL616 записан и сверен. Отключите USB от Tang — дальше шаг 3.", "BL616 written and verified. Unplug the Tang USB — step 3 is next.")
                                      : Lang.T("ПЛИС Tang записана, сверена и загрузилась с новой flash.", "Tang FPGA written, verified and started from the new flash."));
                report.Log("OK");
            } else {
                marks[p] = Mark.Failed;
                results[p] = Lang.T("ошибка", "error");
                message.ForeColor = Bad;
                message.Text = "✗  " + err;
                report.Log(Lang.T("ОШИБКА: ", "ERROR: ") + err);
            }
            rail.Invalidate();
            UpdateButtons();
            Detect();
        }

        void FlashEsp()
        {
            var port = Ports.FindEsp();
            if (port == null) throw new FlashException(Lang.T("ESP32-S3 не найден", "ESP32-S3 not found"));
            report.Log(Lang.T("Порт ", "Port ") + port);
            using (var link = new SerialLink(port.Com, 115200)) {
                var rom = new EspRom(link, report);
                rom.Flash(fw.Esp.Data, fw.Esp.Address, fw.EspFlashSize, backup);
                rom.HardReset();
            }
        }

        void FlashBl616()
        {
            var port = Ports.FindBl616();
            if (port == null) throw new FlashException(Lang.T("BL616 в режиме прошивки не найден", "BL616 in flashing mode not found"));
            report.Log(Lang.T("Порт ", "Port ") + port);
            using (var link = new SerialLink(port.Com, 2000000))
                new Bl616Isp(link, report).Flash(fw.Bl616.Data, fw.Bl616.Address, FirmwareSet.Bl616PartEnd, backup);
        }

        void FlashTang(List<FlashPart> parts)
        {
            string why;
            using (var dev = Ftd2xx.OpenTang(out why)) {
                if (dev == null) throw new FlashException(why);
                report.Log("USB-JTAG: " + dev.Description + " " + dev.Serial);
                new Gowin(new Jtag(dev), report).Program(parts, backup);
            }
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (busy && MessageBox.Show(this, downloading ? Lang.T("Идёт загрузка с GitHub. Прервать и закрыть?", "Downloading from GitHub. Stop and close?")
                                                          : Lang.T("Идёт прошивка. Прервать и закрыть?", "Flashing is in progress. Stop and close?"),
                                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) {
                e.Cancel = true;
                return;
            }
            report.Cancel();
            base.OnFormClosing(e);
        }

        // ---- отчёт шага в окне и в файл -----------------------------------------------------------

        class UiReport : IReport
        {
            readonly Wizard w;
            volatile bool cancelled;
            readonly string file = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "VDAC4Flasher.log");
            public UiReport(Wizard w) { this.w = w; }
            public bool Cancelled { get { return cancelled; } }
            public void Cancel() { cancelled = true; }
            public void Reset() { cancelled = false; }

            public void Log(string text)
            {
                string line = DateTime.Now.ToString("HH:mm:ss.fff ") + text;
                try { File.AppendAllText(file, line + Environment.NewLine, Encoding.UTF8); } catch (Exception) { }
                Ui(() => { w.log.AppendText(line + Environment.NewLine); });
            }

            public void Progress(int percent, string stage)
            {
                Ui(() => {
                    w.bar.Value = Math.Max(0, Math.Min(100, percent));
                    w.stage.Text = stage + " — " + w.bar.Value + " %";
                });
            }

            void Ui(Action a)
            {
                try {
                    if (w.InvokeRequired) w.BeginInvoke(a); else a();
                } catch (InvalidOperationException) { }
            }
        }

        // ---- шаги слева -----------------------------------------------------------------------------

        class RailPanel : Panel
        {
            public Mark[] Marks;
            public string[] Titles = new string[0];

            public RailPanel() { DoubleBuffered = true; }

            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);
                var g = e.Graphics;
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                float s = DeviceDpi / 96f;
                for (int i = 0; i < Titles.Length; i++) {
                    float y = (24 + i * 62) * s, x = 22 * s, r = 15 * s;
                    var m = Marks[i];
                    Color c = m == Mark.Done ? Good : m == Mark.Failed ? Bad : m == Mark.Current ? Accent :
                              m == Mark.Skipped ? Color.FromArgb(160, 160, 170) : Color.FromArgb(190, 195, 205);
                    if (i < Titles.Length - 1)
                        using (var pen = new Pen(Color.FromArgb(210, 214, 222), 2 * s))
                            g.DrawLine(pen, x + r, y + 2 * r, x + r, y + 62 * s);
                    using (var b = new SolidBrush(m == Mark.Pending ? Color.White : c))
                        g.FillEllipse(b, x, y, 2 * r, 2 * r);
                    using (var pen = new Pen(c, 2 * s)) g.DrawEllipse(pen, x, y, 2 * r, 2 * r);
                    string sym = m == Mark.Done ? "✓" : m == Mark.Failed ? "✗" : m == Mark.Skipped ? "–" : (i == 0 ? "•" : i == 4 ? "★" : i.ToString());
                    using (var f = new Font("Segoe UI Semibold", 10f))
                    using (var b = new SolidBrush(m == Mark.Pending ? c : Color.White)) {
                        var sz = g.MeasureString(sym, f);
                        g.DrawString(sym, f, b, x + r - sz.Width / 2, y + r - sz.Height / 2);
                    }
                    using (var f = new Font(Font, m == Mark.Current ? FontStyle.Bold : FontStyle.Regular))
                    using (var b = new SolidBrush(m == Mark.Current ? Color.FromArgb(20, 20, 30) : Color.FromArgb(70, 70, 80)))
                        g.DrawString(Titles[i], f, b, x + 2 * r + 12 * s, y + r - 10 * s);
                }
            }
        }
    }
}
