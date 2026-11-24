// SPDX-License-Identifier: Apache-2.0
// Сеть ПК: адреса своих адаптеров, подключение с таймаутом и поиск платы VDAC4 в локальной сети.
//
// Как найти плату, не зная её адреса. Адрес плате выдаёт роутер (DHCP), и он может меняться.
// Зато у ESP всегда открыт служебный порт 8268 (обновление прошивки Tang, tang_update.cpp), и при
// подключении ESP сразу пишет строку «ZIFI-TANG/1 <версия> <размер>». Значит, достаточно постучаться
// на порт 8268 по всем адресам своей сети (обычно 192.168.x.1 … 192.168.x.254) и ждать эту строку.
// Подключение тут же закрывается, ничего не отправив: ESP ждёт команду, видит конец соединения
// и просто возвращается к ожиданию — на работу платы это не влияет.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace Vdac4Console
{
    /// <summary>Найденная плата: её адрес, адрес ПК, с которого она ответила, и версия прошивки ESP.</summary>
    public sealed class FoundBoard
    {
        public IPAddress Address;
        public IPAddress Local;
        public string Version;
        public override string ToString() { return Address + "  (ESP " + Version + ")"; }
    }

    public static class Network
    {
        public const int UpdatePort = 8268;              // служебный порт ESP, по нему и ищем плату
        public const string Banner = "ZIFI-TANG/1 ";      // первая строка, которую ESP шлёт на этом порту
        const int MaxTargets = 2048;                      // не больше 8 сетей по 254 адреса

        /// <summary>Адрес и маска одного адаптера ПК (только IPv4, только включённые адаптеры).</summary>
        sealed class LocalNet
        {
            public IPAddress Address;
            public uint Ip, Mask;
            public bool HasGateway;
        }

        static uint ToUint(IPAddress a)
        {
            byte[] b = a.GetAddressBytes();
            return (uint)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        }

        static IPAddress FromUint(uint v)
        {
            return new IPAddress(new[] { (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v });
        }

        static List<LocalNet> LocalNets()
        {
            var list = new List<LocalNet>();
            NetworkInterface[] all;
            try { all = NetworkInterface.GetAllNetworkInterfaces(); } catch (NetworkInformationException) { return list; }
            foreach (NetworkInterface ni in all) {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback ||
                    ni.NetworkInterfaceType == NetworkInterfaceType.Tunnel) continue;
                IPInterfaceProperties p;
                try { p = ni.GetIPProperties(); } catch (NetworkInformationException) { continue; }
                bool gateway = false;
                foreach (GatewayIPAddressInformation g in p.GatewayAddresses)
                    if (g.Address.AddressFamily == AddressFamily.InterNetwork && !g.Address.Equals(IPAddress.Any)) gateway = true;
                foreach (UnicastIPAddressInformation u in p.UnicastAddresses) {
                    if (u.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    uint ip = ToUint(u.Address);
                    if ((ip >> 16) == 0xA9FE) continue;            // 169.254.x.x — адрес «нет DHCP», там платы нет
                    uint mask = u.IPv4Mask != null ? ToUint(u.IPv4Mask) : 0xFFFFFF00u;
                    if (mask == 0) mask = 0xFFFFFF00u;
                    list.Add(new LocalNet { Address = u.Address, Ip = ip, Mask = mask, HasGateway = gateway });
                }
            }
            // Сначала адаптеры со шлюзом (настоящая домашняя сеть), потом виртуальные.
            list.Sort((a, b) => (b.HasGateway ? 1 : 0) - (a.HasGateway ? 1 : 0));
            return list;
        }

        /// <summary>Адреса ПК, которые лежат в одной сети с target (для подключения «с нужного адаптера»).</summary>
        public static List<IPAddress> LocalAddressesFor(IPAddress target)
        {
            var result = new List<IPAddress>();
            uint t = ToUint(target);
            foreach (LocalNet n in LocalNets())
                if ((n.Ip & n.Mask) == (t & n.Mask)) result.Add(n.Address);
            return result;
        }

        /// <summary>
        /// Подключиться по TCP не дольше timeoutMs. local — с какого адреса ПК (null — как решит Windows).
        /// Возвращает сокет или null; error — причина отказа.
        /// </summary>
        public static Socket TryConnect(IPAddress target, int port, IPAddress local, int timeoutMs, out SocketError error)
        {
            error = SocketError.TimedOut;
            var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
            try {
                if (local != null) s.Bind(new IPEndPoint(local, 0));
                IAsyncResult ar = s.BeginConnect(new IPEndPoint(target, port), null, null);
                if (!ar.AsyncWaitHandle.WaitOne(timeoutMs)) {
                    s.Close();                                    // не дождались — бросаем попытку
                    return null;
                }
                s.EndConnect(ar);
                error = SocketError.Success;
                return s;
            } catch (SocketException e) {
                error = e.SocketErrorCode;
            } catch (Exception) {
                error = SocketError.SocketError;
            }
            s.Close();
            return null;
        }

        /// <summary>
        /// Проверка «жива ли связь» каждые 5 с, когда данных нет: иначе выключенную плату Windows заметит
        /// только через 2 часа. Обрыв увидим примерно через 15 с — и подключимся снова.
        /// </summary>
        public static void KeepAlive(Socket s)
        {
            try {
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
                var v = new byte[12];
                BitConverter.GetBytes(1u).CopyTo(v, 0);        // включить
                BitConverter.GetBytes(5000u).CopyTo(v, 4);     // первая проверка после 5 с тишины
                BitConverter.GetBytes(1000u).CopyTo(v, 8);     // дальше — раз в секунду
                s.IOControl(IOControlCode.KeepAliveValues, v, null);
            } catch (Exception) {
                // не вышло — не страшно: обрыв всё равно заметим, только позже
            }
        }

        /// <summary>Одна попытка подключения при поиске: сокет, куда и откуда.</summary>
        sealed class Probe
        {
            public Socket Socket;
            public IPAddress Target, Local;
            public IAsyncResult Connect;
        }

        /// <summary>
        /// Поиск плат во всех сетях ПК. Все подключения запускаются разом, ждём ответа не дольше
        /// connectMs, затем у ответивших читаем первую строку не дольше readMs. Обычно — 1–2 секунды.
        /// port и targets меняет только проверка на ПК; программа берёт 8268 и сети своих адаптеров.
        /// </summary>
        public static List<FoundBoard> FindBoards(int connectMs = 1500, int readMs = 1500, int port = UpdatePort,
                                                  IList<KeyValuePair<IPAddress, IPAddress>> targets = null)
        {
            if (targets == null) targets = ScanTargets();
            var probes = new List<Probe>();
            var found = new List<FoundBoard>();
            try {
                foreach (var t in targets) {
                    var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
                    var p = new Probe { Socket = s, Target = t.Key, Local = t.Value };
                    try {
                        if (t.Value != null) s.Bind(new IPEndPoint(t.Value, 0));
                        p.Connect = s.BeginConnect(new IPEndPoint(t.Key, port), null, null);
                    } catch (Exception) {
                        s.Close();
                        continue;
                    }
                    probes.Add(p);
                }
                // Ждём, пока все попытки закончатся (ответ или отказ), но не дольше connectMs.
                var clock = Stopwatch.StartNew();
                while (clock.ElapsedMilliseconds < connectMs) {
                    bool pending = false;
                    foreach (Probe p in probes) if (!p.Connect.IsCompleted) { pending = true; break; }
                    if (!pending) break;
                    Thread.Sleep(30);
                }
                var seen = new HashSet<string>();
                foreach (Probe p in probes) {
                    if (!p.Connect.IsCompleted) continue;
                    try { p.Socket.EndConnect(p.Connect); } catch (Exception) { continue; }
                    if (seen.Contains(p.Target.ToString())) continue;     // та же плата через второй адаптер
                    string line = ReadLine(p.Socket, readMs);
                    if (line == null || !line.StartsWith(Banner, StringComparison.Ordinal)) continue;
                    string[] parts = line.Split(' ');
                    seen.Add(p.Target.ToString());
                    found.Add(new FoundBoard { Address = p.Target, Local = p.Local, Version = parts.Length > 1 ? parts[1] : "?" });
                }
            } finally {
                foreach (Probe p in probes) {
                    try { p.Socket.Close(); } catch (Exception) { }
                }
            }
            return found;
        }

        /// <summary>Пары «адрес для проверки → адрес ПК, с которого стучаться» по всем сетям ПК.</summary>
        public static List<KeyValuePair<IPAddress, IPAddress>> ScanTargets()
        {
            var list = new List<KeyValuePair<IPAddress, IPAddress>>();
            foreach (LocalNet n in LocalNets()) {
                // Большие сети (маска короче /24) — проверяем только свои 254 адреса вокруг ПК:
                // перебор 65 тысяч адресов шёл бы минуты, а домашние сети почти всегда /24.
                uint mask = n.Mask | 0xFFFFFF00u;
                if (mask == 0xFFFFFFFFu || mask == 0xFFFFFFFEu) continue;  // сеть из одного-двух адресов
                uint net = n.Ip & mask, broadcast = net | ~mask;
                for (uint a = net + 1; a < broadcast && list.Count < MaxTargets; a++)
                    if (a != n.Ip) list.Add(new KeyValuePair<IPAddress, IPAddress>(FromUint(a), n.Address));
            }
            return list;
        }

        /// <summary>Первая строка ответа (до «\n», не длиннее 200 байтов) или null, если не пришла за timeoutMs.</summary>
        static string ReadLine(Socket s, int timeoutMs)
        {
            var bytes = new List<byte>();
            var one = new byte[256];
            var clock = Stopwatch.StartNew();
            try {
                while (clock.ElapsedMilliseconds < timeoutMs && bytes.Count < 200) {
                    int left = (int)(timeoutMs - clock.ElapsedMilliseconds);
                    if (left <= 0 || !s.Poll(left * 1000, SelectMode.SelectRead)) break;
                    int n = s.Receive(one);
                    if (n <= 0) break;
                    for (int i = 0; i < n; i++) {
                        if (one[i] == '\n') return Encoding.ASCII.GetString(bytes.ToArray()).TrimEnd('\r');
                        bytes.Add(one[i]);
                    }
                }
            } catch (Exception) { }
            return null;
        }
    }
}
