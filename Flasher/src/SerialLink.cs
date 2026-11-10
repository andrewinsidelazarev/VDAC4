// SPDX-License-Identifier: Apache-2.0
// Канал байтов до устройства: COM-порт USB CDC (ESP32-S3 — USB-Serial/JTAG, BL616 — CDC ПЗУ) и поиск
// портов по VID/PID. Для проверки на ПК тот же интерфейс реализуют модели устройств.
using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Management;
using System.Text.RegularExpressions;
using System.Threading;

namespace Vdac4Flasher
{
    public interface IByteLink : IDisposable
    {
        void Write(byte[] data, int offset, int count);
        /// <summary>Что пришло, но не больше max; ждёт первого байта до timeoutMs. 0 — ничего.</summary>
        int Read(byte[] buffer, int offset, int max, int timeoutMs);
        void SetDtr(bool on);
        void SetRts(bool on);
        void Discard();
    }

    public class SerialLink : IByteLink
    {
        readonly SerialPort port;

        public SerialLink(string name, int baud)
        {
            port = new SerialPort(name, baud, Parity.None, 8, StopBits.One);
            port.Handshake = Handshake.None;
            port.ReadBufferSize = 1 << 20;
            port.WriteBufferSize = 1 << 16;
            port.WriteTimeout = 5000;
            port.DtrEnable = false;
            port.RtsEnable = false;
            port.Open();
        }

        public void Write(byte[] data, int offset, int count) { port.Write(data, offset, count); }

        public int Read(byte[] buffer, int offset, int max, int timeoutMs)
        {
            int deadline = Environment.TickCount + timeoutMs;
            while (port.BytesToRead == 0) {
                if (Environment.TickCount - deadline >= 0) return 0;
                Thread.Sleep(1);
            }
            return port.Read(buffer, offset, Math.Min(max, port.BytesToRead));
        }

        public void SetDtr(bool on) { port.DtrEnable = on; }
        public void SetRts(bool on) { port.RtsEnable = on; }
        public void Discard() { port.DiscardInBuffer(); }

        public void Dispose()
        {
            try { port.Close(); } catch (Exception) { }
            port.Dispose();
        }
    }

    /// <summary>Порт, найденный по VID/PID USB.</summary>
    public class FoundPort
    {
        public string Com;           // «COM5»
        public string Name;          // имя устройства в Windows
        public ushort Vid, Pid;
        public override string ToString() { return Com + " — " + Name; }
    }

    public static class Ports
    {
        static readonly Regex ComRx = new Regex(@"\((COM\d+)\)");
        static readonly Regex VidPidRx = new Regex(@"VID_([0-9A-F]{4})&PID_([0-9A-F]{4})", RegexOptions.IgnoreCase);

        /// <summary>Все COM-порты USB с VID/PID (через WMI: Win32_PnPEntity).</summary>
        public static List<FoundPort> List()
        {
            var list = new List<FoundPort>();
            try {
                using (var s = new ManagementObjectSearcher(
                           "SELECT Name, DeviceID FROM Win32_PnPEntity WHERE Name LIKE '%(COM%'")) {
                    foreach (ManagementObject o in s.Get()) {
                        string name = (string)o["Name"] ?? "", id = (string)o["DeviceID"] ?? "";
                        var com = ComRx.Match(name);
                        var vp = VidPidRx.Match(id);
                        if (!com.Success) continue;
                        list.Add(new FoundPort {
                            Com = com.Groups[1].Value, Name = name.Replace(com.Value, "").Trim(),
                            Vid = vp.Success ? Convert.ToUInt16(vp.Groups[1].Value, 16) : (ushort)0,
                            Pid = vp.Success ? Convert.ToUInt16(vp.Groups[2].Value, 16) : (ushort)0
                        });
                    }
                }
            } catch (Exception) {
                // WMI недоступен: хотя бы имена портов
                foreach (string p in SerialPort.GetPortNames()) list.Add(new FoundPort { Com = p, Name = "" });
            }
            return list;
        }

        /// <summary>ESP32-S3 по собственному USB (USB-Serial/JTAG): VID 303A, PID 1001.</summary>
        public static FoundPort FindEsp()
        {
            foreach (var p in List()) if (p.Vid == 0x303A && p.Pid == 0x1001) return p;
            return null;
        }

        /// <summary>BL616 в ПЗУ ISP (кнопка UPDATE при подключении): CDC Bouffalo Lab. На плате 10.11.2026 ПЗУ
        /// Tang Nano 20K — VID 349B (Bouffalo Lab), PID 6160, имя порта в Windows обычное («Устройство с
        /// последовательным интерфейсом USB»); VID FFFF и 42BF и «Bouffalo» в имени — у других ПЗУ и драйверов.
        /// В обычном режиме (FPGA Partner) BL616 — USB-JTAG FTDI 0403:6010, сюда не попадает.</summary>
        public static FoundPort FindBl616()
        {
            foreach (var p in List())
                if (p.Vid == 0x349B || p.Vid == 0xFFFF || p.Vid == 0x42BF ||
                    p.Name.IndexOf("Bouffalo", StringComparison.OrdinalIgnoreCase) >= 0)
                    return p;
            return null;
        }
    }
}
