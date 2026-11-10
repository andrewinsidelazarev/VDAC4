// SPDX-License-Identifier: Apache-2.0
// Модели устройств для проверки программы на ПК: ПЗУ ESP32-S3 (SLIP, команды загрузчика, zlib, MD5),
// ПЗУ ISP BL616 (кадры, суммы, «PD» при стирании, SHA-256 по XIP) и FT2232 MPSSE → ТАП GW2AR-18 → мост
// SPI 0x16 → SPI NOR flash. Модели строгие: неизвестная команда, неверная сумма или запись без WEL —
// ошибка теста, а не тихий пропуск.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using Vdac4Flasher;

namespace FlasherTests
{
    public class ModelError : Exception { public ModelError(string m) : base(m) { } }

    /// <summary>Очередь ответов устройства для IByteLink.Read.</summary>
    public abstract class LinkModel : IByteLink
    {
        protected readonly Queue<byte> outq = new Queue<byte>();
        public bool Dtr, Rts;
        public virtual void SetDtr(bool on) { Dtr = on; }
        public virtual void SetRts(bool on) { Rts = on; }
        public void Discard() { outq.Clear(); }
        public void Dispose() { }
        public abstract void Write(byte[] data, int offset, int count);

        public virtual int Read(byte[] buffer, int offset, int max, int timeoutMs)
        {
            int n = 0;
            while (n < max && outq.Count > 0) buffer[offset + n++] = outq.Dequeue();
            return n;
        }

        protected void Emit(byte[] b) { foreach (var x in b) outq.Enqueue(x); }
    }

    // ---- ПЗУ ESP32-S3 ---------------------------------------------------------------------------

    public class EspRomModel : LinkModel
    {
        public readonly byte[] Flash;
        public uint FlashSizeParam;
        public bool Attached, InDownload;
        public int Syncs, Md5s, Erases, Reads;
        public bool CorruptChecksumOnce;          // ответить «ошибка суммы» на первый DEFL_DATA
        public int CorruptReadAt = -1;            // READ_FLASH_SLOW с этого адреса вернёт испорченный байт
        public int RefuseReadAt = -1;             // READ_FLASH_SLOW с этого адреса — отказ ПЗУ
        public int Resets;
        // Сторожа RTC (10.11.2026, на плате бэкап обрывался через ~9 с): сброс через USB-Serial/JTAG их не
        // сбрасывает. Пока RWDT включён или SWD не кормится сам, через WatchdogCommands команд после входа в
        // загрузчик чип перезапускается: USB пропадает, порт отвечает IOException, как Windows.
        public uint WdtConfig0 = 0xD0000502, WdtProtect, SwdConf = 0x04B00000, SwdProtect;
        public uint Option1;                      // RTC_CNTL_OPTION1: бит 0 — «загрузчик принудительно» (ставит сброс USB)
        public int WatchdogCommands = 3000;
        public int DieAtCommand = -1;             // на этой команде после входа в загрузчик USB пропадает (кабель)
        public int WatchdogResets;
        public bool Dead;
        int commands;
        readonly List<byte> frame = new List<byte>();
        bool inFrame, esc;
        readonly MemoryStream defl = new MemoryStream();
        uint deflOffset, deflBlocks, deflSize, deflSeq;
        bool prevRts;

        public EspRomModel(int flashBytes, byte fill)
        {
            Flash = new byte[flashBytes];
            for (int i = 0; i < Flash.Length; i++) Flash[i] = (byte)(fill ^ i * 7);
        }

        bool armed;

        public override void SetRts(bool on)
        {
            // RTS — сброс; на отпускании: взведён IO0 (DTR = 1 при RTS = 0) — загрузчик и флаг «загрузчик
            // принудительно» в RTC (его сброс через USB не снимает); без IO0 — по этому флагу, иначе своя прошивка
            base.SetRts(on);
            if (prevRts && !on) {
                if (armed) Option1 |= 1;
                InDownload = armed || (Option1 & 1) != 0;
                armed = false;
                Resets++;
                Dead = false;
                commands = 0;
                outq.Clear();
                if (InDownload) Emit(Encoding.ASCII.GetBytes("ESP-ROM:esp32s3-20210327\r\nwaiting for download\r\n"));
            }
            prevRts = on;
        }

        public override int Read(byte[] buffer, int offset, int max, int timeoutMs)
        {
            if (Dead) throw new IOException("Присоединенное к системе устройство не работает.");
            return base.Read(buffer, offset, max, timeoutMs);
        }

        /// <summary>Сторож или кабель: чип перезапустился посреди работы — USB пропал.</summary>
        void Die(bool watchdog)
        {
            Dead = true;
            InDownload = false;
            outq.Clear();
            if (watchdog) WatchdogResets++;
            throw new IOException("Присоединенное к системе устройство не работает.");
        }

        public override void SetDtr(bool on)
        {
            base.SetDtr(on);
            if (on && !Rts) armed = true;
        }

        public override void Write(byte[] data, int offset, int count)
        {
            if (Dead) throw new IOException("Присоединенное к системе устройство не работает.");
            for (int i = offset; i < offset + count; i++) {
                byte b = data[i];
                if (!inFrame) { if (b == 0xC0) { inFrame = true; frame.Clear(); } continue; }
                if (b == 0xC0) { if (frame.Count == 0) continue; inFrame = false; Handle(frame.ToArray()); continue; }
                if (esc) { frame.Add(b == 0xDC ? (byte)0xC0 : b == 0xDD ? (byte)0xDB : b); esc = false; }
                else if (b == 0xDB) esc = true;
                else frame.Add(b);
            }
        }

        void Reply(byte op, uint value, byte[] data, byte status = 0, byte error = 0)
        {
            var body = new List<byte> { 0x01, op };
            int len = data.Length + 4;
            body.Add((byte)len); body.Add((byte)(len >> 8));
            body.Add((byte)value); body.Add((byte)(value >> 8)); body.Add((byte)(value >> 16)); body.Add((byte)(value >> 24));
            body.AddRange(data);
            body.Add(status); body.Add(error); body.Add(0); body.Add(0);
            var o = new List<byte> { 0xC0 };
            foreach (byte x in body) {
                if (x == 0xC0) { o.Add(0xDB); o.Add(0xDC); } else if (x == 0xDB) { o.Add(0xDB); o.Add(0xDD); } else o.Add(x);
            }
            o.Add(0xC0);
            Emit(o.ToArray());
        }

        static uint W(byte[] p, int i) { return Util.Get32(p, 8 + i * 4); }

        void Handle(byte[] p)
        {
            if (p.Length < 8 || p[0] != 0) throw new ModelError("ESP: не запрос");
            byte op = p[1];
            int len = Util.Get16(p, 2);
            if (p.Length != 8 + len) throw new ModelError("ESP: длина пакета");
            if (!InDownload) return;                                  // своя прошивка: пакеты загрузчика не слышит
            commands++;
            if (commands == DieAtCommand) Die(false);
            if (commands > WatchdogCommands && ((WdtConfig0 & 0x80000000u) != 0 || (SwdConf & EspRom.SWD_AUTO_FEED_EN) == 0)) Die(true);
            switch (op) {
            case 0x08:
                if (len != 36 || p[8] != 7 || p[9] != 7 || p[10] != 0x12 || p[11] != 0x20) throw new ModelError("ESP: SYNC");
                Syncs++;
                for (int i = 0; i < 8; i++) Reply(op, 0, new byte[0]);
                break;
            case 0x09: {
                // WRITE_REG [адрес][значение][маска][задержка]: регистры сторожей — только с ключом защиты
                if (len != 16) throw new ModelError("ESP: WRITE_REG");
                uint addr = W(p, 0), val = W(p, 1), mask = W(p, 2);
                if (addr == EspRom.WDTWPROTECT) WdtProtect = val;
                else if (addr == EspRom.WDTCONFIG0) {
                    if (WdtProtect != EspRom.WDT_WKEY) throw new ModelError("ESP: RWDT без ключа защиты");
                    WdtConfig0 = WdtConfig0 & ~mask | val & mask;
                } else if (addr == EspRom.SWD_WPROTECT) SwdProtect = val;
                else if (addr == EspRom.SWD_CONF) {
                    if (SwdProtect != EspRom.SWD_WKEY) throw new ModelError("ESP: SWD без ключа защиты");
                    SwdConf = SwdConf & ~mask | val & mask;
                } else if (addr == EspRom.OPTION1) Option1 = Option1 & ~mask | val & mask;
                else throw new ModelError("ESP: WRITE_REG 0x" + addr.ToString("X8"));
                Reply(op, 0, new byte[0]);
                break;
            }
            case 0x0A: {
                uint addr = W(p, 0);
                Reply(op, addr == 0x40001000 ? 9u : addr == EspRom.SWD_CONF ? SwdConf : addr == EspRom.OPTION1 ? Option1 :
                          addr == EspRom.WDTCONFIG0 ? WdtConfig0 : 0u, new byte[0]);
                break;
            }
            case 0x0D:
                if (len != 8) throw new ModelError("ESP: SPI_ATTACH");
                Attached = true; Reply(op, 0, new byte[0]);
                break;
            case 0x0B:
                if (len != 24) throw new ModelError("ESP: SPI_SET_PARAMS");
                FlashSizeParam = W(p, 1); Reply(op, 0, new byte[0]);
                break;
            case 0x02: {
                if (len != 20 || !Attached) throw new ModelError("ESP: FLASH_BEGIN");
                uint size = W(p, 0), off = W(p, 3);
                if (off % 4096 != 0 || off + size > FlashSizeParam) throw new ModelError("ESP: FLASH_BEGIN за краем");
                EraseRange(off, size);
                Erases++;
                Reply(op, 0, new byte[0]);
                break;
            }
            case 0x10: {
                if (len != 20 || !Attached) throw new ModelError("ESP: FLASH_DEFL_BEGIN");
                deflSize = W(p, 0); deflBlocks = W(p, 1); deflOffset = W(p, 3); deflSeq = 0;
                if (W(p, 2) != 0x400 || deflSize % 0x400 != 0) throw new ModelError("ESP: DEFL_BEGIN блок");
                EraseRange(deflOffset, deflSize);
                defl.SetLength(0);
                Reply(op, 0, new byte[0]);
                break;
            }
            case 0x11: {
                uint n = W(p, 0), seq = W(p, 1);
                if (seq != deflSeq || n != len - 16) throw new ModelError("ESP: DEFL_DATA номер/длина");
                byte chk = 0xEF;
                for (int i = 0; i < n; i++) chk ^= p[24 + i];
                if (chk != (byte)Util.Get32(p, 4) || CorruptChecksumOnce) {
                    CorruptChecksumOnce = false;
                    Reply(op, 0, new byte[0], 1, 0xC1);
                    break;
                }
                defl.Write(p, 24, (int)n);
                deflSeq++;
                if (deflSeq == deflBlocks) Inflate();
                Reply(op, 0, new byte[0]);
                break;
            }
            case 0x0E: {
                // READ_FLASH_SLOW: [адрес][длина ≤ 64]; ответ — всегда 64 байта (так у ПЗУ, см. esptool)
                if (len != 8 || !Attached) throw new ModelError("ESP: READ_FLASH_SLOW");
                uint off = W(p, 0), n = W(p, 1);
                if (n == 0 || n > 64 || off + n > FlashSizeParam) throw new ModelError("ESP: READ_FLASH_SLOW за краем");
                Reads++;
                if (off == RefuseReadAt) { Reply(op, 0, new byte[64], 1, 0x05); break; }
                var d = new byte[64];
                Array.Copy(Flash, off, d, 0, n);
                if (off == CorruptReadAt) d[3] ^= 0x40;
                Reply(op, 0, d);
                break;
            }
            case 0x13: {
                uint off = W(p, 0), size = W(p, 1);
                Md5s++;
                using (var h = MD5.Create()) {
                    string hex = Util.Hex(h.ComputeHash(Flash, (int)off, (int)size));
                    Reply(op, 0, Encoding.ASCII.GetBytes(hex));
                }
                break;
            }
            default:
                throw new ModelError("ESP: команда 0x" + op.ToString("X2"));
            }
        }

        void EraseRange(uint off, uint size)
        {
            uint a = off & ~0xFFFu, e = (off + size + 0xFFF) & ~0xFFFu;
            for (uint i = a; i < e && i < Flash.Length; i++) Flash[i] = 0xFF;
        }

        void Inflate()
        {
            byte[] z = defl.ToArray();
            if (z.Length < 6 || z[0] != 0x78 || ((z[0] << 8) | z[1]) % 31 != 0) throw new ModelError("ESP: заголовок zlib");
            byte[] raw;
            using (var ms = new MemoryStream(z, 2, z.Length - 6))
            using (var d = new DeflateStream(ms, CompressionMode.Decompress))
            using (var o = new MemoryStream()) { d.CopyTo(o); raw = o.ToArray(); }
            uint a = 1, b = 0;
            foreach (byte x in raw) { a = (a + x) % 65521; b = (b + a) % 65521; }
            uint adler = (uint)(z[z.Length - 4] << 24 | z[z.Length - 3] << 16 | z[z.Length - 2] << 8 | z[z.Length - 1]);
            if (adler != (b << 16 | a)) throw new ModelError("ESP: Adler-32");
            if (raw.Length > deflSize) throw new ModelError("ESP: распаковано больше стёртого");
            for (int i = 0; i < raw.Length; i++) {
                if (Flash[deflOffset + i] != 0xFF) throw new ModelError("ESP: запись в нестёртое");
                Flash[deflOffset + i] = raw[i];
            }
        }
    }

    // ---- ПЗУ ISP BL616 --------------------------------------------------------------------------

    public class Bl616RomModel : LinkModel
    {
        public readonly byte[] Flash = new byte[4 << 20];
        public bool Synced;
        public int Syncs55, Erases, Writes, Reads, PdLeft = 2;
        public uint EraseFrom, EraseTo;
        public uint FlashSet4, FlashSet88;
        public uint IspTimeout;
        public bool CheckSent, XipOn;
        readonly List<byte> rx = new List<byte>();
        static readonly byte[] BootInfo = Hex("0100160600010100279280017582f70286241c000f758010");

        public Bl616RomModel()
        {
            for (int i = 0; i < 0x40000; i++) Flash[i] = (byte)(0x5A ^ i);         // FPGA Partner Sipeed
            for (int i = 0x40000; i < Flash.Length; i++) Flash[i] = (byte)(i >> 3);  // прежняя прошивка
        }

        static byte[] Hex(string s)
        {
            var b = new byte[s.Length / 2];
            for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(2 * i, 2), 16);
            return b;
        }

        void Ok() { Emit(new byte[] { 0x4F, 0x4B }); }
        void Fail(ushort code) { Emit(new byte[] { 0x46, 0x4C, (byte)code, (byte)(code >> 8) }); }
        void Data(byte[] d) { Ok(); Emit(new byte[] { (byte)d.Length, (byte)(d.Length >> 8) }); Emit(d); }

        public override void Write(byte[] data, int offset, int count)
        {
            for (int i = offset; i < offset + count; i++) rx.Add(data[i]);
            while (true) {
                if (!Synced) {
                    int n55 = 0;
                    while (n55 < rx.Count && rx[n55] == 0x55) n55++;
                    if (n55 >= 50) { Synced = true; Syncs55++; rx.RemoveRange(0, n55); Ok(); continue; }
                    // до синхронизации ПЗУ всё прочее пропускает
                    if (n55 == rx.Count) return;
                    rx.RemoveRange(0, Math.Max(1, n55));
                    continue;
                }
                while (rx.Count > 0 && rx[0] == 0x55) rx.RemoveAt(0);   // хвост синхронизации
                if (rx.Count < 4) return;
                int len = rx[2] | rx[3] << 8;
                if (rx.Count < 4 + len) return;
                byte cmd = rx[0], chk = rx[1];
                byte[] p = rx.GetRange(4, len).ToArray();
                rx.RemoveRange(0, 4 + len);
                Handle(cmd, chk, p);
            }
        }

        void Handle(byte cmd, byte chk, byte[] p)
        {
            bool romCmd = cmd == 0x10 || cmd == 0x50 || cmd == 0x23;
            int s = (byte)p.Length + (byte)(p.Length >> 8);
            foreach (byte b in p) s += b;
            if (!romCmd && chk != (byte)s) { Fail(0x0103); return; }       // CMD_CRC_ERROR
            switch (cmd) {
            case 0x10: Data(BootInfo); break;
            case 0x50:
                if (p.Length != 8 || Util.Get32(p, 0) != 0x6102DF04) throw new ModelError("BL616: 0x50");
                IspTimeout = Util.Get32(p, 4) >> 16; Ok(); break;
            case 0x23: throw new ModelError("BL616: A0 — 0x23 не ждём");
            case 0x22: if (p.Length != 8 || Util.Get32(p, 4) != 2000000) throw new ModelError("BL616: 0x22"); Ok(); break;
            case 0x3B:
                if (p.Length == 4) FlashSet4 = Util.Get32(p, 0);
                else if (p.Length == 88) { FlashSet88 = Util.Get32(p, 0); if (p[4 + 13] != 0xC8) throw new ModelError("BL616: flash_para"); }
                else throw new ModelError("BL616: 0x3B длина");
                Ok(); break;
            case 0x36: if (FlashSet4 == 0) throw new ModelError("BL616: JEDEC до 0x3B"); Data(new byte[] { 0xC8, 0x60, 0x16, 0x00 }); break;
            case 0x30: {
                if (FlashSet88 == 0) throw new ModelError("BL616: стирание до flash_para");
                uint a = Util.Get32(p, 0), e = Util.Get32(p, 4);
                if (e < a) throw new ModelError("BL616: конец стирания");
                for (uint i = a & ~0xFFFu; i <= (e | 0xFFF); i++) Flash[i] = 0xFF;
                Erases++;
                EraseFrom = a; EraseTo = e;
                while (PdLeft-- > 0) Emit(new byte[] { 0x50, 0x44 });
                Ok(); break;
            }
            case 0x31: {
                uint a = Util.Get32(p, 0);
                if (p.Length - 4 > 2048) throw new ModelError("BL616: кусок длиннее 2048");
                for (int i = 4; i < p.Length; i++) {
                    if (Flash[a + i - 4] != 0xFF) throw new ModelError("BL616: запись в нестёртое 0x" + (a + i - 4).ToString("X"));
                    Flash[a + i - 4] = p[i];
                }
                Writes++; Ok(); break;
            }
            case 0x32: {
                // flash_read: [адрес][длина ≤ 2048] → «OK», длина, данные
                if (p.Length != 8 || FlashSet88 == 0) throw new ModelError("BL616: 0x32");
                uint a = Util.Get32(p, 0), n = Util.Get32(p, 4);
                if (n == 0 || n > 2048 || a + n > Flash.Length) throw new ModelError("BL616: 0x32 длина");
                var d = new byte[n];
                Array.Copy(Flash, a, d, 0, n);
                Reads++;
                Data(d);
                break;
            }
            case 0x3A: CheckSent = true; Ok(); break;
            case 0x60: XipOn = true; Ok(); break;
            case 0x3E: {
                if (!XipOn) throw new ModelError("BL616: SHA без XIP");
                uint a = Util.Get32(p, 0), n = Util.Get32(p, 4);
                using (var h = SHA256.Create()) Data(h.ComputeHash(Flash, (int)a, (int)n));
                break;
            }
            case 0x61: XipOn = false; Ok(); break;
            default: throw new ModelError("BL616: команда 0x" + cmd.ToString("X2"));
            }
        }
    }

    // ---- FT2232 MPSSE → ТАП GW2AR-18 → SPI flash ---------------------------------------------------

    public class SpiFlashModel
    {
        public readonly byte[] Mem;
        public uint Jedec = 0x0B4017;
        public bool Wel;
        public int Busy;                 // сколько ещё чтений состояния «занята»
        public byte Sr;                  // биты защиты
        public int Erases, Programs, Reads;
        readonly List<byte> cmd = new List<byte>();
        int bit;
        byte cur;


        public SpiFlashModel(int size)
        {
            Mem = new byte[size];
            for (int i = 0; i < size; i++) Mem[i] = (byte)(i * 13 + 5);
        }

        public void Select() { cmd.Clear(); bit = 0; cur = 0; }

        /// <summary>Такт SPI: принять бит MOSI, вернуть бит MISO (старшим вперёд).</summary>
        public int Clock(int mosi)
        {
            int miso = OutBit(cmd.Count, bit);
            cur = (byte)(cur << 1 | mosi);
            if (++bit == 8) { cmd.Add(cur); bit = 0; cur = 0; }
            return miso;
        }

        int OutBit(int byteIndex, int bitIndex)
        {
            if (cmd.Count == 0) return 1;
            byte op = cmd[0];
            byte v = 0xFF;
            if (op == 0x9F && byteIndex >= 1 && byteIndex <= 3) v = (byte)(Jedec >> (8 * (3 - byteIndex)));
            else if (op == 0x05 && byteIndex >= 1) v = Status();
            else if (op == 0x03 && byteIndex >= 4) v = Mem[(Addr() + byteIndex - 4) % Mem.Length];
            return v >> (7 - bitIndex) & 1;
        }

        byte Status() { return (byte)((Busy > 0 ? 1 : 0) | (Wel ? 2 : 0) | Sr); }
        uint Addr() { return (uint)(cmd[1] << 16 | cmd[2] << 8 | cmd[3]); }

        public void Deselect()
        {
            if (cmd.Count == 0) return;
            byte op = cmd[0];
            if (op == 0x05) { if (Busy > 0) Busy--; return; }
            if (Busy > 0 && op != 0x05) throw new ModelError("SPI: команда 0x" + op.ToString("X2") + " при занятой flash");
            switch (op) {
            case 0x06: Wel = true; break;
            case 0x04: Wel = false; break;
            case 0x9F: case 0xFF: case 0x66: case 0x99: break;
            case 0x03: Reads++; break;
            case 0x01:
                if (!Wel) throw new ModelError("SPI: WRSR без WEL");
                Sr = (byte)(cmd.Count > 1 ? cmd[1] & 0x7C : 0); Wel = false; Busy = 2; break;
            case 0x20: case 0xD8: {
                if (!Wel) throw new ModelError("SPI: стирание без WEL");
                if ((Sr & 0x7C) != 0) throw new ModelError("SPI: стирание под защитой");
                uint size = op == 0x20 ? 0x1000u : 0x10000u, a = Addr() & ~(size - 1);
                for (uint i = a; i < a + size; i++) Mem[i] = 0xFF;
                Erases++; Wel = false; Busy = 3; break;
            }
            case 0x02: {
                if (!Wel) throw new ModelError("SPI: запись без WEL");
                uint a = Addr();
                for (int i = 4; i < cmd.Count; i++) {
                    uint at = (a & ~0xFFu) | ((a + (uint)(i - 4)) & 0xFF);
                    Mem[at] &= cmd[i];
                }
                if (bit != 0) throw new ModelError("SPI: неполный байт");
                Programs++; Wel = false; Busy = 1; break;
            }
            default: throw new ModelError("SPI: команда 0x" + op.ToString("X2"));
            }
        }
    }

    public class GowinTapModel : IMpsse
    {
        public string Serial { get { return "2023112818"; } }
        public string Description { get { return "JTAG Debugger A"; } }
        public void Dispose() { }

        public readonly SpiFlashModel Flash = new SpiFlashModel(8 << 20);
        public uint Status = 1 << 13;                // Done: TS-Config работает
        public uint Divisor;
        public int Reloads;
        readonly Queue<byte> outq = new Queue<byte>();
        readonly List<byte> buf = new List<byte>();
        Tap state = Tap.Reset;
        byte ir = 0x11, irShift;
        ulong dr;
        int drBits;
        bool spiSelected;
        int spiPrevMiso = 1;
        bool tms = true;
        int tdiLevel;

        public void Write(byte[] data, int count)
        {
            for (int i = 0; i < count; i++) buf.Add(data[i]);
            int pos = 0;
            while (pos < buf.Count) {
                int used = Exec(pos);
                if (used == 0) break;
                pos += used;
            }
            buf.RemoveRange(0, pos);
        }

        public void Read(byte[] data, int count, int timeoutMs)
        {
            if (outq.Count < count) throw new ModelError("MPSSE: ждут " + count + " байтов, есть " + outq.Count);
            for (int i = 0; i < count; i++) data[i] = outq.Dequeue();
        }

        int Need(int pos, int n) { return pos + n <= buf.Count ? n : 0; }

        int Exec(int p)
        {
            byte op = buf[p];
            switch (op) {
            case 0x85: case 0x8A: case 0x97: case 0x8D: case 0x87: return 1;
            case 0x86: if (Need(p, 3) == 0) return 0; Divisor = (uint)(buf[p + 1] | buf[p + 2] << 8); return 3;
            case 0x80: case 0x82:
                if (Need(p, 3) == 0) return 0;
                if (op == 0x80) { tms = (buf[p + 1] & 0x08) != 0; tdiLevel = buf[p + 1] >> 1 & 1; }
                return 3;
            case 0x4B: case 0x6B: {
                if (Need(p, 3) == 0) return 0;
                int n = buf[p + 1] + 1, v = buf[p + 2];
                if (n > 7) throw new ModelError("MPSSE: TMS длиннее 7");
                int tdi = v >> 7 & 1, got = 0;
                for (int k = 0; k < n; k++) got |= Clock(v >> k & 1, tdi) << k;
                if (op == 0x6B) outq.Enqueue((byte)(got << (8 - n)));
                return 3;
            }
            case 0x39: {
                if (Need(p, 3) == 0) return 0;
                int n = (buf[p + 1] | buf[p + 2] << 8) + 1;
                if (Need(p, 3 + n) == 0) return 0;
                for (int k = 0; k < n; k++) {
                    int b = buf[p + 3 + k], r = 0;
                    for (int i = 0; i < 8; i++) r |= Clock(tms ? 1 : 0, b >> i & 1) << i;
                    outq.Enqueue((byte)r);
                }
                return 3 + n;
            }
            case 0x3B: {
                if (Need(p, 3) == 0) return 0;
                int n = buf[p + 1] + 1, b = buf[p + 2], r = 0;
                for (int i = 0; i < n; i++) r |= Clock(tms ? 1 : 0, b >> i & 1) << i;
                outq.Enqueue((byte)(r << (8 - n)));
                return 3;
            }
            default:
                throw new ModelError("MPSSE: команда 0x" + op.ToString("X2"));
            }
        }

        /// <summary>Такт TCK: TDO — до фронта, по фронту — сдвиг и переход ТАП.</summary>
        int Clock(int tmsBit, int tdi)
        {
            tms = tmsBit != 0;
            int tdo = 1;
            if (state == Tap.ShiftDr) {
                if (ir == 0x16) {
                    if (!spiSelected) throw new ModelError("SPI: такт без CS");
                    tdo = spiPrevMiso;
                    spiPrevMiso = Flash.Clock(tdi);
                } else {
                    tdo = (int)(dr & 1);
                    dr = (dr >> 1) | ((ulong)tdi << (drBits - 1));
                }
            } else if (state == Tap.ShiftIr) {
                tdo = irShift & 1;
                irShift = (byte)(irShift >> 1 | tdi << 7);
            }
            Tap next = Next(state, tms);
            if (state == Tap.ShiftDr && next != Tap.ShiftDr && ir == 0x16) { Flash.Deselect(); spiSelected = false; }
            if (next == Tap.ShiftDr && state != Tap.ShiftDr && ir == 0x16) { Flash.Select(); spiSelected = true; spiPrevMiso = 1; }
            if (next == Tap.CaptureDr) Capture();
            if (next == Tap.CaptureIr) irShift = 0x01;
            if (next == Tap.UpdateIr) Instr(irShift);
            if (next == Tap.Reset) ir = 0x11;
            state = next;
            return tdo;
        }

        void Capture()
        {
            drBits = 32;
            switch (ir) {
            case 0x11: dr = 0x0000081B; break;
            case 0x41: dr = Status; break;
            case 0x13: dr = 0x56444334; break;
            default: dr = 0; drBits = 1; break;
            }
        }

        void Instr(byte v)
        {
            ir = v;
            switch (v) {
            case 0x15: Status |= 1 << 7; break;                             // настройка
            case 0x3A: Status &= ~(1u << 7); break;
            case 0x05:
                if ((Status & 1 << 7) == 0) throw new ModelError("JTAG: ERASE_SRAM вне настройки");
                Status = (Status | 1 << 5) & ~(1u << 13); break;              // SRAM стёрта, Done снят
            case 0x16:
                if ((Status & 1 << 13) != 0) throw new ModelError("JTAG: мост SPI при работающей ПЛИС");
                break;
            case 0x3C:
                Reloads++;
                // загрузка слота 0: слово синхронизации A5C3 в начале — Done
                bool ok = false;
                for (int i = 0; i + 1 < 64; i++) if (Flash.Mem[i] == 0xA5 && Flash.Mem[i + 1] == 0xC3) ok = true;
                Status = ok ? (Status & ~(1u << 5)) | 1u << 13 : Status | 1;
                break;
            case 0x02: case 0x09: case 0x11: case 0x13: case 0x41: case 0x00: break;
            default: throw new ModelError("JTAG: инструкция 0x" + v.ToString("X2"));
            }
        }

        static Tap Next(Tap s, bool t)
        {
            switch (s) {
            case Tap.Reset: return t ? Tap.Reset : Tap.Idle;
            case Tap.Idle: return t ? Tap.SelectDr : Tap.Idle;
            case Tap.SelectDr: return t ? Tap.SelectIr : Tap.CaptureDr;
            case Tap.CaptureDr: return t ? Tap.Exit1Dr : Tap.ShiftDr;
            case Tap.ShiftDr: return t ? Tap.Exit1Dr : Tap.ShiftDr;
            case Tap.Exit1Dr: return t ? Tap.UpdateDr : Tap.PauseDr;
            case Tap.PauseDr: return t ? Tap.Exit2Dr : Tap.PauseDr;
            case Tap.Exit2Dr: return t ? Tap.UpdateDr : Tap.ShiftDr;
            case Tap.UpdateDr: return t ? Tap.SelectDr : Tap.Idle;
            case Tap.SelectIr: return t ? Tap.Reset : Tap.CaptureIr;
            case Tap.CaptureIr: return t ? Tap.Exit1Ir : Tap.ShiftIr;
            case Tap.ShiftIr: return t ? Tap.Exit1Ir : Tap.ShiftIr;
            case Tap.Exit1Ir: return t ? Tap.UpdateIr : Tap.PauseIr;
            case Tap.PauseIr: return t ? Tap.Exit2Ir : Tap.PauseIr;
            case Tap.Exit2Ir: return t ? Tap.UpdateIr : Tap.ShiftIr;
            default: return t ? Tap.SelectDr : Tap.Idle;                       // Update-IR
            }
        }
    }
}
