// SPDX-License-Identifier: Apache-2.0
// USB-JTAG модуля Tang Nano 20K: прошивка FPGA Partner Sipeed в BL616 (с 0x0 его flash) выдаёт себя за
// FT2232 (VID 0403, PID 6010; канал A — JTAG ПЛИС в режиме MPSSE). Драйвер FTDI (ftd2xx.dll) Windows
// ставит сама при первом подключении (Центр обновления); его же использует Gowin Programmer.
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Vdac4Flasher
{
    /// <summary>Поток команд MPSSE и ответов: настоящий FT2232 (Ftd2xx) или модель на ПК.</summary>
    public interface IMpsse : IDisposable
    {
        void Write(byte[] data, int count);
        /// <summary>Ровно count байтов ответа, иначе исключение по сроку.</summary>
        void Read(byte[] data, int count, int timeoutMs);
        string Serial { get; }
        string Description { get; }
    }

    public class Ftd2xx : IMpsse
    {
        const string Dll = "ftd2xx.dll";
        [DllImport(Dll)] static extern int FT_CreateDeviceInfoList(out uint num);
        [DllImport(Dll)] static extern int FT_GetDeviceInfoDetail(uint index, out uint flags, out uint type, out uint id,
            out uint locId, byte[] serial, byte[] description, out IntPtr handle);
        [DllImport(Dll)] static extern int FT_Open(int index, out IntPtr handle);
        [DllImport(Dll)] static extern int FT_Close(IntPtr h);
        [DllImport(Dll)] static extern int FT_ResetDevice(IntPtr h);
        [DllImport(Dll)] static extern int FT_SetUSBParameters(IntPtr h, uint inSize, uint outSize);
        [DllImport(Dll)] static extern int FT_SetChars(IntPtr h, byte ev, byte evEn, byte err, byte errEn);
        [DllImport(Dll)] static extern int FT_SetTimeouts(IntPtr h, uint read, uint write);
        [DllImport(Dll)] static extern int FT_SetLatencyTimer(IntPtr h, byte ms);
        [DllImport(Dll)] static extern int FT_SetBitMode(IntPtr h, byte mask, byte mode);
        [DllImport(Dll)] static extern int FT_Purge(IntPtr h, uint mask);
        [DllImport(Dll)] static extern int FT_Write(IntPtr h, byte[] buf, uint len, out uint written);
        [DllImport(Dll)] static extern int FT_Read(IntPtr h, byte[] buf, uint len, out uint got);
        [DllImport(Dll)] static extern int FT_GetQueueStatus(IntPtr h, out uint rx);

        IntPtr handle;
        public string Serial { get; private set; }
        public string Description { get; private set; }

        /// <summary>Есть ли USB-JTAG Tang (0403:6010), не открывая его: описание или null; why — почему нет.</summary>
        public static string Present(out string why)
        {
            why = "";
            uint num;
            try {
                if (FT_CreateDeviceInfoList(out num) != 0) return null;
            } catch (DllNotFoundException) {
                why = "ftd2xx.dll";
                return null;
            }
            for (uint i = 0; i < num; i++) {
                uint flags, type, id, loc;
                IntPtr h;
                var ser = new byte[16];
                var desc = new byte[64];
                if (FT_GetDeviceInfoDetail(i, out flags, out type, out id, out loc, ser, desc, out h) == 0 && id == 0x04036010)
                    return Z(desc);
            }
            return null;
        }

        /// <summary>Канал A устройства 0403:6010 (USB-JTAG Tang). null — нет такого устройства.</summary>
        public static Ftd2xx OpenTang(out string why)
        {
            why = "";
            uint num;
            try {
                if (FT_CreateDeviceInfoList(out num) != 0) { why = "FT_CreateDeviceInfoList"; return null; }
            } catch (DllNotFoundException) {
                why = Lang.T("нет драйвера FTDI (ftd2xx.dll). Windows ставит его сама при первом подключении Tang, если есть интернет; иначе установите «CDM driver» с ftdichip.com",
                             "no FTDI driver (ftd2xx.dll). Windows installs it by itself on the first Tang plug-in when online; otherwise install the CDM driver from ftdichip.com");
                return null;
            }
            // канал A: описание кончается на «A»; нет такого — первый 0403:6010 (каналы перечисляются A, B)
            int best = -1;
            string bestSerial = "", bestDesc = "";
            for (uint i = 0; i < num; i++) {
                uint flags, type, id, loc;
                IntPtr h;
                var ser = new byte[16];
                var desc = new byte[64];
                if (FT_GetDeviceInfoDetail(i, out flags, out type, out id, out loc, ser, desc, out h) != 0) continue;
                if (id != 0x04036010) continue;
                string d = Z(desc), s = Z(ser);
                if (best < 0 || (d.EndsWith("A") && !bestDesc.EndsWith("A"))) { best = (int)i; bestSerial = s; bestDesc = d; }
            }
            if (best < 0) { why = Lang.T("USB-JTAG Tang (0403:6010) не найден", "Tang USB-JTAG (0403:6010) not found"); return null; }
            IntPtr hh;
            if (FT_Open(best, out hh) != 0) {
                why = Lang.T("USB-JTAG Tang занят другой программой (Gowin Programmer?)", "Tang USB-JTAG is busy (Gowin Programmer?)");
                return null;
            }
            var dev = new Ftd2xx { handle = hh, Serial = bestSerial, Description = bestDesc };
            dev.Setup();
            return dev;
        }

        static string Z(byte[] b)
        {
            int n = Array.IndexOf(b, (byte)0);
            return Encoding.ASCII.GetString(b, 0, n < 0 ? b.Length : n).Trim();
        }

        void Setup()
        {
            Check(FT_ResetDevice(handle), "FT_ResetDevice");
            Check(FT_SetUSBParameters(handle, 65536, 65536), "FT_SetUSBParameters");
            FT_SetChars(handle, 0, 0, 0, 0);
            Check(FT_SetTimeouts(handle, 3000, 3000), "FT_SetTimeouts");
            Check(FT_SetLatencyTimer(handle, 2), "FT_SetLatencyTimer");
            Check(FT_SetBitMode(handle, 0x00, 0x00), "FT_SetBitMode reset");
            Check(FT_SetBitMode(handle, 0x0B, 0x02), "FT_SetBitMode MPSSE");
            Thread.Sleep(50);
            FT_Purge(handle, 3);
        }

        static void Check(int st, string what)
        {
            if (st != 0) throw new FlashException("FTDI " + what + ": " + st);
        }

        public void Write(byte[] data, int count)
        {
            uint done;
            var buf = data;
            if (count != data.Length) { buf = new byte[count]; Array.Copy(data, buf, count); }
            Check(FT_Write(handle, buf, (uint)count, out done), "FT_Write");
            if (done != count) throw new FlashException("FTDI FT_Write: " + done + "/" + count);
        }

        public void Read(byte[] data, int count, int timeoutMs)
        {
            int got = 0, deadline = Environment.TickCount + timeoutMs;
            var tmp = new byte[count];
            while (got < count) {
                uint q;
                Check(FT_GetQueueStatus(handle, out q), "FT_GetQueueStatus");
                if (q > 0) {
                    uint n;
                    Check(FT_Read(handle, tmp, (uint)Math.Min(q, (uint)(count - got)), out n), "FT_Read");
                    Array.Copy(tmp, 0, data, got, (int)n);
                    got += (int)n;
                    continue;
                }
                if (Environment.TickCount - deadline >= 0)
                    throw new FlashException(Lang.T("USB-JTAG Tang не отвечает", "Tang USB-JTAG does not answer") +
                                             " (" + got + "/" + count + ")");
                Thread.Sleep(0);
            }
        }

        public void Dispose()
        {
            if (handle != IntPtr.Zero) {
                try { FT_SetBitMode(handle, 0x00, 0x00); } catch (Exception) { }
                FT_Close(handle);
                handle = IntPtr.Zero;
            }
        }
    }
}
