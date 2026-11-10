// SPDX-License-Identifier: Apache-2.0
// Прошивка BL616 модуля Tang Nano 20K через его ПЗУ ISP по USB CDC (кнопка UPDATE при подключении —
// порт «Bouffalo CDC DEMO»), без BLFlashCommand и Python. Последовательность — как у bflb-mcu-tool 1.10
// (Bouffalo Lab, MIT) в режиме «bootrom load» (load_function = 2: пишет само ПЗУ, загрузчик в ОЗУ не нужен);
// она же — в журнале удачной записи BLFlashCommand с этой платы 18.10.2026:
//   синхронизация: 300 байтов 0x55 (0,006 с на 500 000 бод) → «OK»;
//   0x10 сведения о загрузке → вывод flash (sw_usage >> 14 & 0x3F), признак A0 (байт 0 = 1);
//   срок ISP 10 с (A0 — запись 0x27101200 по 0x6102DF04, иначе 0x23); 0x22 такт;
//   0x3B параметры flash: 4 байта, 0x36 JEDEC, 0x3B ещё раз — 4 + 84 байта flash_para;
//   0x30 стирание [начало, конец включительно]; срок ISP 2 с; 0x31 запись по 2048 байтов;
//   0x3A проверка записи; 0x60, 0x3E SHA-256 через XIP, 0x61.
// Кадр: [команда][сумма][длина LE16][данные]; сумма — младший байт суммы байтов длины и данных
// (у команд ПЗУ 0x10, 0x23, 0x50 — 0, как у bflb). Ответ: «OK» / «PD» (ещё занят) / «FL» + код LE16;
// с данными — «OK», длина LE16, данные.
// Перед стиранием — бэкап всего раздела прошивки VDAC4 (0x40000…0x7FFFF): чтение 0x32 [адрес][длина] по
// 2048 байтов (flash_read bflb-mcu-tool, tx_size 2056 − 8), сверка с SHA-256 по XIP (0x60/0x3E/0x61), файл
// на диск. Стирается раздел целиком (не только длина образа): хвост прежней, более длинной прошивки не
// остаётся. FPGA Partner (0x0…0x3FFFF) и кэш ядер (с 0x80000) не трогаются.
using System;
using System.Security.Cryptography;
using System.Threading;

namespace Vdac4Flasher
{
    public class Bl616Isp
    {
        readonly IByteLink link;
        readonly IReport report;
        readonly byte[] rx = new byte[8192];
        int rxPos, rxLen;
        bool a0;
        byte flashPin = 0x02;

        /// <summary>Параметры flash (байты 12…95 заголовка загрузки) — chips/bl616/efuse_bootheader/flash_para.bin
        /// bflb (он же — для GD25LQ32D, JEDEC C86016, что стоит в BL616 Tang Nano 20K).</summary>
        static readonly byte[] FlashPara = {
            0x04, 0x01, 0x00, 0x00, 0x66, 0x99, 0xFF, 0x03, 0x9F, 0x00, 0xB7, 0xE9, 0x04, 0xC8, 0x00, 0x01,
            0xC7, 0x20, 0x52, 0xD8, 0x06, 0x02, 0x32, 0x00, 0x0B, 0x01, 0x0B, 0x01, 0x3B, 0x01, 0xBB, 0x00,
            0x6B, 0x01, 0xEB, 0x02, 0xEB, 0x02, 0x02, 0x50, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x02, 0x01,
            0x02, 0x01, 0xAB, 0x01, 0x05, 0x35, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x38, 0xFF, 0xA0, 0xFF,
            0x77, 0x03, 0x02, 0x40, 0x77, 0x03, 0x02, 0xF0, 0x2C, 0x01, 0xB0, 0x04, 0xB0, 0x04, 0x05, 0x00,
            0xE8, 0x80, 0x14, 0x00
        };
        const uint FlashClockCfg = 0x41, FlashIoMode = 1, FlashClockDelay = 1;   // flash_set = 0x01014102 при выводе 2
        const int Chunk = 2048;                                                  // tx_size 2056 − 8

        public Bl616Isp(IByteLink link, IReport report) { this.link = link; this.report = report; }

        /// <summary>Шаг BL616: бэкап раздела [address, partEnd) (backup null — без бэкапа, только для проверок),
        /// стирание раздела, запись образа, сверка SHA-256.</summary>
        public void Flash(byte[] image, uint address, uint partEnd, Backup backup)
        {
            if (address + image.Length > partEnd)
                throw new FlashException(Lang.T("образ BL616 не помещается в раздел прошивки", "the BL616 image does not fit into the firmware partition"));
            Handshake();
            byte[] info = Request(0x10, new byte[0], false, 2000);
            if (info.Length < 12) throw new FlashException(Lang.T("короткие сведения о загрузке BL616", "short BL616 boot info"));
            a0 = info[0] == 0x01;
            uint sw = Util.Get32(info, 8);
            flashPin = (byte)(sw >> 14 & 0x3F);
            string chip = "";
            for (int i = 17; i >= 12 && i < info.Length; i--) chip += info[i].ToString("x2");
            report.Log(Lang.T("ПЗУ BL616 на связи, чип ", "BL616 ROM answers, chip ") + chip + (a0 ? " (A0)" : "") +
                       Lang.T(", вывод flash 0x", ", flash pin 0x") + flashPin.ToString("X2"));

            SetIspTimeout(10000);
            var clk = new byte[8];
            Util.Put32(clk, 0, 1);                 // прерывания разрешены
            Util.Put32(clk, 4, 2000000);           // скорость загрузки
            Ack(Send(0x22, clk, true), 2000);
            Thread.Sleep(10);
            Drain(100);
            Thread.Sleep(100);

            uint flashSet = flashPin | FlashClockCfg << 8 | FlashIoMode << 16 | FlashClockDelay << 24;
            var set4 = new byte[4];
            Util.Put32(set4, 0, flashSet);
            Ack(Send(0x3B, set4, true), 2000);
            byte[] jid = Request(0x36, new byte[0], true, 2000);
            if (jid.Length < 3) throw new FlashException(Lang.T("flash BL616 не ответила (JEDEC)", "BL616 flash does not answer (JEDEC)"));
            uint flashBytes = (jid[2] & 0x1F) >= 0x13 ? (1u << ((jid[2] & 0x1F) - 0x13)) * 512 * 1024 : 0;
            report.Log("JEDEC " + Util.Hex(jid, 0, 3) + (flashBytes > 0 ? ", " + Util.Size(flashBytes) : ""));
            if (flashBytes > 0 && partEnd > flashBytes)
                throw new FlashException(Lang.T("раздел прошивки за краем flash BL616", "the firmware partition is beyond BL616 flash"));
            var set88 = new byte[4 + FlashPara.Length];
            Util.Put32(set88, 0, flashSet);
            Array.Copy(FlashPara, 0, set88, 4, FlashPara.Length);
            Ack(Send(0x3B, set88, true), 2000);

            uint part = partEnd - address;
            if (backup != null) {
                byte[] old = new byte[part];
                for (uint off = 0; off < part; off += Chunk) {
                    if (report.Cancelled) throw new OperationCanceledException();
                    uint n = Math.Min((uint)Chunk, part - off);
                    var rq = new byte[8];
                    Util.Put32(rq, 0, address + off);
                    Util.Put32(rq, 4, n);
                    byte[] d = Request(0x32, rq, true, 2000);
                    if (d.Length != n)
                        throw new FlashException(string.Format(Lang.T("ПЗУ BL616 вернуло {0} байтов вместо {1} (чтение 0x{2:X6})",
                                                                      "BL616 ROM returned {0} bytes instead of {1} (read at 0x{2:X6})"),
                                                               d.Length, n, address + off));
                    Array.Copy(d, 0, old, off, n);
                    report.Progress(2 + (int)((long)(off + n) * 23 / part), Lang.T("Бэкап: чтение flash", "Backup: reading flash"));
                }
                byte[] sha = XipSha(address, part);
                byte[] want;
                using (var h = SHA256.Create()) want = h.ComputeHash(old);
                if (sha.Length < 32 || Util.Hex(sha, 0, 32) != Util.Hex(want))
                    throw new FlashException(Lang.T("бэкап прочитан с ошибкой (SHA-256 не сошёлся), flash не тронута — повторите шаг",
                                                    "the backup was read with errors (SHA-256 mismatch), flash untouched — retry the step"));
                string path = backup.Save(string.Format("bl616_0x{0:X5}_{1}K.bin", address, part >> 10), old, address,
                                          Lang.T("BL616: раздел прошивки VDAC4", "BL616: VDAC4 firmware partition"));
                report.Log(Lang.T("Бэкап: ", "Backup: ") + path + " (" + Util.Size(old.Length) + ")");
            }

            report.Progress(26, Lang.T("Стирание раздела прошивки", "Erasing the firmware partition"));
            var er = new byte[8];
            Util.Put32(er, 0, address);
            Util.Put32(er, 4, partEnd - 1);
            Ack(Send(0x30, er, true), 100000);
            report.Log(string.Format(Lang.T("Стёрт раздел 0x{0:X5}…0x{1:X5}", "Erased the partition 0x{0:X5}…0x{1:X5}"), address, partEnd - 1));

            SetIspTimeout(2000);
            int chunks = (image.Length + Chunk - 1) / Chunk;
            for (int i = 0; i < chunks; i++) {
                if (report.Cancelled) throw new OperationCanceledException();
                int n = Math.Min(Chunk, image.Length - i * Chunk);
                var d = new byte[4 + n];
                Util.Put32(d, 0, (uint)(address + i * Chunk));
                Array.Copy(image, i * Chunk, d, 4, n);
                for (int tries = 0; ; tries++) {
                    try {
                        Ack(Send(0x31, d, true), 2000);
                        break;
                    } catch (FlashException) {
                        if (tries >= 3) throw;
                        Drain(50);
                    }
                }
                report.Progress(30 + (i + 1) * 60 / chunks, Lang.T("Запись", "Writing"));
            }
            Ack(Send(0x3A, new byte[0], true), 2000);

            report.Progress(92, Lang.T("Сверка SHA-256", "SHA-256 check"));
            byte[] got = XipSha(address, (uint)image.Length);
            byte[] wantImage;
            using (var h = SHA256.Create()) wantImage = h.ComputeHash(image);
            if (got.Length < 32 || Util.Hex(got, 0, 32) != Util.Hex(wantImage))
                throw new FlashException(Lang.T("сверка не сошлась: SHA-256 flash ", "verification failed: flash SHA-256 ") +
                                         Util.Hex(got) + Lang.T(", образа ", ", image ") + Util.Hex(wantImage));
            report.Log(Lang.T("Записано и сверено (SHA-256 ", "Written and verified (SHA-256 ") + Util.Hex(wantImage) + ")");
            report.Progress(100, Lang.T("Готово", "Done"));
        }

        /// <summary>SHA-256 области flash, посчитанный ПЗУ через XIP (0x60 вход, 0x3E, 0x61 выход).</summary>
        byte[] XipSha(uint address, uint length)
        {
            Ack(Send(0x60, new byte[0], true), 2000);
            var sh = new byte[8];
            Util.Put32(sh, 0, address);
            Util.Put32(sh, 4, length);
            try {
                return Request(0x3E, sh, true, 4000);
            } finally {
                Ack(Send(0x61, new byte[0], true), 2000);
            }
        }

        /// <summary>Уже в ISP — ответит на 0x10 сразу; иначе 0x55 до «OK» (до трёх попыток).</summary>
        void Handshake()
        {
            link.SetDtr(false);
            link.SetRts(false);
            Drain(50);
            Send(0x10, new byte[0], false);
            if (TryAck(150) == "OK") {
                byte[] skip;
                TryData(200, out skip);
                Drain(50);
                return;
            }
            var sync = new byte[300];
            for (int i = 0; i < sync.Length; i++) sync[i] = 0x55;
            for (int attempt = 0; attempt < 3; attempt++) {
                if (report.Cancelled) throw new OperationCanceledException();
                Drain(100);
                link.Write(sync, 0, sync.Length);
                int deadline = Environment.TickCount + 300;
                bool ok = false;
                while (!ok && Environment.TickCount - deadline < 0) {
                    int b = ReadByte(50);
                    if (b == 0x4F || b == 0x4B) ok = true;
                }
                if (ok) {
                    Thread.Sleep(30 + 500);
                    Drain(50);
                    return;
                }
            }
            throw new FlashException(Lang.T(
                "BL616 не отвечает. Отключите USB Tang, нажмите и держите кнопку UPDATE на модуле Tang, подключите USB, отпустите кнопку и повторите.",
                "The BL616 does not answer. Unplug the Tang USB, hold the UPDATE button on the Tang module, plug USB in, release the button and try again."));
        }

        void SetIspTimeout(int ms)
        {
            if (a0) {
                var d = new byte[8];
                Util.Put32(d, 0, 0x6102DF04);
                Util.Put32(d, 4, (uint)ms << 16 | 0x1200);
                Ack(Send(0x50, d, false), 2000);
            } else {
                var d = new byte[4];
                Util.Put32(d, 0, (uint)ms);
                Ack(Send(0x23, d, false), 2000);
            }
        }

        byte Send(byte cmd, byte[] data, bool checksum)
        {
            var f = new byte[4 + data.Length];
            f[0] = cmd;
            f[2] = (byte)data.Length;
            f[3] = (byte)(data.Length >> 8);
            Array.Copy(data, 0, f, 4, data.Length);
            if (checksum) {
                int s = f[2] + f[3];
                foreach (byte b in data) s += b;
                f[1] = (byte)s;
            }
            link.Write(f, 0, f.Length);
            return cmd;
        }

        byte[] Request(byte cmd, byte[] data, bool checksum, int timeoutMs)
        {
            for (int attempt = 0; attempt < 2; attempt++) {
                Send(cmd, data, checksum);
                string ack = TryAck(timeoutMs);
                byte[] d;
                if (ack == "OK" && TryData(timeoutMs, out d)) return d;
                Drain(50);
            }
            throw new FlashException(string.Format(Lang.T("ПЗУ BL616 не ответило на команду 0x{0:X2}", "BL616 ROM gave no answer to command 0x{0:X2}"), cmd));
        }

        void Ack(byte cmd, int timeoutMs)
        {
            int deadline = Environment.TickCount + timeoutMs;
            while (true) {
                string a = TryAck(Math.Max(1, deadline - Environment.TickCount));
                if (a == "OK") return;
                if (a == "PD" && Environment.TickCount - deadline < 0) continue;     // ещё стирает
                if (a == null) throw new FlashException(string.Format(Lang.T("ПЗУ BL616 молчит после команды 0x{0:X2}", "BL616 ROM is silent after command 0x{0:X2}"), cmd));
                throw new FlashException(string.Format(Lang.T("ПЗУ BL616 отказало: команда 0x{0:X2}, {1}", "BL616 ROM refused: command 0x{0:X2}, {1}"), cmd, a));
            }
        }

        /// <summary>«OK», «PD», «FLxxxx» или null — нет ответа.</summary>
        string TryAck(int timeoutMs)
        {
            int b0 = ReadByte(timeoutMs), b1 = b0 < 0 ? -1 : ReadByte(timeoutMs);
            if (b1 < 0) return null;
            if (b0 == 0x4F || b0 == 0x4B || b1 == 0x4F || b1 == 0x4B) return "OK";
            if (b0 == 0x50 || b0 == 0x44 || b1 == 0x50 || b1 == 0x44) return "PD";
            int c0 = ReadByte(timeoutMs), c1 = c0 < 0 ? -1 : ReadByte(timeoutMs);
            return "FL" + (c1 < 0 ? "?" : ((c1 << 8) | c0).ToString("X4"));
        }

        bool TryData(int timeoutMs, out byte[] data)
        {
            data = null;
            int l0, l1;
            while (true) {
                l0 = ReadByte(timeoutMs);
                l1 = l0 < 0 ? -1 : ReadByte(timeoutMs);
                if (l1 < 0) return false;
                if (l0 == 0x4F && l1 == 0x4B) continue;        // лишние «OK»
                break;
            }
            int len = l0 | l1 << 8;
            data = new byte[len];
            for (int i = 0; i < len; i++) {
                int b = ReadByte(timeoutMs);
                if (b < 0) return false;
                data[i] = (byte)b;
            }
            return true;
        }

        int ReadByte(int timeoutMs)
        {
            if (rxPos >= rxLen) {
                rxPos = 0;
                rxLen = link.Read(rx, 0, rx.Length, timeoutMs);
                if (rxLen <= 0) { rxLen = 0; return -1; }
            }
            return rx[rxPos++];
        }

        void Drain(int ms)
        {
            rxPos = rxLen = 0;
            while (link.Read(rx, 0, rx.Length, ms) > 0) { }
            rxPos = rxLen = 0;
        }
    }
}
