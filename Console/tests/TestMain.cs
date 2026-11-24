// SPDX-License-Identifier: Apache-2.0
// Проверки «Консоли VDAC4» на ПК, без платы: модели консоли ESP (порт 23) и служебного порта 8268
// на адресах 127.0.0.x этого ПК ведут себя как net_console.cpp и tang_update.cpp.
//   ConsoleTests.exe <папка для временных файлов>
// Итог — строка «N checks, M failed»; код выхода 0 — всё прошло.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace Vdac4Console
{
    static class TestMain
    {
        static int checks, failed;

        static void Check(bool ok, string what)
        {
            checks++;
            if (!ok) failed++;
            Console.WriteLine((ok ? "ok   " : "FAIL ") + what);
        }

        /// <summary>Ждать условие не дольше ms, прокачивая очередь окна (иначе BeginInvoke не дойдёт).</summary>
        static bool Wait(Func<bool> cond, int ms)
        {
            var clock = Stopwatch.StartNew();
            while (clock.ElapsedMilliseconds < ms) {
                Application.DoEvents();
                if (cond()) return true;
                Thread.Sleep(20);
            }
            return cond();
        }

        // ---------- модель консоли ESP ----------

        /// <summary>
        /// Как net_console.cpp: при подключении — строка приветствия и история, затем ответы на команды.
        /// Один клиент за раз (следующий ждёт, пока не закончится текущий) — как listen(server, 1).
        /// </summary>
        sealed class ModelConsole : IDisposable
        {
            public readonly TcpListener Listener;
            public readonly List<string> Commands = new List<string>();
            public int Accepted;
            public volatile bool Silent;              // не слать ничего (консоль «занята»)
            public volatile bool ClientClosed;        // клиент закрыл соединение сам
            public byte[] History = new byte[0];
            volatile Socket current;
            readonly Thread thread;
            volatile bool stop;

            public ModelConsole(IPAddress at)
            {
                Listener = new TcpListener(at, 0);
                Listener.Start(1);
                thread = new Thread(Run) { IsBackground = true };
                thread.Start();
            }

            public int Port { get { return ((IPEndPoint)Listener.LocalEndpoint).Port; } }

            /// <summary>Оборвать текущее соединение (как перезагрузка платы).</summary>
            public void DropClient()
            {
                Socket s = current;
                if (s != null) try { s.Close(); } catch (Exception) { }
            }

            void Run()
            {
                while (!stop) {
                    Socket s;
                    try { s = Listener.AcceptSocket(); } catch (Exception) { return; }
                    current = s;
                    Interlocked.Increment(ref Accepted);
                    ClientClosed = false;
                    try {
                        if (!Silent) {
                            s.Send(Encoding.ASCII.GetBytes("VDAC4 console, vdac4-test. Commands: help, mem, ver, log, tang\n"));
                            // историю — по одному байту на первых 64 байтах: так рвутся и русские буквы
                            int k = 0;
                            for (; k < Math.Min(64, History.Length); k++) s.Send(History, k, 1, SocketFlags.None);
                            if (k < History.Length) s.Send(History, k, History.Length - k, SocketFlags.None);
                        }
                        var line = new StringBuilder();
                        var b = new byte[256];
                        while (true) {
                            int n = s.Receive(b);
                            if (n <= 0) { ClientClosed = true; break; }
                            for (int i = 0; i < n; i++) {
                                if (b[i] == '\n') {
                                    string c = line.ToString();
                                    line.Clear();
                                    lock (Commands) Commands.Add(c);
                                    if (c == "ver") s.Send(Encoding.ASCII.GetBytes("vdac4-test, up 1 s, 9 tasks\n"));
                                } else {
                                    line.Append((char)b[i]);
                                }
                            }
                        }
                    } catch (Exception) {
                    }
                    try { s.Close(); } catch (Exception) { }
                    current = null;
                }
            }

            public void Dispose()
            {
                stop = true;
                DropClient();
                Listener.Stop();
            }
        }

        // ---------- модель порта 8268 ----------

        /// <summary>Шлёт одну строку и ждёт; запоминает, прислал ли клиент хоть байт.</summary>
        sealed class ModelBanner : IDisposable
        {
            readonly TcpListener listener;
            public volatile int Connections, BytesFromClient;
            readonly string banner;

            public ModelBanner(IPAddress at, int port, string banner)
            {
                this.banner = banner;
                listener = new TcpListener(at, port);
                listener.Start(4);
                new Thread(Run) { IsBackground = true }.Start();
            }

            public int Port { get { return ((IPEndPoint)listener.LocalEndpoint).Port; } }

            void Run()
            {
                while (true) {
                    Socket s;
                    try { s = listener.AcceptSocket(); } catch (Exception) { return; }
                    Connections++;
                    new Thread(() => {
                        try {
                            s.Send(Encoding.ASCII.GetBytes(banner + "\n"));
                            var b = new byte[64];
                            int n;
                            while ((n = s.Receive(b)) > 0) BytesFromClient += n;
                        } catch (Exception) { }
                        try { s.Close(); } catch (Exception) { }
                    }) { IsBackground = true }.Start();
                }
            }

            public void Dispose() { listener.Stop(); }
        }

        // ---------- проверки ----------

        static void TestText()
        {
            var t = new ConsoleText();
            byte[] ru = Encoding.UTF8.GetBytes("журнал ESP\n");
            var sb = new StringBuilder();
            for (int i = 0; i < ru.Length; i++) sb.Append(t.Feed(ru, i, 1));        // по байту: буквы рвутся
            Check(sb.ToString() == "журнал ESP\r\n", "UTF-8 split byte by byte: «" + sb.ToString().TrimEnd() + "»");

            t = new ConsoleText();
            byte[] ends = Encoding.ASCII.GetBytes("a\nb\r\nc\rd\n");
            Check(t.Feed(ends, 0, ends.Length) == "a\r\nb\r\nc\r\nd\r\n", "line ends \\n, \\r\\n, \\r → \\r\\n");

            t = new ConsoleText();
            byte[] crlf = Encoding.ASCII.GetBytes("x\r");
            byte[] lf = Encoding.ASCII.GetBytes("\ny\n");
            Check(t.Feed(crlf, 0, crlf.Length) + t.Feed(lf, 0, lf.Length) == "x\r\ny\r\n", "\\r and \\n in different chunks → one line end");

            t = new ConsoleText();
            byte[] ansi = Encoding.ASCII.GetBytes("\x1b[0;32mI (1) ok\x1b[0m\n\0bell\a\tX\n");
            string a = t.Feed(ansi, 0, 5) + t.Feed(ansi, 5, ansi.Length - 5);
            Check(a == "I (1) ok\r\nbell\tX\r\n", "colour codes, NUL and bell removed, tab kept: " + a.Replace("\r\n", "|"));
        }

        static void TestLink()
        {
            using (var m = new ModelConsole(IPAddress.Loopback)) {
                m.History = Encoding.UTF8.GetBytes("I (100) запуск\nI (200) tang: LINK\n");
                var text = new StringBuilder();
                var states = new List<LinkState>();
                var link = new ConsoleLink("127.0.0.1", m.Port) { RetryDelayMs = 200, ConnectTimeoutMs = 1000, SilentAfterMs = 500 };
                link.TextArrived += s => { lock (text) text.Append(s); };
                link.StateChanged += (st, s) => { lock (states) states.Add(st); };
                link.Start();
                Func<string> got = () => { lock (text) return text.ToString(); };
                Check(Wait(() => got().Contains("I (200) tang: LINK\r\n"), 3000), "banner and history received");
                Check(got().StartsWith("VDAC4 console, vdac4-test.") && got().Contains("I (100) запуск\r\n"),
                      "Russian text in history survives single-byte chunks");
                Check(link.Send("ver"), "Send(\"ver\") returns true");
                Check(Wait(() => got().Contains("vdac4-test, up 1 s"), 2000), "answer to ver shown");
                link.Send("tang rd 40\r\n");
                Check(Wait(() => { lock (m.Commands) return m.Commands.Count >= 2; }, 2000), "two commands arrived");
                lock (m.Commands)
                    Check(m.Commands.Count == 2 && m.Commands[0] == "ver" && m.Commands[1] == "tang rd 40",
                          "commands arrive as single lines ending in \\n: " + string.Join(" | ", m.Commands));

                // плата перезагрузилась: соединение оборвалось — программа подключается снова сама
                m.DropClient();
                Check(Wait(() => m.Accepted >= 2, 3000), "reconnects after the board drops the connection");
                Check(Wait(() => got().Contains("=====") && got().LastIndexOf("VDAC4 console") > got().IndexOf("====="), 3000),
                      "reconnect marker, then the history again");

                // «Отключить» отпускает консоль сразу: модель видит конец соединения
                var clock = Stopwatch.StartNew();
                link.Stop();
                Check(Wait(() => m.ClientClosed, 2000), "Stop() releases the console (" + clock.ElapsedMilliseconds + " ms)");
                Check(!link.Running, "thread ended after Stop()");
                lock (states) Check(states.Count > 0 && states[states.Count - 1] == LinkState.Stopped, "last state is Stopped");
                Check(!link.Send("ver"), "Send() after Stop() returns false");
            }

            // консоль молчит (её держит другой клиент) — подсказка Silent
            using (var m = new ModelConsole(IPAddress.Loopback) { Silent = true }) {
                var states = new List<LinkState>();
                var link = new ConsoleLink("127.0.0.1", m.Port) { RetryDelayMs = 200, ConnectTimeoutMs = 1000, SilentAfterMs = 400 };
                link.StateChanged += (st, s) => { lock (states) states.Add(st); };
                link.Start();
                Check(Wait(() => { lock (states) return states.Contains(LinkState.Silent); }, 2000), "silent console → Silent hint");
                link.Stop();
            }

            // порт закрыт — отказ, с новой попыткой
            int closedPort;
            var probe = new TcpListener(IPAddress.Loopback, 0);
            probe.Start();
            closedPort = ((IPEndPoint)probe.LocalEndpoint).Port;
            probe.Stop();
            {
                var said = new List<string>();
                // Время ожидания — как в программе (3 с): получив отказ, Windows ещё ~2 с повторяет попытку
                // и только потом сообщает «отказано»; при 1 с это выглядело бы как «не отвечает».
                var link = new ConsoleLink("127.0.0.1", closedPort) { RetryDelayMs = 200 };
                link.StateChanged += (st, s) => { if (st == LinkState.Lost) lock (said) said.Add(s); };
                link.Start();
                Check(Wait(() => { lock (said) return said.Count >= 2; }, 9000), "closed port → Lost, retried");
                lock (said) Check(said.Count > 0 && (said[0].Contains("отказала") || said[0].Contains("refused")), "refusal explained: " + (said.Count > 0 ? said[0] : ""));
                link.Stop();
            }

            // неверный адрес
            {
                string said = null;
                var link = new ConsoleLink("нет.такого.адреса.invalid", 23) { RetryDelayMs = 200 };
                link.StateChanged += (st, s) => { if (st == LinkState.Lost && said == null) said = s; };
                link.Start();
                Check(Wait(() => said != null, 15000), "bad host name → Lost: " + said);
                link.Stop();
            }
        }

        static void TestFind()
        {
            IPAddress one = IPAddress.Parse("127.0.0.1"), two = IPAddress.Parse("127.0.0.2"), three = IPAddress.Parse("127.0.0.3");
            using (var board = new ModelBanner(one, 0, "ZIFI-TANG/1 vdac4-0.1.167 8388608"))
            using (var other = new ModelBanner(two, board.Port, "SSH-2.0-OpenSSH_9.6")) {
                var targets = new List<KeyValuePair<IPAddress, IPAddress>> {
                    new KeyValuePair<IPAddress, IPAddress>(one, null),
                    new KeyValuePair<IPAddress, IPAddress>(two, null),
                    new KeyValuePair<IPAddress, IPAddress>(three, null),          // никого нет
                    new KeyValuePair<IPAddress, IPAddress>(one, IPAddress.Loopback) // та же плата «через второй адаптер»
                };
                var clock = Stopwatch.StartNew();
                List<FoundBoard> found = Network.FindBoards(1000, 1000, board.Port, targets);
                Check(found.Count == 1 && found[0].Address.Equals(one) && found[0].Version == "vdac4-0.1.167",
                      "finds the board by its banner, ignores other services and duplicates (" + found.Count + " found, " + clock.ElapsedMilliseconds + " ms)");
                Check(Wait(() => board.Connections >= 1, 1000) && board.BytesFromClient == 0, "search sends nothing to the board");
            }
            var all = Network.ScanTargets();
            var unique = new HashSet<string>();
            foreach (var t in all) unique.Add(t.Key + ">" + t.Value);
            Check(all.Count <= 2048 && unique.Count == all.Count, "scan targets of this PC: " + all.Count + " addresses, no repeats");
        }

        static void TestForm(string tmp)
        {
            using (var m = new ModelConsole(IPAddress.Loopback)) {
                // 40 КБ истории: больше 32 767 знаков, на которых обычное поле Windows обрезает текст
                var h = new StringBuilder();
                for (int i = 0; h.Length < 40000; i++) h.Append("I (" + i + ") строка журнала номер " + i + "\n");
                h.Append("I (99999) последняя строка\n");
                m.History = Encoding.UTF8.GetBytes(h.ToString());
                string ini = Path.Combine(tmp, "form_test.ini");
                if (File.Exists(ini)) File.Delete(ini);
                var settings = new Settings(ini);
                using (var w = new MainForm(settings, false)) {
                    w.StartPosition = FormStartPosition.Manual;
                    w.Location = new Point(-20000, -20000);
                    w.ShowInTaskbar = false;
                    w.ConnectForTest("127.0.0.1", m.Port);
                    w.Show();
                    Check(Wait(() => w.OutputText.Contains("I (99999) последняя строка"), 5000),
                          "form shows all 40 KB of history (" + w.OutputText.Length + " chars)");
                    Check(w.OutputText.Length > 32767, "text box is not cut at 32 767 chars");
                    Check(w.SendForTest("ver") && Wait(() => w.OutputText.Contains("> ver\r\nvdac4-test, up 1 s"), 3000),
                          "command echoed in the window, answer below it");
                    w.Close();
                }
                Check(Wait(() => m.ClientClosed, 2000), "closing the window releases the console");
            }
        }

        [STAThread]
        static int Main(string[] args)
        {
            string tmp = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "vdac4console");
            Directory.CreateDirectory(tmp);
            Lang.Russian = true;
            TestText();
            TestLink();
            TestFind();
            TestForm(tmp);
            Console.WriteLine(checks + " checks, " + failed + " failed");
            return failed == 0 ? 0 : 1;
        }
    }
}
