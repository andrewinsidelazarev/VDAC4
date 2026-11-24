// SPDX-License-Identifier: Apache-2.0
// Соединение с сетевой консолью ESP платы VDAC4 (TCP, порт 23; в прошивке — src/net_console.cpp).
//
// Как устроена консоль на плате:
//  • ESP держит в памяти кольцо из последних 32 КБ своего вывода (всё, что пишут ESP_LOG*).
//  • Подключился клиент — ESP шлёт строку «VDAC4 console, vdac4-X.Y.Z. Commands: …», потом всё кольцо
//    (историю с включения платы), потом новые строки по мере появления.
//  • Строка от клиента, оконченная «\n», — команда: help, mem, ver, top, log, tang …
//  • Клиент только один: пока кто-то подключён, новые подключения ждут в очереди и ничего не получают.
//    Поэтому программа отпускает консоль сразу, как только пользователь нажал «Отключить» или закрыл окно.
//
// Работа идёт в отдельном потоке: окно не замирает, пока сеть ждёт. Обрыв связи (плата
// перезагрузилась, Wi-Fi пропал) — не конец: поток сам подключается снова каждые 3 секунды,
// пока пользователь не нажмёт «Отключить».
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace Vdac4Console
{
    /// <summary>Что сейчас со связью — для строки состояния окна.</summary>
    public enum LinkState
    {
        Connecting,      // пробуем подключиться
        Connected,       // подключены, данные идут
        Silent,          // подключены, но консоль молчит: скорее всего, её держит другой клиент
        Lost,            // связи нет, скоро новая попытка
        Stopped          // пользователь отключился сам
    }

    public sealed class ConsoleLink : IDisposable
    {
        public const int ConsolePort = 23;

        /// <summary>Новый текст с платы. Вызывается из фонового потока!</summary>
        public event Action<string> TextArrived;
        /// <summary>Связь изменилась: состояние и пояснение. Вызывается из фонового потока!</summary>
        public event Action<LinkState, string> StateChanged;

        /// <summary>С какого адреса ПК удалось подключиться (null — адрес выбрала Windows).</summary>
        public IPAddress WorkingLocal { get; private set; }

        // Таймауты, мс. Поля, а не константы: проверка на ПК ставит их короче.
        public int ConnectTimeoutMs = 3000;      // ждать ответа на подключение
        public int RetryDelayMs = 3000;          // пауза перед новой попыткой после обрыва
        public int SilentAfterMs = 4000;         // столько молчания после подключения — подсказать «занято»

        readonly string host;
        readonly int port;
        readonly IPAddress preferredLocal;
        readonly object sendLock = new object();
        readonly ManualResetEvent wake = new ManualResetEvent(false);   // будит паузы при «Отключить»
        Thread thread;
        volatile bool stopping;
        volatile Socket socket;                  // текущее соединение (null — нет)

        /// <param name="host">Адрес платы: «192.168.1.110» или имя.</param>
        /// <param name="preferredLocal">Адрес этого ПК, с которого плату нашёл поиск (можно null).</param>
        public ConsoleLink(string host, int port = ConsolePort, IPAddress preferredLocal = null)
        {
            this.host = host.Trim();
            this.port = port;
            this.preferredLocal = preferredLocal;
        }

        public bool Running { get { return thread != null && thread.IsAlive; } }

        public void Start()
        {
            if (thread != null) throw new InvalidOperationException("already started");
            thread = new Thread(Run) { IsBackground = true, Name = "VDAC4 console" };
            thread.Start();
        }

        /// <summary>Отключиться и отпустить консоль платы. Ждёт конца потока не дольше 2 секунд.</summary>
        public void Stop()
        {
            stopping = true;
            wake.Set();
            CloseSocket();
            if (thread != null && thread != Thread.CurrentThread) thread.Join(2000);
        }

        public void Dispose() { Stop(); }

        /// <summary>Послать команду консоли («ver», «log»…). false — связи сейчас нет.</summary>
        public bool Send(string command)
        {
            Socket s = socket;
            if (s == null) return false;
            // Команда — одна строка ASCII с «\n» в конце: так её ждёт net_console.cpp (serve).
            byte[] bytes = Encoding.ASCII.GetBytes(command.Replace("\r", "").Replace("\n", "") + "\n");
            try {
                lock (sendLock) s.Send(bytes);
                return true;
            } catch (Exception) {
                CloseSocket();          // поток приёма увидит обрыв и подключится снова
                return false;
            }
        }

        void Say(LinkState state, string text)
        {
            var h = StateChanged;
            if (h != null) h(state, text);
        }

        void Text(string text)
        {
            var h = TextArrived;
            if (h != null && text.Length > 0) h(text);
        }

        void CloseSocket()
        {
            Socket s = socket;
            socket = null;
            if (s == null) return;
            try { s.Close(); } catch (Exception) { }
        }

        /// <summary>Пауза, которую прерывает «Отключить». true — пора выходить.</summary>
        bool Pause(int ms)
        {
            wake.WaitOne(ms);
            return stopping;
        }

        void Run()
        {
            bool everConnected = false;
            while (!stopping) {
                Say(LinkState.Connecting, Lang.T("Подключение к ", "Connecting to ") + host + ":" + port + "…");
                string why;
                Socket s = Connect(out why);
                if (stopping) { if (s != null) s.Close(); break; }
                if (s == null) {
                    Say(LinkState.Lost, why + " " + Lang.T("Новая попытка через 3 с.", "Retrying in 3 s."));
                    if (Pause(RetryDelayMs)) break;
                    continue;
                }
                socket = s;
                if (everConnected)
                    Text("\r\n===== " + DateTime.Now.ToString("HH:mm:ss") + " " +
                         Lang.T("снова подключено: ниже опять история с включения платы",
                                "reconnected: the history since power-up follows again") + " =====\r\n");
                everConnected = true;
                Say(LinkState.Connected, Lang.T("Подключено к ", "Connected to ") + host + ":" + port);
                string end = Receive(s);
                CloseSocket();
                if (stopping) break;
                Say(LinkState.Lost, end + " " + Lang.T("Новая попытка через 3 с.", "Retrying in 3 s."));
                if (Pause(RetryDelayMs)) break;
            }
            Say(LinkState.Stopped, Lang.T("Отключено.", "Disconnected."));
        }

        /// <summary>Принимать, пока связь жива. Возвращает пояснение, почему кончилось.</summary>
        string Receive(Socket s)
        {
            var text = new ConsoleText();
            var buffer = new byte[8192];
            DateTime since = DateTime.UtcNow;
            bool gotAny = false, saidSilent = false;
            try {
                while (!stopping) {
                    // Poll, а не таймаут приёма: в Windows сокет после таймаута Receive нельзя использовать дальше.
                    if (!s.Poll(250 * 1000, SelectMode.SelectRead)) {
                        if (!gotAny && !saidSilent && (DateTime.UtcNow - since).TotalMilliseconds > SilentAfterMs) {
                            saidSilent = true;
                            Say(LinkState.Silent, Lang.T(
                                "Подключено, но консоль молчит: скорее всего, её держит другая программа (консоль принимает одного клиента). Как только та отключится, вывод пойдёт сюда.",
                                "Connected, but the console is silent: most likely another program holds it (the console takes one client). Output appears here as soon as it disconnects."));
                        }
                        continue;
                    }
                    int n = s.Receive(buffer);
                    if (n <= 0) return Lang.T("Плата закрыла соединение.", "The board closed the connection.");
                    if (!gotAny) {
                        gotAny = true;
                        if (saidSilent) Say(LinkState.Connected, Lang.T("Подключено к ", "Connected to ") + host + ":" + port);
                    }
                    Text(text.Feed(buffer, 0, n));
                }
                return "";
            } catch (ObjectDisposedException) {
                return Lang.T("Соединение закрыто.", "Connection closed.");
            } catch (SocketException e) {
                return Lang.T("Связь потеряна: ", "Connection lost: ") + e.Message;
            }
        }

        /// <summary>
        /// Подключиться к плате. Если у ПК несколько сетевых адаптеров в одной сети (виртуальный мост
        /// Hyper-V, VPN), Windows может отправить пакеты не в тот адаптер — тогда пробуем ещё и с каждого
        /// адреса ПК из той же сети, что и плата.
        /// </summary>
        Socket Connect(out string why)
        {
            why = "";
            IPAddress target;
            try {
                target = Resolve(host);
            } catch (Exception e) {
                why = Lang.T("Не понял адрес «", "Bad address \"") + host + Lang.T("»: ", "\": ") + e.Message;
                return null;
            }
            var locals = new List<IPAddress>();
            if (WorkingLocal != null) locals.Add(WorkingLocal);
            if (preferredLocal != null && !locals.Contains(preferredLocal)) locals.Add(preferredLocal);
            if (!locals.Contains(null)) locals.Add(null);                       // как решит Windows
            foreach (IPAddress a in Network.LocalAddressesFor(target))
                if (!locals.Contains(a)) locals.Add(a);

            bool refused = false;
            foreach (IPAddress local in locals) {
                if (stopping) return null;
                SocketError error;
                Socket s = Network.TryConnect(target, port, local, ConnectTimeoutMs, out error);
                if (s != null) {
                    WorkingLocal = local;
                    Network.KeepAlive(s);
                    return s;
                }
                if (error == SocketError.ConnectionRefused) refused = true;
            }
            why = refused
                ? Lang.T("Плата отказала в подключении к порту ", "The board refused a connection to port ") + port + "."
                : Lang.T("Плата не отвечает по адресу ", "No answer from ") + target + ".";
            return null;
        }

        static IPAddress Resolve(string name)
        {
            IPAddress a;
            if (IPAddress.TryParse(name, out a)) return a;
            foreach (IPAddress x in Dns.GetHostAddresses(name))
                if (x.AddressFamily == AddressFamily.InterNetwork) return x;
            throw new ArgumentException(Lang.T("нет адреса IPv4", "no IPv4 address"));
        }
    }
}
