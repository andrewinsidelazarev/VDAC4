// SPDX-License-Identifier: Apache-2.0
// Прошивка ESP32-S3 через его ПЗУ-загрузчик (собственный USB: USB-Serial/JTAG), без esptool и Python.
// Протокол — «Serial Protocol» esptool (docs.espressif.com/projects/esptool, advanced-topics/serial-protocol):
// пакеты SLIP (0xC0, замены 0xDB 0xDC / 0xDB 0xDD); запрос [00][команда][длина LE16][сумма LE32][данные],
// ответ [01][команда][длина][значение LE32][данные], в конце данных — 4 байта состояния ПЗУ ESP32-S3
// (первый 0 — успех, второй — код ошибки). Сумма — только у *_DATA: 0xEF XOR всех байтов данных.
// ПЗУ стирает область по FLASH_BEGIN / FLASH_DEFL_BEGIN (размер — до блока 1 КБ, ПЗУ округляет до
// сектора); команды стирания всей flash у ПЗУ нет (она — у заглушки esptool), поэтому «стереть всё» —
// FLASH_BEGIN без данных по остатку flash. Конец записи (FLASH_DEFL_END) ПЗУ не шлём: он выводит его
// из загрузчика, а после записи ещё сверка MD5 (так же делает esptool без заглушки).
// Порядок шага: бэкап всей flash (READ_FLASH_SLOW, 64 байта на команду — больше ПЗУ не отдаёт; esptool
// --no-stub читает так же) со сверкой по MD5 самого ПЗУ → файл на диск → стирание всей flash → запись →
// сверка MD5. Чтобы 131 072 команды на 8 МБ не ждали каждая своего ответа, в полёте до 16 запросов: ПЗУ
// отвечает по порядку, лишнее USB держит в своей очереди.
// Сторожевые таймеры (10.11.2026, на плате: бэкап обрывался на 1 МБ — «IOException: Присоединенное к системе
// устройство не работает», через 8,2 с после «ПЗУ на связи», оба раза): сброс в загрузчик через USB-Serial/JTAG
// не сбрасывает сторожей RTC — RWDT и SWD — и они перезапускают чип посреди работы, USB пропадает. Сразу после
// входа в загрузчик RWDT выключается, а SWD кормится сам (WRITE_REG, как esptool: esp32s3.py disable_watchdogs).
// Перед перезапуском в прошивку снимается флаг «загрузчик принудительно» (RTC_CNTL_OPTION1, как esptool
// hard_reset), иначе чип может снова прийти в загрузчик.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace Vdac4Flasher
{
    public class EspRom
    {
        const byte FLASH_BEGIN = 0x02, SYNC = 0x08, WRITE_REG = 0x09, READ_REG = 0x0A, SPI_SET_PARAMS = 0x0B, SPI_ATTACH = 0x0D,
                   READ_FLASH_SLOW = 0x0E, FLASH_DEFL_BEGIN = 0x10, FLASH_DEFL_DATA = 0x11, SPI_FLASH_MD5 = 0x13;
        const uint CHIP_MAGIC_REG = 0x40001000, ESP32S3_MAGIC = 9;
        // RTC_CNTL ESP32-S3 (регистры и ключи — esptool targets/esp32s3.py, ESP-IDF soc/rtc_cntl_reg.h)
        public const uint RTC_CNTL = 0x60008000,
                          WDTCONFIG0 = RTC_CNTL + 0x98, WDTWPROTECT = RTC_CNTL + 0xB0, WDT_WKEY = 0x50D83AA1,
                          SWD_CONF = RTC_CNTL + 0xB4, SWD_WPROTECT = RTC_CNTL + 0xB8, SWD_WKEY = 0x8F1D312A,
                          SWD_AUTO_FEED_EN = 1u << 31,
                          OPTION1 = RTC_CNTL + 0x12C, FORCE_DOWNLOAD_BOOT = 1;
        public const int Block = 0x400;           // данные в пакете ПЗУ (FLASH_WRITE_SIZE esptool для ПЗУ)
        public const int ReadBlock = 64;          // READ_FLASH_SLOW: больше ПЗУ за раз не читает
        const int ReadWindow = 16;                // запросов чтения в полёте
        const int StatusBytes = 4;                // ПЗУ ESP32-S3: 4 байта состояния
        const int EraseMsPerMb = 30000, WriteMsPerMb = 40000, Md5MsPerMb = 8000;

        readonly IByteLink link;
        readonly IReport report;
        readonly List<byte> frame = new List<byte>();
        bool inFrame, escape;
        readonly byte[] rxBuf = new byte[4096];
        int rxPos, rxLen;

        public EspRom(IByteLink link, IReport report) { this.link = link; this.report = report; }

        /// <summary>Шаг ESP целиком: бэкап всей flash (backup — куда; null — без бэкапа, только для проверок),
        /// стирание всей flash, запись образа с адреса offset и сверка MD5.</summary>
        public void Flash(byte[] image, uint offset, uint flashSize, Backup backup)
        {
            try {
                FlashSteps(image, offset, flashSize, backup);
            } catch (IOException e) {
                // порт пропал: чип перезапустился или отключился кабель
                throw new FlashException(Lang.T("ESP32-S3 пропал с USB (перезапустился или отошёл кабель): ",
                                                "the ESP32-S3 dropped off USB (restarted or the cable came loose): ") + e.Message +
                                         Lang.T(" Повторите шаг.", " Retry the step."), e);
            }
        }

        void FlashSteps(byte[] image, uint offset, uint flashSize, Backup backup)
        {
            if (offset + image.Length > flashSize)
                throw new FlashException(Lang.T("образ не помещается во flash", "the image does not fit into flash"));
            Connect();
            uint magic = ReadReg(CHIP_MAGIC_REG);
            if (magic != ESP32S3_MAGIC)
                throw new FlashException(Lang.T("на порту не ESP32-S3 (код чипа 0x", "the port is not an ESP32-S3 (chip code 0x") +
                                         magic.ToString("X") + ")");
            report.Log(Lang.T("ПЗУ ESP32-S3 на связи", "ESP32-S3 ROM answers"));
            DisableWatchdogs();
            report.Log(Lang.T("Сторожевые таймеры RTC выключены (RWDT, SWD)", "RTC watchdogs off (RWDT, SWD)"));
            Command(SPI_ATTACH, Words(0, 0), 0, 3000);
            Command(SPI_SET_PARAMS, Words(0, flashSize, 0x10000, 0x1000, 0x100, 0xFFFF), 0, 3000);

            // 1. бэкап: вся flash — с прежней прошивкой, её настройками (NVS, Wi-Fi) и файлами (LittleFS)
            if (backup != null) {
                byte[] old = ReadFlash(0, flashSize, 0, 40, Lang.T("Бэкап: чтение flash", "Backup: reading flash"));
                report.Progress(41, Lang.T("Бэкап: сверка MD5", "Backup: MD5 check"));
                string md5 = Md5(0, flashSize);
                string want;
                using (var h = MD5.Create()) want = Util.Hex(h.ComputeHash(old));
                if (md5 != want)
                    throw new FlashException(Lang.T("бэкап прочитан с ошибкой (MD5 не сошёлся), flash не тронута — повторите шаг",
                                                    "the backup was read with errors (MD5 mismatch), flash untouched — retry the step"));
                string path = backup.Save("esp32s3_flash_" + (flashSize >> 20) + "M.bin", old, 0,
                                          Lang.T("ESP32-S3: вся flash", "ESP32-S3: whole flash"));
                report.Log(Lang.T("Бэкап: ", "Backup: ") + path + " (" + Util.Size(old.Length) + ", MD5 " + want + ")");
            }

            // 2. стирание всей flash: всё, кроме области образа, — здесь; область образа сотрёт FLASH_DEFL_BEGIN
            uint end = (uint)(offset + image.Length + 0xFFF) & ~0xFFFu;
            var spans = new List<uint[]>();
            if (offset > 0) spans.Add(new uint[] { 0, offset & ~0xFFFu });
            if (end < flashSize) spans.Add(new uint[] { end, flashSize });
            uint total = 0, done = 0;
            foreach (var s in spans) total += s[1] - s[0];
            foreach (var s in spans)
                for (uint at = s[0]; at < s[1]; at += 0x100000) {
                    uint n = Math.Min(0x100000, s[1] - at);
                    report.Progress(45 + (int)(done * 15L / Math.Max(total, 1)), Lang.T("Стирание всей flash", "Erasing the whole flash"));
                    Command(FLASH_BEGIN, Words(n, 0, (uint)Block, at, 0), 0, Scale(EraseMsPerMb, n));
                    done += n;
                    if (report.Cancelled) throw new OperationCanceledException();
                }

            // 3. запись (FLASH_DEFL_BEGIN стирает область образа — так стёрта вся flash)
            byte[] packed = Zlib(image);
            int blocks = (packed.Length + Block - 1) / Block;
            uint eraseSize = (uint)((image.Length + Block - 1) / Block * Block);
            report.Log(Lang.T("Сжато: ", "Compressed: ") + Util.Size(image.Length) + " → " + Util.Size(packed.Length));
            const int basePct = 60;
            report.Progress(basePct, Lang.T("Стирание области образа", "Erasing the image area"));
            Command(FLASH_DEFL_BEGIN, Words(eraseSize, (uint)blocks, (uint)Block, offset, 0), 0, Scale(EraseMsPerMb, eraseSize));
            report.Log(Lang.T("Стёрта вся flash: ", "Whole flash erased: ") + Util.Size(flashSize));
            for (int i = 0; i < blocks; i++) {
                if (report.Cancelled) throw new OperationCanceledException();
                int n = Math.Min(Block, packed.Length - i * Block);
                var data = new byte[16 + n];
                Util.Put32(data, 0, (uint)n);
                Util.Put32(data, 4, (uint)i);
                Array.Copy(packed, i * Block, data, 16, n);
                byte chk = 0xEF;
                for (int k = 0; k < n; k++) chk ^= packed[i * Block + k];
                Command(FLASH_DEFL_DATA, data, chk, Math.Max(10000, Scale(WriteMsPerMb, (uint)(Block * 32))));
                report.Progress(basePct + (int)((long)(i + 1) * (95 - basePct) / blocks), Lang.T("Запись", "Writing"));
            }

            report.Progress(96, Lang.T("Сверка MD5", "MD5 check"));
            string got = Md5(offset, (uint)image.Length);
            string wantImage;
            using (var h = MD5.Create()) wantImage = Util.Hex(h.ComputeHash(image));
            if (got != wantImage)
                throw new FlashException(Lang.T("сверка не сошлась: MD5 flash ", "verification failed: flash MD5 ") + got +
                                         Lang.T(", образа ", ", image ") + wantImage);
            report.Log(Lang.T("Записано и сверено (MD5 ", "Written and verified (MD5 ") + wantImage + ")");
            report.Progress(100, Lang.T("Готово", "Done"));
        }

        /// <summary>MD5 области flash, посчитанный самим ПЗУ (32 знака hex).</summary>
        string Md5(uint offset, uint length)
        {
            byte[] md5 = Command(SPI_FLASH_MD5, Words(offset, length, 0, 0), 0, Scale(Md5MsPerMb, length));
            return md5.Length >= 32 ? Encoding.ASCII.GetString(md5, 0, 32).ToLowerInvariant() : Util.Hex(md5);
        }

        /// <summary>Прочитать flash ПЗУ-командой READ_FLASH_SLOW: по 64 байта, до ReadWindow запросов в полёте.
        /// Ход — от pctFrom до pctTo.</summary>
        public byte[] ReadFlash(uint offset, uint length, int pctFrom, int pctTo, string stage)
        {
            var data = new byte[length];
            int blocks = (int)((length + ReadBlock - 1) / ReadBlock), sent = 0, got = 0;
            var batch = new List<byte>(ReadWindow * 24);
            while (got < blocks) {
                if (report.Cancelled) throw new OperationCanceledException();
                if (sent - got <= ReadWindow / 2 && sent < blocks) {
                    batch.Clear();
                    while (sent < blocks && sent - got < ReadWindow) {
                        uint at = offset + (uint)sent * ReadBlock;
                        uint n = Math.Min((uint)ReadBlock, offset + length - at);
                        batch.AddRange(Slip(Packet(READ_FLASH_SLOW, Words(at, n), 0)));
                        sent++;
                    }
                    link.Write(batch.ToArray(), 0, batch.Count);
                }
                uint value;
                byte[] r = Response(READ_FLASH_SLOW, 3000, out value);
                int want = (int)Math.Min((uint)ReadBlock, length - (uint)got * ReadBlock);
                if (r.Length < want)
                    throw new FlashException(string.Format(Lang.T("ПЗУ ESP вернуло {0} байтов вместо {1} (чтение 0x{2:X6})",
                                                                  "ESP ROM returned {0} bytes instead of {1} (read at 0x{2:X6})"),
                                                           r.Length, want, offset + (uint)got * ReadBlock));
                Array.Copy(r, 0, data, got * ReadBlock, want);
                got++;
                if ((got & 1023) == 0 || got == blocks)
                    report.Progress(pctFrom + (int)((long)got * (pctTo - pctFrom) / blocks),
                                    stage + " " + Util.Size((long)got * ReadBlock) + " / " + Util.Size(length));
            }
            return data;
        }

        /// <summary>Перезапуск модуля в записанную прошивку (USB-Serial/JTAG: RTS — сброс, DTR — 0). Сначала —
        /// снять «загрузчик принудительно» (ПЗУ может уже не ответить — тогда просто сброс).</summary>
        public void HardReset()
        {
            try {
                WriteReg(OPTION1, 0, FORCE_DOWNLOAD_BOOT);
            } catch (Exception) {
            }
            link.SetDtr(false);
            link.SetRts(true);
            Thread.Sleep(200);
            link.SetRts(false);
            Thread.Sleep(200);
        }

        // ---- связь с ПЗУ ------------------------------------------------------------------------

        /// <summary>Сброс в загрузчик (последовательность esptool для USB-Serial/JTAG) и SYNC.</summary>
        void Connect()
        {
            for (int attempt = 0; attempt < 6; attempt++) {
                if (report.Cancelled) throw new OperationCanceledException();
                // DTR — IO0, RTS — сброс; переход через (1,1), а не (0,0): Windows передаёт DTR вместе с RTS
                link.SetRts(false); link.SetDtr(false);
                Thread.Sleep(100);
                link.SetDtr(true); link.SetRts(false);
                Thread.Sleep(100);
                link.SetRts(true); link.SetDtr(false); link.SetRts(true);
                Thread.Sleep(100);
                link.SetDtr(false); link.SetRts(false);
                Thread.Sleep(attempt == 0 ? 250 : 500);
                link.Discard();
                ResetRx();
                for (int k = 0; k < 5; k++) {
                    try {
                        var data = new byte[36];
                        data[0] = 0x07; data[1] = 0x07; data[2] = 0x12; data[3] = 0x20;
                        for (int i = 4; i < 36; i++) data[i] = 0x55;
                        Command(SYNC, data, 0, 150);
                        // ПЗУ отвечает на SYNC несколько раз: остальное выбросить
                        Thread.Sleep(100);
                        link.Discard();
                        ResetRx();
                        return;
                    } catch (TimeoutException) {
                    } catch (FlashException) {
                    }
                }
            }
            throw new FlashException(Lang.T(
                "ESP32-S3 не входит в загрузчик. Отключите USB, нажмите и держите кнопку BOOT на модуле, подключите USB, отпустите кнопку и повторите.",
                "The ESP32-S3 does not enter its bootloader. Unplug USB, hold the BOOT button on the module, plug USB in, release the button and try again."));
        }

        uint ReadReg(uint addr)
        {
            uint value;
            Command(READ_REG, Words(addr), 0, 3000, out value);
            return value;
        }

        /// <summary>WRITE_REG ПЗУ: [адрес][значение][маска][задержка мкс].</summary>
        void WriteReg(uint addr, uint value, uint mask = 0xFFFFFFFF)
        {
            Command(WRITE_REG, Words(addr, value, mask, 0), 0, 3000);
        }

        /// <summary>Сторожа RTC: RWDT выключить, SWD — кормить самому (порядок и ключи — как у esptool).</summary>
        void DisableWatchdogs()
        {
            WriteReg(WDTWPROTECT, WDT_WKEY);
            WriteReg(WDTCONFIG0, 0);
            WriteReg(WDTWPROTECT, 0);
            WriteReg(SWD_WPROTECT, SWD_WKEY);
            WriteReg(SWD_CONF, ReadReg(SWD_CONF) | SWD_AUTO_FEED_EN);
            WriteReg(SWD_WPROTECT, 0);
        }

        byte[] Command(byte op, byte[] data, uint checksum, int timeoutMs)
        {
            uint value;
            return Command(op, data, checksum, timeoutMs, out value);
        }

        /// <summary>Команда ПЗУ: данные ответа без 4 байтов состояния; ошибка ПЗУ — FlashException.</summary>
        byte[] Command(byte op, byte[] data, uint checksum, int timeoutMs, out uint value)
        {
            byte[] slip = Slip(Packet(op, data, checksum));
            link.Write(slip, 0, slip.Length);
            return Response(op, timeoutMs, out value);
        }

        static byte[] Packet(byte op, byte[] data, uint checksum)
        {
            var pkt = new byte[8 + data.Length];
            pkt[0] = 0x00;
            pkt[1] = op;
            pkt[2] = (byte)data.Length;
            pkt[3] = (byte)(data.Length >> 8);
            Util.Put32(pkt, 4, checksum);
            Array.Copy(data, 0, pkt, 8, data.Length);
            return pkt;
        }

        /// <summary>Ответ ПЗУ на команду op: данные без 4 байтов состояния; ошибка ПЗУ — FlashException.</summary>
        byte[] Response(byte op, int timeoutMs, out uint value)
        {
            int deadline = Environment.TickCount + timeoutMs;
            while (true) {
                int left = deadline - Environment.TickCount;
                if (left <= 0) throw new TimeoutException();
                byte[] resp = NextFrame(left);
                if (resp == null) throw new TimeoutException();
                if (resp.Length < 8 || resp[0] != 0x01 || resp[1] != op) continue;   // чужой ответ (эхо прежних SYNC)
                value = Util.Get32(resp, 4);
                int len = Util.Get16(resp, 2);
                if (resp.Length < 8 + len || len < 2)
                    throw new FlashException(Lang.T("короткий ответ ПЗУ ESP", "short ESP ROM reply"));
                int st = len >= StatusBytes ? len - StatusBytes : len - 2;
                if (resp[8 + st] != 0)
                    throw new FlashException(string.Format(Lang.T("ПЗУ ESP отказало: команда 0x{0:X2}, код 0x{1:X2}",
                                                                  "ESP ROM refused: command 0x{0:X2}, code 0x{1:X2}"), op, resp[9 + st]));
                var outData = new byte[st];
                Array.Copy(resp, 8, outData, 0, st);
                return outData;
            }
        }

        static byte[] Slip(byte[] pkt)
        {
            var o = new List<byte>(pkt.Length + 8) { 0xC0 };
            foreach (byte b in pkt) {
                if (b == 0xC0) { o.Add(0xDB); o.Add(0xDC); }
                else if (b == 0xDB) { o.Add(0xDB); o.Add(0xDD); }
                else o.Add(b);
            }
            o.Add(0xC0);
            return o.ToArray();
        }

        void ResetRx() { frame.Clear(); inFrame = false; escape = false; rxPos = rxLen = 0; }

        /// <summary>Следующий пакет SLIP (без рамки), null — за timeoutMs не пришёл.</summary>
        byte[] NextFrame(int timeoutMs)
        {
            int deadline = Environment.TickCount + timeoutMs;
            while (true) {
                while (rxPos < rxLen) {
                    byte b = rxBuf[rxPos++];
                    if (!inFrame) {
                        if (b == 0xC0) { inFrame = true; frame.Clear(); }
                        continue;                                      // текст журнала ПЗУ между пакетами
                    }
                    if (b == 0xC0) {
                        if (frame.Count == 0) continue;                // C0 C0: начало следующего
                        inFrame = false;
                        return frame.ToArray();
                    }
                    if (escape) {
                        frame.Add(b == 0xDC ? (byte)0xC0 : b == 0xDD ? (byte)0xDB : b);
                        escape = false;
                    } else if (b == 0xDB) {
                        escape = true;
                    } else {
                        frame.Add(b);
                    }
                }
                int left = deadline - Environment.TickCount;
                if (left <= 0) return null;
                rxLen = link.Read(rxBuf, 0, rxBuf.Length, Math.Min(left, 50));
                rxPos = 0;
            }
        }

        static byte[] Words(params uint[] w)
        {
            var b = new byte[w.Length * 4];
            for (int i = 0; i < w.Length; i++) Util.Put32(b, i * 4, w[i]);
            return b;
        }

        static int Scale(int msPerMb, uint bytes) { return Math.Max(3000, (int)(msPerMb * (bytes / 1048576.0))); }

        /// <summary>Поток zlib (заголовок 78 9C, deflate, Adler-32) — его ждёт ПЗУ в FLASH_DEFL_DATA.</summary>
        public static byte[] Zlib(byte[] data)
        {
            using (var ms = new MemoryStream()) {
                ms.WriteByte(0x78);
                ms.WriteByte(0x9C);
                using (var d = new DeflateStream(ms, CompressionLevel.Optimal, true)) d.Write(data, 0, data.Length);
                uint a = 1, b = 0;
                foreach (byte x in data) { a = (a + x) % 65521; b = (b + a) % 65521; }
                uint adler = b << 16 | a;
                ms.WriteByte((byte)(adler >> 24)); ms.WriteByte((byte)(adler >> 16));
                ms.WriteByte((byte)(adler >> 8)); ms.WriteByte((byte)adler);
                return ms.ToArray();
            }
        }
    }
}
