// SPDX-License-Identifier: Apache-2.0
// Окно «Консоль VDAC4»: адрес платы, живой вывод ESP, кнопки команд консоли.
//
// Порядок работы окна:
//  • при запуске: адрес платы известен (запомнен в прошлый раз) — сразу подключаемся;
//    не известен — сами ищем плату в сети (кнопка «Найти плату» делает то же);
//  • текст с платы приходит из фонового потока (ConsoleLink) в очередь, а таймер окна раз в 0,1 с
//    переносит накопленное в поле — так окно не тонет, когда при подключении разом приходят
//    32 КБ истории;
//  • «Пауза» — текст копится в очереди, поле стоит на месте (удобно читать и выделять);
//  • закрытие окна или «Отключить» сразу отпускают консоль платы: она принимает только одного клиента.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace Vdac4Console
{
    public sealed class MainForm : Form
    {
        static readonly Color Accent = Color.FromArgb(37, 99, 235), Good = Color.FromArgb(22, 128, 61),
                              Warn = Color.FromArgb(161, 98, 7), Bad = Color.FromArgb(185, 28, 28),
                              Muted = Color.FromArgb(90, 90, 100), Bar = Color.FromArgb(243, 244, 246);
        const int KeepChars = 3000000;          // больше ~3 млн знаков в поле — срезаем самое старое
        const int CutChars = 1000000;           // …примерно по миллиону знаков за раз

        readonly Settings settings;
        readonly bool snapshot;                 // окно для картинки в описание: без сети
        ConsoleLink link;
        readonly object queueLock = new object();
        readonly StringBuilder queue = new StringBuilder();   // текст из сети, ещё не показанный
        readonly System.Windows.Forms.Timer pump = new System.Windows.Forms.Timer { Interval = 100 };
        bool searching;

        Label addressLabel, status, commandLabel, hint;
        TextBox address, output, command;
        Button find, connect, send, clear, copy, save;
        CheckBox pause;
        LinkLabel langLink;
        readonly List<KeyValuePair<Button, string>> quick = new List<KeyValuePair<Button, string>>();
        ContextMenuStrip boardsMenu;
        ToolTip tips;

        public MainForm(Settings settings, bool snapshot = false)
        {
            this.settings = settings;
            this.snapshot = snapshot;
            Font = new Font("Segoe UI", 10f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(1040, 700);
            MinimumSize = new Size(760, 480);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.White;
            Build();
            ApplyTexts();
            pump.Tick += (s, e) => Pump();
            if (!snapshot) pump.Start();
        }

        // ---------- построение окна ----------

        void Build()
        {
            tips = new ToolTip { AutoPopDelay = 20000, InitialDelay = 400 };
            var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(0),
                                              BackColor = Bar };     // фон за короткими подписями — как у полос
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));       // адрес и кнопки связи
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));       // строка состояния
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));   // вывод платы
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));       // команда и быстрые кнопки
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));       // подсказка
            Controls.Add(root);

            // Верхняя полоса: адрес, «Найти плату», «Подключить», язык.
            var top = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 5, BackColor = Bar,
                                             Padding = new Padding(12, 10, 12, 10), Margin = new Padding(0) };
            for (int i = 0; i < 4; i++) top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            addressLabel = new Label { AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, 8, 0) };
            address = new TextBox { Width = 170, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, 10, 0),
                                    Text = settings.Address };
            address.KeyDown += (s, e) => {
                if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; Connect(); }
            };
            find = MakeButton(false);
            find.Click += (s, e) => Find();
            connect = MakeButton(true);
            connect.Click += (s, e) => { if (link != null) Disconnect(); else Connect(); };
            langLink = new LinkLabel { AutoSize = true, Anchor = AnchorStyles.Right, LinkColor = Accent, Margin = new Padding(0) };
            langLink.LinkClicked += (s, e) => {
                Lang.Russian = !Lang.Russian;
                settings.Language = Lang.Russian ? "ru" : "en";
                settings.Save();
                ApplyTexts();
            };
            top.Controls.Add(addressLabel, 0, 0);
            top.Controls.Add(address, 1, 0);
            top.Controls.Add(find, 2, 0);
            top.Controls.Add(connect, 3, 0);
            top.Controls.Add(langLink, 4, 0);
            root.Controls.Add(top, 0, 0);

            status = new Label { AutoSize = true, Dock = DockStyle.Fill, Padding = new Padding(12, 6, 12, 6), Margin = new Padding(0),
                                 BackColor = Color.White,
                                 Font = new Font("Segoe UI Semibold", 10f), ForeColor = Muted };
            root.Controls.Add(status, 0, 1);

            // Вывод платы: тёмное поле, моноширинный шрифт, без переноса строк (таблицы журнала не ломаются).
            output = new TextBox { Multiline = true, ReadOnly = true, WordWrap = false, ScrollBars = ScrollBars.Both,
                                   Dock = DockStyle.Fill, Font = new Font("Consolas", 10f), BorderStyle = BorderStyle.None,
                                   BackColor = Color.FromArgb(24, 24, 27), ForeColor = Color.FromArgb(228, 228, 231),
                                   Margin = new Padding(0), HideSelection = false };
            output.MaxLength = 0;          // 0 — без предела (по умолчанию поле обрезало бы текст на 32 767 знаках)
            root.Controls.Add(output, 0, 2);

            // Нижняя полоса: строка команды, быстрые команды, пауза, очистить/копировать/сохранить.
            var bottom = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true, BackColor = Bar,
                                               Padding = new Padding(12, 8, 12, 4), Margin = new Padding(0) };
            commandLabel = new Label { AutoSize = true, Margin = new Padding(0, 9, 6, 0) };
            command = new TextBox { Width = 260, Margin = new Padding(0, 5, 6, 0) };
            command.KeyDown += (s, e) => {
                if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; SendTyped(); }
            };
            send = MakeButton(false);
            send.Click += (s, e) => SendTyped();
            pause = new CheckBox { AutoSize = true, Margin = new Padding(14, 10, 10, 0) };
            pause.CheckedChanged += (s, e) => { if (!pause.Checked) Pump(); ApplyPauseText(); };
            clear = MakeButton(false);
            clear.Click += (s, e) => output.Clear();
            copy = MakeButton(false);
            copy.Click += (s, e) => CopyAll();
            save = MakeButton(true);              // главная кнопка: журнал в файл — его и пришлют разработчику
            save.Click += (s, e) => SaveToFile();
            // первый ряд: строка команды и работа с текстом; второй — быстрые команды
            bottom.Controls.Add(commandLabel);
            bottom.Controls.Add(command);
            bottom.Controls.Add(send);
            bottom.Controls.Add(pause);
            bottom.Controls.Add(clear);
            bottom.Controls.Add(copy);
            bottom.Controls.Add(save);
            bottom.SetFlowBreak(save, true);
            foreach (string c in new[] { "ver", "mem", "top", "tang", "log" }) {
                var b = MakeButton(false);
                string cmd = c;
                b.Click += (s, e) => SendCommand(cmd);
                quick.Add(new KeyValuePair<Button, string>(b, cmd));
                bottom.Controls.Add(b);
            }
            root.Controls.Add(bottom, 0, 3);

            hint = new Label { AutoSize = true, Dock = DockStyle.Fill, BackColor = Bar, ForeColor = Muted,
                               Padding = new Padding(12, 2, 12, 10), Margin = new Padding(0), Font = new Font("Segoe UI", 9f) };
            root.Controls.Add(hint, 0, 4);
            root.SizeChanged += (s, e) => FitLabels();
            FitLabels();

            boardsMenu = new ContextMenuStrip();
        }

        Button MakeButton(bool primary)
        {
            var b = new Button { AutoSize = true, MinimumSize = new Size(0, 32), Padding = new Padding(8, 0, 8, 0),
                                 Margin = new Padding(0, 4, 8, 4), FlatStyle = FlatStyle.Flat, UseVisualStyleBackColor = false };
            if (primary) {
                b.BackColor = Accent; b.ForeColor = Color.White; b.FlatAppearance.BorderSize = 0;
                b.Font = new Font("Segoe UI Semibold", 10f);
            } else {
                b.BackColor = Color.White; b.ForeColor = Color.FromArgb(30, 30, 40);
                b.FlatAppearance.BorderColor = Color.FromArgb(200, 200, 210);
            }
            return b;
        }

        /// <summary>Длинные подписи переносятся по ширине окна, а не уходят за край.</summary>
        void FitLabels()
        {
            int w = Math.Max(300, ClientSize.Width);      // отступы от края даёт Padding самой подписи
            status.MaximumSize = new Size(w, 0);
            hint.MaximumSize = new Size(w, 0);
        }

        /// <summary>Все надписи — на языке окна (вызывается и при переключении языка).</summary>
        void ApplyTexts()
        {
            Text = Lang.T("Консоль VDAC4 — журнал ESP", "VDAC4 Console — ESP log");
            addressLabel.Text = Lang.T("Адрес платы:", "Board address:");
            find.Text = searching ? Lang.T("Ищу…", "Searching…") : Lang.T("Найти плату", "Find board");
            connect.Text = link != null ? Lang.T("Отключить", "Disconnect") : Lang.T("Подключить", "Connect");
            langLink.Text = Lang.Russian ? "English" : "Русский";
            commandLabel.Text = Lang.T("Команда:", "Command:");
            send.Text = Lang.T("Отправить", "Send");
            foreach (var q in quick) {
                switch (q.Value) {
                    case "ver": q.Key.Text = Lang.T("ver — версия", "ver — version"); break;
                    case "mem": q.Key.Text = Lang.T("mem — память", "mem — memory"); break;
                    case "top": q.Key.Text = Lang.T("top — загрузка", "top — CPU load"); break;
                    case "tang": q.Key.Text = Lang.T("tang — регистры Tang", "tang — Tang registers"); break;
                    case "log": q.Key.Text = Lang.T("log — журнал из флеша", "log — stored log"); break;
                }
            }
            tips.SetToolTip(find, Lang.T("Ищет плату во всех сетях этого ПК (порт 8268 ESP). 1–2 секунды.",
                                         "Looks for the board in all networks of this PC (ESP port 8268). 1–2 seconds."));
            foreach (var q in quick) {
                if (q.Value == "log")
                    tips.SetToolTip(q.Key, Lang.T("Журнал из флеша ESP: около 250 последних записей, переживает перезагрузку.\nНе нажимать во время игры: чтение флеша приостанавливает ESP.",
                                                  "The ESP stored log: ~250 last records, survives a restart.\nNot during a game: reading the flash pauses the ESP."));
                else
                    tips.SetToolTip(q.Key, Lang.T("Только читает, ничего на плате не меняет.", "Read-only, changes nothing on the board."));
            }
            ApplyPauseText();
            clear.Text = Lang.T("Очистить", "Clear");
            copy.Text = Lang.T("Копировать всё", "Copy all");
            save.Text = Lang.T("Сохранить в файл…", "Save to file…");
            hint.Text = Lang.T(
                "Журнал для разработчика: дождитесь зелёной строки «Подключено», повторите сбой на Evo (например, запустите ядро), " +
                "нажмите «Сохранить в файл…» и пришлите файл. Окно показывает вывод ESP с включения платы (последние 32 КБ) и всё новое. " +
                "«log» — журнал из флеша ESP, не во время игры. Консоль принимает одного клиента: закройте программу, когда журнал не нужен. " +
                "Команды записи (tang wr, tang core …) — только если знаете, что они делают.",
                "Log for the developer: wait for the green \"Connected\" line, repeat the failure on the Evo (for example, start the core), " +
                "press \"Save to file…\" and send the file. The window shows the ESP output since power-up (last 32 KB) and everything new. " +
                "\"log\" is the ESP stored log from flash, not during a game. The console takes one client: close the program when you no longer need the log. " +
                "Write commands (tang wr, tang core …) only if you know what they do.");
            if (link == null && !searching && status.Text.Length == 0)
                SetStatus(Lang.T("Не подключено.", "Not connected."), Muted);
        }

        void ApplyPauseText()
        {
            int waiting;
            lock (queueLock) waiting = queue.Length;
            pause.Text = pause.Checked && waiting > 0
                ? Lang.T("Пауза (ждёт ", "Pause (") + (waiting / 1024 + 1) + Lang.T(" КБ)", " KB waiting)")
                : Lang.T("Пауза", "Pause");
        }

        void SetStatus(string text, Color color)
        {
            status.Text = text;
            status.ForeColor = color;
        }

        // ---------- запуск ----------

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            if (snapshot) return;
            if (address.Text.Trim().Length > 0) Connect();
            else Find();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            pump.Stop();
            Disconnect();
            settings.Address = address.Text.Trim();
            settings.Save();
            base.OnFormClosing(e);
        }

        // ---------- связь ----------

        void Connect()
        {
            string host = address.Text.Trim();
            if (host.Length == 0) {
                SetStatus(Lang.T("Введите адрес платы или нажмите «Найти плату».", "Enter the board address or press \"Find board\"."), Warn);
                return;
            }
            IPAddress local = null;
            if (settings.Address == host) IPAddress.TryParse(settings.Local, out local);
            settings.Address = host;
            settings.Save();
            StartLink(new ConsoleLink(host, ConsoleLink.ConsolePort, local));
        }

        /// <summary>Отпустить прежнее соединение и запустить новое: текст — в очередь, состояние — в окно.</summary>
        void StartLink(ConsoleLink l)
        {
            Disconnect();
            l.TextArrived += t => { lock (queueLock) queue.Append(t); };
            // Событие приходит из фонового потока, а трогать окно можно только из его собственного потока:
            // BeginInvoke ставит вызов в очередь окна.
            l.StateChanged += (st, text) => {
                try { BeginInvoke(new Action(() => OnState(l, st, text))); } catch (InvalidOperationException) { }
            };
            link = l;
            ApplyTexts();
            l.Start();
        }

        void Disconnect()
        {
            ConsoleLink l = link;
            link = null;
            if (l != null) l.Stop();
            Pump();
            if (!IsDisposed) ApplyTexts();
        }

        void OnState(ConsoleLink from, LinkState state, string text)
        {
            if (from != link) {                         // сообщение от прежнего соединения
                if (state == LinkState.Stopped && link == null) SetStatus(text, Muted);
                return;
            }
            switch (state) {
                case LinkState.Connected:
                    SetStatus(text, Good);
                    if (from.WorkingLocal != null && settings.Local != from.WorkingLocal.ToString()) {
                        settings.Local = from.WorkingLocal.ToString();
                        settings.Save();
                    }
                    break;
                case LinkState.Connecting: SetStatus(text, Muted); break;
                case LinkState.Silent: SetStatus(text, Warn); break;
                case LinkState.Lost: SetStatus(text, Bad); break;
                case LinkState.Stopped: SetStatus(text, Muted); break;
            }
        }

        void SendTyped()
        {
            string c = command.Text.Trim();
            if (c.Length == 0) return;
            if (SendCommand(c)) command.Clear();
        }

        bool SendCommand(string c)
        {
            if (link == null || !link.Send(c)) {
                SetStatus(Lang.T("Нет связи с консолью — команда «", "No console connection — command \"") + c +
                          Lang.T("» не отправлена.", "\" not sent."), Bad);
                return false;
            }
            // Плата команду не повторяет — покажем её сами, чтобы было видно, где начинается ответ.
            lock (queueLock) queue.Append("\r\n> " + c + "\r\n");
            if (c == "log")
                SetStatus(Lang.T("Читаю журнал из флеша ESP…", "Reading the ESP stored log…"), Good);
            return true;
        }

        // ---------- поиск платы ----------

        void Find()
        {
            if (searching) return;
            searching = true;
            ApplyTexts();
            find.Enabled = false;
            SetStatus(Lang.T("Ищу плату VDAC4 в сети…", "Looking for the VDAC4 board on the network…"), Muted);
            var t = new Thread(() => {
                List<FoundBoard> boards;
                try { boards = Network.FindBoards(); } catch (Exception) { boards = new List<FoundBoard>(); }
                try { BeginInvoke(new Action(() => Found(boards))); } catch (InvalidOperationException) { }
            }) { IsBackground = true, Name = "VDAC4 search" };
            t.Start();
        }

        void Found(List<FoundBoard> boards)
        {
            searching = false;
            find.Enabled = true;
            ApplyTexts();
            if (boards.Count == 0) {
                SetStatus(Lang.T(
                    "Плата не найдена. Проверьте: Evo включён, ESP подключён к Wi-Fi, ПК в той же сети. Адрес можно ввести вручную — его показывают плагины FTP Server и SMB Server в Wild Commander (строка «IP»).",
                    "No board found. Check: the Evo is on, the ESP is on Wi-Fi, the PC is on the same network. You can type the address — the FTP Server and SMB Server plugins of Wild Commander show it (the \"IP\" line)."), Bad);
                return;
            }
            if (boards.Count == 1) {
                Use(boards[0]);
                return;
            }
            // Несколько плат — список под кнопкой, пусть выберет человек.
            boardsMenu.Items.Clear();
            foreach (FoundBoard b in boards) {
                FoundBoard board = b;
                boardsMenu.Items.Add(b.ToString(), null, (s, e) => Use(board));
            }
            SetStatus(Lang.T("Найдено плат: ", "Boards found: ") + boards.Count + Lang.T(". Выберите нужную.", ". Pick one."), Warn);
            boardsMenu.Show(find, new Point(0, find.Height));
        }

        void Use(FoundBoard b)
        {
            address.Text = b.Address.ToString();
            settings.Address = address.Text;
            settings.Local = b.Local != null ? b.Local.ToString() : "";
            settings.Save();
            lock (queueLock)
                queue.Append("===== " + DateTime.Now.ToString("HH:mm:ss") + " " + Lang.T("найдена плата ", "board found at ") +
                             b.Address + ", ESP " + b.Version + " =====\r\n");
            Connect();
        }

        // ---------- вывод ----------

        /// <summary>Перенести накопленный текст в поле (раз в 0,1 с и при снятии паузы).</summary>
        void Pump()
        {
            if (IsDisposed) return;
            if (pause.Checked) { ApplyPauseText(); return; }
            string text;
            lock (queueLock) {
                if (queue.Length == 0) return;
                text = queue.ToString();
                queue.Clear();
            }
            if (output.TextLength + text.Length > KeepChars) {
                // Самое старое — прочь, по границе строки: поле в миллионы знаков тормозит.
                string all = output.Text + text;
                int cut = all.IndexOf('\n', Math.Min(all.Length - 1, all.Length - KeepChars + CutChars));
                output.Text = cut >= 0 ? all.Substring(cut + 1) : all;
                output.SelectionStart = output.TextLength;
                output.ScrollToCaret();
            } else {
                output.AppendText(text);
            }
        }

        string AllText()
        {
            string waiting;
            lock (queueLock) waiting = queue.ToString();
            return output.Text + waiting;
        }

        void CopyAll()
        {
            string t = AllText();
            if (t.Length == 0) return;
            try {
                Clipboard.SetText(t);
                SetStatus(Lang.T("Весь текст скопирован.", "All text copied."), Good);
            } catch (Exception e) {
                SetStatus(e.Message, Bad);
            }
        }

        void SaveToFile()
        {
            using (var d = new SaveFileDialog()) {
                d.Filter = Lang.T("Текст (*.txt)|*.txt|Все файлы|*.*", "Text (*.txt)|*.txt|All files|*.*");
                d.FileName = "vdac4_esp_log_" + DateTime.Now.ToString("yyyy-MM-dd_HH-mm") + ".txt";
                d.InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Desktop);
                if (d.ShowDialog(this) != DialogResult.OK) return;
                try {
                    File.WriteAllText(d.FileName, AllText(), new UTF8Encoding(false));
                    SetStatus(Lang.T("Сохранено: ", "Saved: ") + d.FileName, Good);
                } catch (Exception e) {
                    SetStatus(e.Message, Bad);
                }
            }
        }

        // ---------- для проверки и картинок ----------

        /// <summary>Подставить текст и состояние без сети (картинки для описания, проверки на ПК).</summary>
        public void ShowSample(string text, string state, int color)
        {
            address.Text = "192.168.1.110";
            output.Text = text;
            output.SelectionStart = output.TextLength;
            output.ScrollToCaret();
            SetStatus(state, color == 0 ? Good : color == 1 ? Warn : Bad);
        }

        /// <summary>Текст поля вывода (для проверки на ПК).</summary>
        public string OutputText { get { Pump(); return output.Text; } }

        /// <summary>Текст строки состояния (для проверки на ПК).</summary>
        public string StatusText { get { return status.Text; } }

        /// <summary>Подключиться к адресу и порту модели консоли (для проверки на ПК).</summary>
        public void ConnectForTest(string host, int port)
        {
            StartLink(new ConsoleLink(host, port) { RetryDelayMs = 200, ConnectTimeoutMs = 1000, SilentAfterMs = 500 });
        }

        public bool SendForTest(string c) { return SendCommand(c); }
    }
}
