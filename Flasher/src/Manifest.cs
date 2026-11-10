// SPDX-License-Identifier: Apache-2.0
// Что программа берёт с GitHub и как пишет опись firmware.ini в папке firmware.
//
// Списка прошивок на GitHub нет (10.11.2026): опись, которую надо было обновлять при каждом пуше образов,
// отставала от них — и программа отказывалась качать. Теперь пути образов в репозитории VDAC4 — здесь, в
// программе; целость каждого скачанного файла сверяется с хэшем git того же коммита (API GitHub), версии
// читаются из файлов репозитория того же коммита:
//   ESP   — строка VERSION в firmware.sha256 (там же SHA-256 образа firmware.factory.bin);
//   BL616 — последний #define FW_VERSION в Tang Nano 20k/BL616/app/main.c;
//   ПЛИС  — колонка версии в таблице слотов Tang Nano 20k/Firmware/README.md.
// Скачав комплект, программа пишет firmware.ini (Make): по нему FirmwareSet.Load сверяет файлы и потом,
// без сети.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Vdac4Flasher
{
    /// <summary>Файл комплекта: куда положить в папке firmware и где он лежит в репозитории VDAC4.</summary>
    public class FirmwareEntry
    {
        public string Local;      // путь от папки firmware (esp\firmware.factory.bin)
        public string Repo;       // путь в репозитории (ESP32 S3 Zero N8R8/Firmware/…)
        public string Sha256 = "";   // сумма содержимого (у .fs — байтов битстрима), для описи
    }

    public static class Manifest
    {
        public const string EspRepo = "ESP32 S3 Zero N8R8/Firmware/firmware/vdac4/firmware.factory.bin";
        public const string EspSumsRepo = "ESP32 S3 Zero N8R8/Firmware/firmware/vdac4/firmware.sha256";
        public const string Bl616Repo = "Tang Nano 20k/BL616/firmware/vdac4_copro_bl616.bin";
        public const string Bl616MainRepo = "Tang Nano 20k/BL616/app/main.c";
        public const string TangReadmeRepo = "Tang Nano 20k/Firmware/README.md";
        public static readonly string[] SlotRepo = {
            "Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.fs", "Tang Nano 20k/Firmware/firmware/ts_fb_slot1.fs",
            null, "Tang Nano 20k/Firmware/firmware/ts_fb_video.bin"
        };
        public static readonly int[] Slots = { 0, 1, 3 };

        /// <summary>Образы комплекта по порядку: ESP, BL616, слоты ПЛИС 0, 1, 3 (шрифты — только свои, не с GitHub).</summary>
        public static List<FirmwareEntry> Entries()
        {
            var list = new List<FirmwareEntry>();
            list.Add(new FirmwareEntry { Local = @"esp\firmware.factory.bin", Repo = EspRepo });
            list.Add(new FirmwareEntry { Local = @"bl616\vdac4_copro_bl616.bin", Repo = Bl616Repo });
            foreach (int slot in Slots)
                list.Add(new FirmwareEntry { Local = @"tang\" + RepoName(SlotRepo[slot]), Repo = SlotRepo[slot] });
            return list;
        }

        static string RepoName(string repo) { return repo.Substring(repo.LastIndexOf('/') + 1); }

        /// <summary>Версии и сумма образа ESP из файлов репозитория (пусто — не нашлось).</summary>
        public class Versions
        {
            public string Esp = "", EspFactorySha256 = "", Bl616 = "", Tang = "";
        }

        public static Versions ReadVersions(string espSums, string bl616Main, string tangReadme)
        {
            var v = new Versions();
            foreach (string raw in espSums.Split('\n')) {
                string line = raw.Trim();
                if (line.StartsWith("VERSION ")) v.Esp = line.Substring(8).Trim();
                var m = Regex.Match(line, @"^([0-9A-Fa-f]{64})\s+\*?firmware\.factory\.bin$");
                if (m.Success) v.EspFactorySha256 = m.Groups[1].Value.ToLowerInvariant();
            }
            // последний FW_VERSION: первый — у моста FTC_BRIDGE (#ifdef), второй — у прошивки
            Match fv = null;
            foreach (Match m in Regex.Matches(bl616Main, @"^\s*#define\s+FW_VERSION\s+(0x[0-9A-Fa-f]+|\d+)", RegexOptions.Multiline)) fv = m;
            if (fv != null) {
                string g = fv.Groups[1].Value;
                v.Bl616 = (g.StartsWith("0x") ? Convert.ToInt32(g.Substring(2), 16) : int.Parse(g)).ToString();
            }
            var tang = new List<string>();
            foreach (int slot in Slots) {
                var m = Regex.Match(tangReadme, @"\]\(firmware/" + Regex.Escape(RepoName(SlotRepo[slot])) + @"\)\s*\|\s*(\d+\.\d+)");
                tang.Add(m.Success ? m.Groups[1].Value : "?");
            }
            v.Tang = string.Join(" / ", tang.ToArray());
            return v;
        }

        /// <summary>Опись firmware.ini для папки firmware: пути, адреса, суммы, версии; commit — откуда скачан комплект.</summary>
        public static string Make(List<FirmwareEntry> entries, Versions v, string commit, string downloaded)
        {
            var sb = new StringBuilder();
            sb.Append("; Прошивки VDAC4 для «Первой прошивки VDAC4»: что и куда записывать. Опись пишет сама программа, скачав\r\n");
            sb.Append("; комплект с GitHub (" + Github.Repo + "): пути file/slotN — от папки firmware рядом с программой, repo — путь\r\n");
            sb.Append("; в репозитории. По суммам sha256 программа сверяет файлы перед прошивкой (у .fs — по байтам битстрима).\r\n\r\n");
            sb.Append("[esp]\r\n");
            sb.Append("; ESP32-S3 Zero (N8R8): полный образ — загрузчик, таблица разделов, приложение (merged)\r\n");
            sb.Append("file = " + entries[0].Local + "\r\n");
            sb.Append("repo = " + entries[0].Repo + "\r\n");
            sb.Append("address = 0x0\r\nflash_size = 8M\r\n");
            sb.Append("version = " + v.Esp + "\r\n");
            sb.Append("sha256 = " + entries[0].Sha256 + "\r\n\r\n");
            sb.Append("[bl616]\r\n");
            sb.Append("; BL616 модуля Tang Nano 20K: сопроцессор FT812 и кэш ядер VDAC4 — с 0x40000;\r\n");
            sb.Append("; FPGA Partner Sipeed с 0x0 (USB-JTAG для ПЛИС) не трогается\r\n");
            sb.Append("file = " + entries[1].Local + "\r\n");
            sb.Append("repo = " + entries[1].Repo + "\r\n");
            sb.Append("address = 0x40000\r\n");
            sb.Append("version = " + v.Bl616 + "\r\n");
            sb.Append("sha256 = " + entries[1].Sha256 + "\r\n\r\n");
            sb.Append("[tang]\r\n");
            sb.Append("; flash ПЛИС: слот 0 (0x000000) — TS-Config, грузится при включении; слот 1 (0x100000) — FT812;\r\n");
            sb.Append("; слот 3 (0x300000) — ядро видео\r\n");
            for (int i = 0; i < Slots.Length; i++) sb.Append("slot" + Slots[i] + " = " + entries[2 + i].Local + "\r\n");
            for (int i = 0; i < Slots.Length; i++) sb.Append("repo_slot" + Slots[i] + " = " + entries[2 + i].Repo + "\r\n");
            sb.Append("version = " + v.Tang + "\r\n");
            for (int i = 0; i < Slots.Length; i++) sb.Append("sha256_slot" + Slots[i] + " = " + entries[2 + i].Sha256 + "\r\n");
            sb.Append("; шрифты ПЗУ FT812 (слот 2, образ VD4FONT1 — данные Bridgetek, в комплект и на GitHub не входят):\r\n");
            sb.Append("; свой файл положить в папку firmware — программа запишет и его\r\n");
            sb.Append("fonts = tang\\romfonts.bin\r\n");
            if (!string.IsNullOrEmpty(commit)) {
                sb.Append("\r\n[source]\r\n");
                sb.Append("; откуда и когда скачан комплект\r\n");
                sb.Append("commit = " + commit + "\r\n");
                sb.Append("downloaded = " + downloaded + "\r\n");
            }
            return sb.ToString();
        }

        /// <summary>Опись по файлам рабочей копии репозитория v4 (комплект выпуска и проверки на ПК).</summary>
        public static string MakeFromRepo(string v4)
        {
            Func<string, string> local = repo => Path.Combine(v4, repo.Replace('/', '\\'));
            var entries = Entries();
            foreach (var e in entries) e.Sha256 = FirmwareSet.ContentSha256(e.Repo, File.ReadAllBytes(local(e.Repo)));
            var v = ReadVersions(File.ReadAllText(local(EspSumsRepo)), File.ReadAllText(local(Bl616MainRepo)),
                                 File.ReadAllText(local(TangReadmeRepo)));
            return Make(entries, v, null, null);
        }
    }
}
