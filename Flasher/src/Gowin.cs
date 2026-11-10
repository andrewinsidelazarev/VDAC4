// SPDX-License-Identifier: Apache-2.0
// Flash ПЛИС Tang Nano 20K (GW2AR-LV18, IDCODE 0x0000081B, внешняя SPI flash 8 МБ) через JTAG, без
// Gowin Programmer. Как openFPGALoader (gowin.cpp, Apache-2.0) для GW2A: SRAM стирается (ПЛИС
// перестаёт грузить свою конфигурацию с flash), затем каждый обмен с SPI flash — инструкция 0x16 в IR и
// сдвиг DR из Exit2-DR: TCK — SCK, TDI — MOSI, TDO — MISO (на бит позже), CS — пока ТАП в Shift-DR;
// SPI идёт старшим битом вперёд, JTAG — младшим, поэтому байты переворачиваются. В конце — RELOAD:
// ПЛИС грузится с flash (слот 0).
// Порядок: бэкап каждого затронутого слота flash (1 МБ целиком: 0 — TS-Config, 1 — FT812, 2 — шрифты, 3 —
// видео) → файлы на диск → стирание этих слотов целиком (блоки 64 КБ) → запись образов → сверка чтением →
// RELOAD. Прочие слоты и настройки Tang в последнем секторе flash не трогаются.
using System;
using System.Collections.Generic;
using System.Threading;

namespace Vdac4Flasher
{
    public class Gowin
    {
        const byte NOOP = 0x02, ERASE_SRAM = 0x05, XFER_DONE = 0x09, READ_IDCODE = 0x11, READ_USERCODE = 0x13,
                   CONFIG_ENABLE = 0x15, SPI_BRIDGE = 0x16, CONFIG_DISABLE = 0x3A, RELOAD = 0x3C, STATUS = 0x41;
        const uint ST_CRC_ERROR = 1 << 0, ST_MEMORY_ERASE = 1 << 5, ST_EDIT_MODE = 1 << 7, ST_DONE = 1 << 13;
        public const uint Gw2ar18 = 0x0000081B;
        /// <summary>С 0x7F0000 — настройки кадрового буфера Tang (core_mailbox.h CC_SLOT_END): не трогать.</summary>
        public const uint TangSettingsBase = 0x7F0000;

        readonly Jtag j;
        readonly IReport report;

        public Gowin(Jtag jtag, IReport report) { j = jtag; this.report = report; }

        void Cmd(byte c)
        {
            j.ShiftIr(c);
            j.ToggleClk(6);
        }

        uint Reg(byte c)
        {
            Cmd(c);
            var o = new byte[4];
            j.ShiftDr(new byte[] { 0xFF, 0xFF, 0xFF, 0xFF }, 32, o);
            j.Flush();
            return Util.Get32(o, 0);
        }

        public uint IdCode() { return Reg(READ_IDCODE); }
        public uint Status() { return Reg(STATUS); }
        public uint UserCode() { return Reg(READ_USERCODE); }

        bool Poll(uint mask, uint value, int timeoutMs)
        {
            int deadline = Environment.TickCount + timeoutMs;
            do {
                if ((Status() & mask) == value) return true;
            } while (Environment.TickCount - deadline < 0);
            return false;
        }

        /// <summary>Подключиться: ТАП — в Reset, IDCODE — GW2AR-18.</summary>
        public void Open()
        {
            j.Init(6000000);
            uint id = IdCode();
            if ((id & 0x0FFFFFFF) != Gw2ar18)
                throw new FlashException(Lang.T("на JTAG не GW2AR-18 Tang Nano 20K (IDCODE 0x", "the JTAG target is not the Tang Nano 20K GW2AR-18 (IDCODE 0x") +
                                         id.ToString("X8") + ")");
            report.Log("JTAG: GW2AR-18, IDCODE 0x" + id.ToString("X8") + Lang.T(", состояние 0x", ", status 0x") + Status().ToString("X8"));
        }

        /// <summary>Стереть SRAM (TN653; при ошибке CRC — недокументированная последовательность openFPGALoader).</summary>
        void EraseSram()
        {
            uint st = Status();
            if ((st & ST_CRC_ERROR) != 0) {
                Cmd(CONFIG_DISABLE); Cmd(0); IdCode(); Status();
                Cmd(CONFIG_DISABLE); Cmd(0); Status(); IdCode();
                Cmd(CONFIG_ENABLE); Cmd(RELOAD); Cmd(NOOP);
                Cmd(CONFIG_DISABLE); Cmd(NOOP); IdCode(); Cmd(NOOP); IdCode();
            }
            Cmd(CONFIG_ENABLE);
            if (!Poll(ST_EDIT_MODE, ST_EDIT_MODE, 3000))
                throw new FlashException(Lang.T("ПЛИС не входит в режим настройки", "the FPGA does not enter configuration mode"));
            Cmd(ERASE_SRAM);
            Cmd(NOOP);
            if (!Poll(ST_MEMORY_ERASE, ST_MEMORY_ERASE, 3000))
                throw new FlashException(Lang.T("SRAM ПЛИС не стёрлась", "FPGA SRAM did not erase"));
            Cmd(XFER_DONE);
            Cmd(NOOP);
            Cmd(CONFIG_DISABLE);
            Cmd(NOOP);
            if (!Poll(ST_EDIT_MODE, 0, 3000))
                throw new FlashException(Lang.T("ПЛИС не выходит из режима настройки", "the FPGA does not leave configuration mode"));
            if ((Status() & ST_DONE) != 0)
                throw new FlashException(Lang.T("после стирания SRAM ПЛИС всё ещё сконфигурирована", "the FPGA is still configured after the SRAM erase"));
        }

        /// <summary>Обмен с SPI flash: tx уходит целиком; read — вернуть принятое (то же число байтов, что tx).</summary>
        byte[] Spi(byte[] tx, bool read)
        {
            Cmd(SPI_BRIDGE);
            j.SetState(Tap.Exit2Dr);
            int n = tx.Length + (read ? 1 : 0);          // MISO — на бит позже: лишний байт для хвоста
            var jtx = new byte[n];
            for (int i = 0; i < tx.Length; i++) jtx[i] = Util.Reverse(tx[i]);
            if (!read) {
                j.ShiftDr(jtx, 8 * n, null);
                return null;
            }
            var jrx = new byte[n];
            j.ShiftDr(jtx, 8 * n, jrx);
            j.Flush();
            var rx = new byte[tx.Length];
            for (int i = 0; i < tx.Length; i++)
                rx[i] = (byte)(Util.Reverse((byte)(jrx[i] >> 1)) | (jrx[i + 1] & 0x01));
            return rx;
        }

        byte ReadStatus() { return Spi(new byte[] { 0x05, 0x00 }, true)[1]; }

        void WriteEnable()
        {
            Spi(new byte[] { 0x06 }, false);
            for (int i = 0; i < 100; i++)
                if ((ReadStatus() & 0x02) != 0) return;
            throw new FlashException(Lang.T("flash ПЛИС не принимает запись (WEL)", "FPGA flash does not accept writes (WEL)"));
        }

        void WaitReady(int timeoutMs)
        {
            int deadline = Environment.TickCount + timeoutMs;
            while ((ReadStatus() & 0x01) != 0)
                if (Environment.TickCount - deadline >= 0)
                    throw new FlashException(Lang.T("flash ПЛИС занята дольше срока", "FPGA flash stays busy too long"));
        }

        static byte[] Addr(byte op, uint a, int extra)
        {
            var b = new byte[4 + extra];
            b[0] = op; b[1] = (byte)(a >> 16); b[2] = (byte)(a >> 8); b[3] = (byte)a;
            return b;
        }

        public uint FlashId { get; private set; }

        /// <summary>Бэкап и стирание затронутых слотов, запись кусков, сверка чтением; затем ПЛИС перезагружается
        /// с flash. backup null — без бэкапа (только для проверок).</summary>
        public void Program(List<FlashPart> parts, Backup backup)
        {
            // слоты по 1 МБ, которых касаются куски
            var slots = new SortedSet<uint>();
            foreach (var p in parts) {
                if (p.Data.Length == 0) continue;
                for (uint s = p.Address / FirmwareSet.TangSlotBytes; s <= (p.Address + (uint)p.Data.Length - 1) / FirmwareSet.TangSlotBytes; s++)
                    slots.Add(s);
            }
            foreach (uint s in slots)
                if ((s + 1) * FirmwareSet.TangSlotBytes > TangSettingsBase)
                    throw new FlashException(Lang.T("кусок заходит в настройки Tang в конце flash", "a part reaches the Tang settings at the end of flash"));

            Open();
            report.Progress(1, Lang.T("Стирание SRAM ПЛИС", "Erasing FPGA SRAM"));
            EraseSram();
            j.SetClock(10000000);
            // разбудить flash и сбросить её (как SPIFlash::reset openFPGALoader)
            Spi(new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, false);
            Spi(new byte[] { 0x66 }, false);
            Spi(new byte[] { 0x99 }, false);
            j.Flush();
            Thread.Sleep(1);
            byte[] id = Spi(new byte[] { 0x9F, 0, 0, 0 }, true);
            FlashId = (uint)(id[1] << 16 | id[2] << 8 | id[3]);
            report.Log(Lang.T("flash ПЛИС: JEDEC ", "FPGA flash: JEDEC ") + FlashId.ToString("X6"));
            if (FlashId == 0 || FlashId == 0xFFFFFF)
                throw new FlashException(Lang.T("flash ПЛИС не отвечает через JTAG", "FPGA flash does not answer over JTAG"));
            byte sr = ReadStatus();
            if ((sr & 0x7C) != 0) {                      // биты защиты BP — снять
                report.Log(Lang.T("снимаю защиту flash, регистр 0x", "removing flash protection, status 0x") + sr.ToString("X2"));
                WriteEnable();
                Spi(new byte[] { 0x01, 0x00 }, false);
                WaitReady(2000);
            }

            // 1. бэкап слотов целиком
            if (backup != null) {
                int k = 0;
                foreach (uint s in slots) {
                    uint at = s * FirmwareSet.TangSlotBytes;
                    var old = new byte[FirmwareSet.TangSlotBytes];
                    for (uint off = 0; off < old.Length; off += 4096) {
                        if (report.Cancelled) throw new OperationCanceledException();
                        byte[] rx = Spi(Addr(0x03, at + off, 4096), true);
                        Array.Copy(rx, 4, old, off, 4096);
                        if ((off & 0xFFFF) == 0)
                            report.Progress(2 + (int)((k * (long)old.Length + off) * 20 / (slots.Count * (long)old.Length)),
                                            Lang.T("Бэкап: чтение слота ", "Backup: reading slot ") + s);
                    }
                    string path = backup.Save("tang_slot" + s + ".bin", old, at, Lang.T("ПЛИС Tang: слот ", "Tang FPGA: slot ") + s);
                    report.Log(Lang.T("Бэкап: ", "Backup: ") + path);
                    k++;
                }
            }

            // 2. стирание слотов целиком
            {
                int k = 0;
                foreach (uint s in slots) {
                    report.Progress(23 + k * 5 / slots.Count, Lang.T("Стирание слота ", "Erasing slot ") + s);
                    Erase(s * FirmwareSet.TangSlotBytes, FirmwareSet.TangSlotBytes);
                    report.Log(string.Format(Lang.T("Слот {0} стёрт (0x{1:X6}, 1 МБ)", "Slot {0} erased (0x{1:X6}, 1 MB)"), s, s * FirmwareSet.TangSlotBytes));
                    k++;
                }
            }

            // 3. запись
            long total = 0, done = 0;
            foreach (var p in parts) total += p.Data.Length;
            foreach (var p in parts) {
                report.Log(string.Format(Lang.T("{0}: {1} с 0x{2:X6}", "{0}: {1} at 0x{2:X6}"), p.Name, Util.Size(p.Data.Length), p.Address));
                for (int off = 0; off < p.Data.Length; off += 256) {
                    if (report.Cancelled) throw new OperationCanceledException();
                    int n = Math.Min(256, p.Data.Length - off);
                    bool blank = true;
                    for (int k = 0; k < n && blank; k++) blank = p.Data[off + k] == 0xFF;
                    if (!blank) {
                        WriteEnable();
                        var pp = Addr(0x02, (uint)(p.Address + off), n);
                        Array.Copy(p.Data, off, pp, 4, n);
                        Spi(pp, false);
                        WaitReady(100);
                    }
                    done += n;
                    if ((off & 0x3FFF) == 0)
                        report.Progress(28 + (int)(done * 44 / total), Lang.T("Запись flash ПЛИС", "Writing FPGA flash"));
                }
            }
            done = 0;
            foreach (var p in parts) {
                for (int off = 0; off < p.Data.Length; off += 4096) {
                    if (report.Cancelled) throw new OperationCanceledException();
                    int n = Math.Min(4096, p.Data.Length - off);
                    byte[] rx = Spi(Addr(0x03, (uint)(p.Address + off), n), true);
                    for (int k = 0; k < n; k++)
                        if (rx[4 + k] != p.Data[off + k])
                            throw new FlashException(string.Format(Lang.T("сверка не сошлась: {0}, адрес 0x{1:X6} (прочитано {2:X2}, нужно {3:X2})",
                                                                          "verification failed: {0}, address 0x{1:X6} (read {2:X2}, expected {3:X2})"),
                                                                   p.Name, p.Address + off + k, rx[4 + k], p.Data[off + k]));
                    done += n;
                    report.Progress(72 + (int)(done * 25 / total), Lang.T("Сверка чтением", "Read-back check"));
                }
            }
            report.Log(Lang.T("flash ПЛИС записана и сверена", "FPGA flash written and verified"));

            j.SetClock(6000000);
            Cmd(RELOAD);
            Cmd(NOOP);
            j.Flush();
            Thread.Sleep(3500);                             // загрузка слота 0 ~3 с
            uint st = Status();
            report.Log(Lang.T("ПЛИС перезагружена, состояние 0x", "FPGA reloaded, status 0x") + st.ToString("X8"));
            if ((st & ST_DONE) == 0 || (st & ST_CRC_ERROR) != 0)
                throw new FlashException(Lang.T("ПЛИС не загрузилась с новой flash (нет Done) — выключите и включите питание Tang и проверьте картинку",
                                                "the FPGA did not start from the new flash (no Done) — power-cycle the Tang and check the picture"));
            report.Progress(100, Lang.T("Готово", "Done"));
        }

        /// <summary>Стереть [addr, addr+len) по 4 КБ: блоки 64 КБ — где они целиком внутри.</summary>
        void Erase(uint addr, uint len)
        {
            uint a = addr & ~0xFFFu, end = (addr + len + 0xFFF) & ~0xFFFu;
            while (a < end) {
                if (report.Cancelled) throw new OperationCanceledException();
                WriteEnable();
                if ((a & 0xFFFF) == 0 && a + 0x10000 <= end) {
                    Spi(Addr(0xD8, a, 0), false);
                    WaitReady(3000);
                    a += 0x10000;
                } else {
                    Spi(Addr(0x20, a, 0), false);
                    WaitReady(1000);
                    a += 0x1000;
                }
            }
        }
    }
}
