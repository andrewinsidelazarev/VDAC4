// SPDX-License-Identifier: Apache-2.0
// Образы прошивок из папки firmware рядом с программой: опись firmware.ini, загрузка, проверки.
//
// firmware.ini (пути file/slotN — от папки firmware, repo — путь файла в репозитории VDAC4 на GitHub):
//   [esp]    file = esp\firmware.factory.bin   repo = …   address = 0x0   flash_size = 8M   version = …   sha256 = …
//   [bl616]  file = bl616\vdac4_copro_bl616.bin repo = …  address = 0x40000  version = …   sha256 = …
//   [tang]   slot0 = tang\ts_fb_hdmi_top.fs   slot1 = tang\ts_fb_slot1.fs   slot3 = tang\ts_fb_video.bin
//            repo_slot0 / 1 / 3 = …   fonts = tang\romfonts.bin   version = …   sha256_slot0 / 1 / 3 = …
//   [source] commit = …   downloaded = …  — откуда скачан комплект
// Опись пишет сама программа, скачав комплект с GitHub (Manifest.Make, Downloader); в выпуске — по рабочей копии.
// Сумма sha256 у битстрима .fs — по его байтам (после разбора строк 0/1), а не по тексту файла: git хранит
// .fs с концами строк LF, а рабочая копия Windows — с CRLF; байты битстрима при этом те же.
// Нет описи — ищутся файлы с этими именами по тем же путям.
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Vdac4Flasher
{
    /// <summary>Кусок для записи во flash: адрес и данные.</summary>
    public class FlashPart
    {
        public string Name;
        public uint Address;
        public byte[] Data;
        public string File;
    }

    public class FirmwareSet
    {
        public string Folder;
        public FlashPart Esp;            // образ ESP32-S3 целиком (загрузчик, разделы, приложение)
        public uint EspFlashSize = 8u << 20;
        public string EspVersion = "";
        public FlashPart Bl616;          // прошивка BL616 модуля Tang (с 0x40000, FPGA Partner Sipeed с 0x0 не трогается)
        public string Bl616Version = "";
        public List<FlashPart> Tang = new List<FlashPart>();   // слоты flash ПЛИС и шрифты FT812
        public string TangVersion = "";
        public List<string> Problems = new List<string>();     // что не так с файлами (окно покажет)
        public string Commit = "";       // коммит VDAC4 на GitHub, с которого скачан комплект ([source])
        public string Downloaded = "";   // когда скачан

        public const uint TangSlotBytes = 0x100000;
        public const uint TangFontBase = 0x200000, TangFontEnd = 0x300000;
        /// <summary>Раздел прошивки VDAC4 во flash BL616: 0x40000…0x7FFFF (дальше — кэш ядер, его не трогаем).</summary>
        public const uint Bl616PartEnd = 0x80000;

        static string DefaultSlotFile(int slot)
        {
            return slot == 0 ? @"tang\ts_fb_hdmi_top.fs" : slot == 1 ? @"tang\ts_fb_slot1.fs" : @"tang\ts_fb_video.bin";
        }

        /// <summary>Сумма для описи: у .fs — по байтам битстрима, у прочих — по файлу как есть.</summary>
        public static string ContentSha256(string name, byte[] file)
        {
            return Sha256(name.EndsWith(".fs", StringComparison.OrdinalIgnoreCase) ? ParseFs(file, name) : file);
        }

        public static FirmwareSet Load(string folder)
        {
            var set = new FirmwareSet { Folder = folder };
            var ini = Ini.Read(Path.Combine(folder, "firmware.ini"));
            set.Commit = ini.Get("source", "commit", "");
            set.Downloaded = ini.Get("source", "downloaded", "");

            string espFile = ini.Get("esp", "file", @"esp\firmware.factory.bin");
            set.EspVersion = ini.Get("esp", "version", "");
            string fs = ini.Get("esp", "flash_size", "8M").Trim().ToUpperInvariant();
            if (fs.EndsWith("M")) set.EspFlashSize = uint.Parse(fs.TrimEnd('M')) << 20;
            set.Esp = set.LoadPart("ESP32-S3", espFile, ini.Get("esp", "address", "0x0"), ini.Get("esp", "sha256", ""),
                                   CheckEsp);

            set.Bl616Version = ini.Get("bl616", "version", "");
            set.Bl616 = set.LoadPart("BL616", ini.Get("bl616", "file", @"bl616\vdac4_copro_bl616.bin"),
                                     ini.Get("bl616", "address", "0x40000"), ini.Get("bl616", "sha256", ""), CheckBl616);

            set.TangVersion = ini.Get("tang", "version", "");
            // Постоянные ядра: слот 0 — TS-Config, 1 — FT812, 3 — видео; слот 2 — шрифты
            foreach (int slot in new[] { 0, 1, 3 }) {
                var part = set.LoadPart(Lang.T("ПЛИС, слот ", "FPGA, slot ") + slot,
                                        ini.Get("tang", "slot" + slot, DefaultSlotFile(slot)), "0x" + (slot * TangSlotBytes).ToString("X"),
                                        ini.Get("tang", "sha256_slot" + slot, ""), CheckGowin);
                if (part != null) {
                    if (part.Data.Length > TangSlotBytes)
                        set.Problems.Add(part.File + ": " + Lang.T("длиннее слота 1 МБ", "longer than a 1 MB slot"));
                    else
                        set.Tang.Add(part);
                }
            }
            // шрифты ПЗУ FT812 (слот 2, VD4FONT1): данные Bridgetek, в комплект не входят — только свой файл
            string fonts = ini.Get("tang", "fonts", @"tang\romfonts.bin");
            if (File.Exists(Path.Combine(folder, fonts))) {
                byte[] img = File.ReadAllBytes(Path.Combine(folder, fonts));
                try {
                    set.Tang.AddRange(FontSpans(img, fonts));
                } catch (FlashException e) {
                    set.Problems.Add(fonts + ": " + e.Message);
                }
            }
            return set;
        }

        FlashPart LoadPart(string name, string rel, string address, string sha, Action<byte[]> check)
        {
            string path = Path.Combine(Folder, rel);
            if (!File.Exists(path)) {
                Problems.Add(Lang.T("нет файла ", "missing file ") + rel);
                return null;
            }
            byte[] data;
            try {
                data = path.EndsWith(".fs", StringComparison.OrdinalIgnoreCase) ? ReadFs(path) : File.ReadAllBytes(path);
            } catch (FlashException e) {
                Problems.Add(rel + ": " + e.Message);
                return null;
            }
            if (!string.IsNullOrEmpty(sha)) {
                string got = Sha256(data);          // у .fs — байты битстрима (см. начало файла)
                if (!got.Equals(sha.Trim(), StringComparison.OrdinalIgnoreCase)) {
                    Problems.Add(rel + ": " + Lang.T("SHA-256 не совпадает с описью — файл испорчен или подменён",
                                                     "SHA-256 differs from firmware.ini — the file is damaged or replaced"));
                    return null;
                }
            }
            try {
                check(data);
            } catch (FlashException e) {
                Problems.Add(rel + ": " + e.Message);
                return null;
            }
            return new FlashPart { Name = name, Address = Util.ParseNumber(address), Data = data, File = rel };
        }

        public static string Sha256(byte[] data)
        {
            using (var h = SHA256.Create()) return Util.Hex(h.ComputeHash(data));
        }

        /// <summary>Битстрим Gowin .fs: строки из 0 и 1, «//» — заголовок (как tang_tool.py load_image).</summary>
        public static byte[] ReadFs(string path)
        {
            return ParseFs(File.ReadAllBytes(path), Path.GetFileName(path));
        }

        /// <summary>Текст .fs → байты битстрима; концы строк любые (LF или CRLF).</summary>
        public static byte[] ParseFs(byte[] text, string name)
        {
            var data = new MemoryStream(text.Length / 8 + 16);
            int v = 0, n = 0;
            bool lineStart = true, comment = false;
            for (int i = 0; i < text.Length; i++) {
                byte c = text[i];
                if (c == '\n' || c == '\r') { lineStart = true; comment = false; continue; }
                if (comment) continue;
                if (lineStart && (c == ' ' || c == '\t')) continue;
                if (lineStart && c == '/' && i + 1 < text.Length && text[i + 1] == '/') { comment = true; continue; }
                lineStart = false;
                if (c == ' ' || c == '\t') continue;
                if (c != '0' && c != '1')
                    throw new FlashException(name + ": " + Lang.T("в строке битстрима не только 0 и 1", "bitstream line has more than 0 and 1"));
                v = v << 1 | (c - '0');
                if (++n == 8) { data.WriteByte((byte)v); v = 0; n = 0; }
            }
            if (n != 0)
                throw new FlashException(name + ": " + Lang.T("число битов не кратно 8", "bit count is not a multiple of 8"));
            return data.ToArray();
        }

        internal static void CheckEsp(byte[] d)
        {
            if (d.Length < 0x10000 || d[0] != 0xE9)
                throw new FlashException(Lang.T("это не полный образ ESP (нет загрузчика 0xE9 в начале)",
                                                "not a full ESP image (no 0xE9 bootloader at the start)"));
        }

        internal static void CheckBl616(byte[] d)
        {
            if (d.Length < 0x1000 || d[0] != 'B' || d[1] != 'F' || d[2] != 'N' || d[3] != 'P')
                throw new FlashException(Lang.T("это не образ BL616 (нет заголовка BFNP)", "not a BL616 image (no BFNP header)"));
            if (d.Length > 0x40000)
                throw new FlashException(Lang.T("образ BL616 длиннее 256 КБ", "BL616 image is longer than 256 KB"));
        }

        static readonly byte[] IdcodeCheck = { 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B };

        internal static void CheckGowin(byte[] d)
        {
            if (IndexOf(d, new byte[] { 0xA5, 0xC3 }, 64) < 0)
                throw new FlashException(Lang.T("нет слова синхронизации A5C3 — это не битстрим Gowin",
                                                "no A5C3 sync word — not a Gowin bitstream"));
            if (IndexOf(d, IdcodeCheck, 64) < 0)
                throw new FlashException(Lang.T("битстрим не для GW2AR-18 (нет проверки IDCODE 0x0000081B)",
                                                "bitstream is not for GW2AR-18 (no IDCODE 0x0000081B check)"));
        }

        /// <summary>Адрес перехода multiboot из заголовка (команда D2 00 FF FF + адрес), null — нет.</summary>
        public static uint? JumpAddress(byte[] d)
        {
            int at = IndexOf(d, new byte[] { 0xD2, 0x00, 0xFF, 0xFF }, 128);
            if (at < 0 || at + 8 > d.Length) return null;
            return (uint)(d[at + 4] << 24 | d[at + 5] << 16 | d[at + 6] << 8 | d[at + 7]);
        }

        static int IndexOf(byte[] d, byte[] what, int within)
        {
            for (int i = 0; i + what.Length <= Math.Min(within, d.Length); i++) {
                int k = 0;
                while (k < what.Length && d[i + k] == what[k]) k++;
                if (k == what.Length) return i;
            }
            return -1;
        }

        /// <summary>Шрифты ПЗУ FT812 VD4FONT1: куски {адрес, длина, смещение} — те же проверки, что у ESP.</summary>
        public static List<FlashPart> FontSpans(byte[] img, string file)
        {
            if (img.Length < 12 || Encoding.ASCII.GetString(img, 0, 8) != "VD4FONT1")
                throw new FlashException(Lang.T("это не образ шрифтов VD4FONT1", "not a VD4FONT1 font image"));
            uint count = Util.Get32(img, 8);
            if (count < 1 || count > 8 || img.Length < 12 + 12 * count)
                throw new FlashException(Lang.T("образ шрифтов: число кусков ", "font image: span count ") + count);
            var parts = new List<FlashPart>();
            for (int i = 0; i < count; i++) {
                uint addr = Util.Get32(img, 12 + 12 * i), len = Util.Get32(img, 16 + 12 * i), off = Util.Get32(img, 20 + 12 * i);
                if (len == 0 || ((addr | len | off) & 3) != 0 || addr < TangFontBase || addr + len > TangFontEnd ||
                    off < 12 + 12 * count || off + len > img.Length)
                    throw new FlashException(Lang.T("образ шрифтов: негодный кусок ", "font image: bad span ") + i);
                var data = new byte[len];
                Array.Copy(img, off, data, 0, len);
                parts.Add(new FlashPart { Name = Lang.T("шрифты FT812, кусок ", "FT812 fonts, span ") + i, Address = addr,
                                          Data = data, File = file });
            }
            return parts;
        }
    }

    /// <summary>Простейший INI: [секция], ключ = значение, «;» и «#» — комментарии.</summary>
    public class Ini
    {
        readonly Dictionary<string, string> values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        public static Ini Read(string path)
        {
            return File.Exists(path) ? Parse(File.ReadAllText(path, Encoding.UTF8)) : new Ini();
        }

        public static Ini Parse(string text)
        {
            var ini = new Ini();
            string section = "";
            foreach (string raw in text.Split('\n')) {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                if (line[0] == '[' && line.EndsWith("]")) { section = line.Substring(1, line.Length - 2).Trim(); continue; }
                int eq = line.IndexOf('=');
                if (eq > 0) ini.values[section + "." + line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
            }
            return ini;
        }

        public string Get(string section, string key, string def)
        {
            string v;
            return values.TryGetValue(section + "." + key, out v) && v.Length > 0 ? v : def;
        }
    }
}
