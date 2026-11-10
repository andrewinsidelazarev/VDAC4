// SPDX-License-Identifier: Apache-2.0
// Проверки «Первой прошивки VDAC4» на ПК: опись и версии по файлам репозитория, загрузка с GitHub (подменой —
// без сети: API коммита, списки каталогов с хэшами git, raw), бэкап, стирание и прошивка ESP32-S3, BL616 и flash
// ПЛИС на моделях устройств (Models.cs) настоящими образами из репозитория VDAC4; разбор .fs и шрифтов.
// Ожидается «ВСЁ ВЕРНО».
// Запуск: FlasherTests.exe [папка VDAC4]
//         FlasherTests.exe --make-ini <папка VDAC4> <файл>  — опись комплекта выпуска по рабочей копии (build.ps1)
//         FlasherTests.exe --online                         — скачать комплект с настоящего GitHub во временную папку
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Vdac4Flasher;

namespace FlasherTests
{
    class ConsoleReport : IReport
    {
        public readonly List<string> Lines = new List<string>();
        public int LastPercent;
        public bool Quiet;
        public bool Cancelled { get { return false; } }
        public void Log(string text) { Lines.Add(text); if (!Quiet) Console.WriteLine("    " + text); }
        public void Progress(int percent, string stage)
        {
            if (percent < LastPercent && percent != 0) throw new ModelError("ход назад: " + LastPercent + " → " + percent + " (" + stage + ")");
            LastPercent = percent;
        }
    }

    /// <summary>Подмена GitHub из файлов репозитория на диске: API коммита, списки каталогов с хэшами git (как
    /// отдаёт API: объекты с вложенным «_links») и raw.githubusercontent.com.</summary>
    class FakeGithub : IFetch
    {
        public const string Commit = "0123456789abcdef0123456789abcdef01234567";
        readonly string v4;
        public bool ApiDown;                       // API не отвечает вовсе: ни коммита, ни списков
        public bool ListingDown;                   // коммит есть, списки каталогов — 403 (предел запросов)
        public string CorruptRepo;                 // этот файл отдать испорченным
        public string WrongBlobRepo;               // у этого файла в списке каталога чужой хэш
        public string MissingRepo;                 // этого файла в коммите нет
        public readonly List<string> Urls = new List<string>();

        public FakeGithub(string v4) { this.v4 = v4; }

        /// <summary>Файл, как его хранит git: текст — с LF (в рабочей копии Windows — CRLF), .bin — как есть.</summary>
        byte[] Blob(string path)
        {
            string file = Path.Combine(v4, path.Replace('/', '\\'));
            if (path == MissingRepo || !File.Exists(file)) return null;
            byte[] data = File.ReadAllBytes(file);
            if (!path.EndsWith(".bin")) data = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(data).Replace("\r\n", "\n"));
            return data;
        }

        public byte[] Get(string url, string accept, Action<long, long> progress, Func<bool> cancelled)
        {
            Urls.Add(url);
            if (url == Github.CommitUrl) {
                if (ApiDown) throw new FlashException("GitHub ответил 403 на " + url);
                if (accept != "application/vnd.github.sha") throw new ModelError("API: без Accept vnd.github.sha");
                return Encoding.ASCII.GetBytes(Commit + "\n");
            }
            string api = "https://api.github.com/repos/" + Github.Repo + "/contents/";
            if (url.StartsWith(api)) {
                if (ApiDown || ListingDown) throw new FlashException("GitHub ответил 403 на " + url);
                if (accept != "application/vnd.github+json") throw new ModelError("API: без Accept vnd.github+json");
                int q = url.IndexOf("?ref=");
                if (q < 0 || url.Substring(q + 5) != Commit) throw new ModelError("список каталога не по коммиту: " + url);
                string dir = Uri.UnescapeDataString(url.Substring(api.Length, q - api.Length));
                var sb = new StringBuilder("[\n");
                foreach (string f in Directory.GetFiles(Path.Combine(v4, dir.Replace('/', '\\')))) {
                    string path = dir + "/" + Path.GetFileName(f);
                    byte[] blob = Blob(path);
                    if (blob == null) continue;
                    string sha = path == WrongBlobRepo ? new string('0', 40) : Github.GitBlobSha1(blob);
                    sb.Append("  {\"name\": \"" + Path.GetFileName(f) + "\", \"path\": \"" + path + "\", \"sha\": \"" + sha +
                              "\", \"size\": " + blob.Length + ", \"type\": \"file\", \"_links\": {\"self\": \"x\", \"git\": " +
                              "\"https://api.github.com/repos/x/git/blobs/" + new string('f', 40) + "\"}},\n");
                }
                sb.Append("  {\"name\": \"sub\", \"path\": \"" + dir + "/sub\", \"sha\": \"" + new string('e', 40) + "\", \"type\": \"dir\"}\n]");
                return Encoding.UTF8.GetBytes(sb.ToString());
            }
            string prefix = "https://raw.githubusercontent.com/" + Github.Repo + "/";
            if (!url.StartsWith(prefix)) throw new ModelError("чужой адрес: " + url);
            string rest = url.Substring(prefix.Length);
            string reference = ApiDown ? "refs/heads/main/" : Commit + "/";
            if (!rest.StartsWith(reference)) throw new ModelError("не тот коммит: " + url);
            string p = Uri.UnescapeDataString(rest.Substring(reference.Length));
            byte[] data = Blob(p);
            if (data == null) throw new FlashException("GitHub ответил 404 на " + url);
            if (p == CorruptRepo) { data = (byte[])data.Clone(); data[data.Length / 2] ^= 1; }
            if (progress != null) { progress(data.Length / 2, data.Length); progress(data.Length, data.Length); }
            return data;
        }
    }

    static class TestMain
    {
        static int checks, errors;
        static string v4, tmp;

        static void Check(bool ok, string what)
        {
            checks++;
            if (ok) Console.WriteLine("  ок: " + what);
            else { errors++; Console.WriteLine("  ОШИБКА: " + what); }
        }

        static void Throws(Action a, string contains, string what)
        {
            try {
                a();
                Check(false, what + " — исключения нет");
            } catch (Exception e) {
                Check(e.Message.Contains(contains), what + " — «" + e.Message + "»");
            }
        }

        static int Main(string[] args)
        {
            Console.OutputEncoding = Encoding.UTF8;
            Lang.Russian = true;
            if (args.Length > 2 && args[0] == "--make-ini") {
                // опись комплекта выпуска (build.ps1 -Release) — по файлам рабочей копии репозитория
                File.WriteAllText(args[2], Manifest.MakeFromRepo(args[1]), new UTF8Encoding(false));
                Console.WriteLine("записано: " + args[2]);
                return 0;
            }
            if (args.Length > 0 && args[0] == "--online") return Online();

            v4 = args.Length > 0 ? args[0] : @"E:\zx\Tang Nano 20k VDAC\VDAC4";
            tmp = Path.Combine(Environment.GetEnvironmentVariable("TEMP") ?? ".", "flasher_tests");
            if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
            Directory.CreateDirectory(tmp);
            try {
                var fw = TestFirmware();
                TestDownload();
                TestBackupFiles();
                TestEsp(fw);
                TestBl616(fw);
                TestGowin(fw);
            } catch (Exception e) {
                errors++;
                Console.WriteLine("  ОШИБКА (исключение): " + e);
            }
            Console.WriteLine("проверок " + checks + ", ошибок " + errors);
            Console.WriteLine(errors == 0 ? "ВСЁ ВЕРНО" : "ЕСТЬ ОШИБКИ");
            return errors == 0 ? 0 : 1;
        }

        /// <summary>Настоящий GitHub: образы ветки main во временную папку, как это делает программа.</summary>
        static int Online()
        {
            string folder = Path.Combine(Environment.GetEnvironmentVariable("TEMP") ?? ".", "flasher_online", "firmware");
            if (Directory.Exists(folder)) Directory.Delete(folder, true);
            Directory.CreateDirectory(folder);
            var r = new ConsoleReport();
            try {
                var res = Downloader.Download(new HttpFetch(), folder, r);
                var set = FirmwareSet.Load(folder);
                Console.WriteLine("коммит " + res.Commit + (res.GitChecked ? ", хэши git сошлись" : ", без хэшей git") +
                                  ", файлов " + res.Files + ", " + Util.Size(res.Bytes));
                Console.WriteLine("ESP " + set.EspVersion + ", BL616 " + set.Bl616Version + ", ПЛИС " + set.TangVersion);
                foreach (string p in set.Problems) Console.WriteLine("  замечание: " + p);
                Console.WriteLine(set.Problems.Count == 0 ? "СКАЧАНО ВЕРНО" : "ЕСТЬ ЗАМЕЧАНИЯ");
                return set.Problems.Count == 0 ? 0 : 1;
            } catch (Exception e) {
                Console.WriteLine("ОШИБКА: " + e.Message);
                return 1;
            }
        }

        // ---- образы и опись -------------------------------------------------------------------------

        static FirmwareSet TestFirmware()
        {
            Console.WriteLine("== образы и опись");
            string fs = Path.Combine(tmp, "t.fs");
            File.WriteAllText(fs, "//Device: GW2AR-18C\n0101010111110000\n\n10100101\n", Encoding.ASCII);
            byte[] b = FirmwareSet.ReadFs(fs);
            Check(b.Length == 3 && b[0] == 0x55 && b[1] == 0xF0 && b[2] == 0xA5, ".fs → байты старшим битом вперёд, «//» — заголовок");
            File.WriteAllText(fs, "//Device: GW2AR-18C\r\n0101010111110000\r\n\r\n10100101\r\n", Encoding.ASCII);
            Check(FirmwareSet.ReadFs(fs).SequenceEqual(b), ".fs с CRLF — те же байты");
            File.WriteAllText(fs, "0101x\n", Encoding.ASCII);
            Throws(() => FirmwareSet.ReadFs(fs), "не только 0 и 1", ".fs с чужим знаком");

            // опись по файлам репозитория: версии — из firmware.sha256, BL616 app/main.c и README ПЛИС
            string made = Manifest.MakeFromRepo(v4);
            string espVersion = File.ReadAllLines(Path.Combine(v4, @"ESP32 S3 Zero N8R8\Firmware\firmware\vdac4\firmware.sha256"))
                                    .First(l => l.StartsWith("VERSION ")).Substring(8).Trim();
            var vers = Ini.Parse(made);
            Check(vers.Get("esp", "version", "") == espVersion && System.Text.RegularExpressions.Regex.IsMatch(vers.Get("bl616", "version", ""), @"^\d+$") &&
                  System.Text.RegularExpressions.Regex.IsMatch(vers.Get("tang", "version", ""), @"^\d+\.\d+ / \d+\.\d+ / \d+\.\d+$"),
                  "версии из репозитория: ESP " + vers.Get("esp", "version", "") + ", BL616 " + vers.Get("bl616", "version", "") +
                  ", ПЛИС " + vers.Get("tang", "version", ""));
            Check(Github.GitBlobSha1(Encoding.ASCII.GetBytes("hello\n")) == "ce013625030ba8dba906f756967f9e9ca394464a",
                  "хэш git: «hello\\n» — ce01362… (как git hash-object)");
            var listing = Github.ParseListing("[{\"name\":\"a b.fs\",\"path\":\"x\\/a b.fs\",\"sha\":\"" + new string('A', 40) +
                                              "\",\"_links\":{\"sha\":\"" + new string('1', 40) + "\",\"path\":\"no\"}}," +
                                              "{\"_links\":{},\"sha\":\"" + new string('2', 40) + "\",\"path\":\"y\"}]");
            Check(listing.Count == 2 && listing["x/a b.fs"] == new string('a', 40) && listing["y"] == new string('2', 40),
                  "список каталога API: «path» и «sha» объекта, не вложенного «_links»; «\\/» в пути");

            // комплект из настоящих файлов
            string folder = Path.Combine(tmp, "firmware");
            Directory.CreateDirectory(Path.Combine(folder, "esp"));
            Directory.CreateDirectory(Path.Combine(folder, "bl616"));
            Directory.CreateDirectory(Path.Combine(folder, "tang"));
            File.Copy(Path.Combine(v4, @"ESP32 S3 Zero N8R8\Firmware\firmware\vdac4\firmware.factory.bin"), Path.Combine(folder, @"esp\firmware.factory.bin"));
            File.Copy(Path.Combine(v4, @"Tang Nano 20k\BL616\firmware\vdac4_copro_bl616.bin"), Path.Combine(folder, @"bl616\vdac4_copro_bl616.bin"));
            File.Copy(Path.Combine(v4, @"Tang Nano 20k\Firmware\firmware\ts_fb_hdmi_top.fs"), Path.Combine(folder, @"tang\ts_fb_hdmi_top.fs"));
            File.Copy(Path.Combine(v4, @"Tang Nano 20k\Firmware\firmware\ts_fb_slot1.fs"), Path.Combine(folder, @"tang\ts_fb_slot1.fs"));
            File.Copy(Path.Combine(v4, @"Tang Nano 20k\Firmware\firmware\ts_fb_video.bin"), Path.Combine(folder, @"tang\ts_fb_video.bin"));
            File.WriteAllText(Path.Combine(folder, "firmware.ini"), made, new UTF8Encoding(false));
            var set = FirmwareSet.Load(folder);
            Check(set.Problems.Count == 0, "комплект из репозитория без замечаний" + (set.Problems.Count > 0 ? ": " + string.Join("; ", set.Problems) : ""));
            Check(set.Esp != null && set.Esp.Address == 0 && set.Esp.Data[0] == 0xE9 && set.EspFlashSize == 8u << 20, "ESP: образ с 0x0, flash 8 МБ");
            Check(set.Bl616 != null && set.Bl616.Address == 0x40000, "BL616: с 0x40000");
            Check(set.Tang.Count == 3 && set.Tang[0].Address == 0 && set.Tang[1].Address == 0x100000 && set.Tang[2].Address == 0x300000, "ПЛИС: слоты 0, 1 и 3");
            Console.WriteLine("    версии: ESP " + set.EspVersion + ", BL616 " + set.Bl616Version + ", ПЛИС " + set.TangVersion);
            uint? j0 = FirmwareSet.JumpAddress(set.Tang[0].Data), j1 = FirmwareSet.JumpAddress(set.Tang[1].Data);
            Check(j0 == 0x100000, "слот 0 переходит в 0x100000 (" + (j0.HasValue ? "0x" + j0.Value.ToString("X") : "нет") + ")");
            Console.WriteLine("    слот 1: переход " + (j1.HasValue ? "0x" + j1.Value.ToString("X") : "нет"));

            // испорченный файл — по сумме описи
            byte[] bl = File.ReadAllBytes(Path.Combine(folder, @"bl616\vdac4_copro_bl616.bin"));
            bl[100] ^= 1;
            File.WriteAllBytes(Path.Combine(folder, @"bl616\vdac4_copro_bl616.bin"), bl);
            var bad = FirmwareSet.Load(folder);
            Check(bad.Bl616 == null && bad.Problems.Any(p => p.Contains("SHA-256")), "испорченный образ BL616 не принят (SHA-256)");
            bl[100] ^= 1;
            File.WriteAllBytes(Path.Combine(folder, @"bl616\vdac4_copro_bl616.bin"), bl);

            // шрифты VD4FONT1
            var font = MakeFont(0x200000);
            var spans = FirmwareSet.FontSpans(font, "romfonts.bin");
            Check(spans.Count == 1 && spans[0].Address == 0x200000 && spans[0].Data.Length == 64 && spans[0].Data[63] == 63, "шрифты: кусок по 0x200000");
            Util.Put32(font, 12, 0x100000);
            Throws(() => FirmwareSet.FontSpans(font, "f"), "негодный кусок", "шрифты в слот 1 не пишутся");
            return set;
        }

        /// <summary>Образ шрифтов VD4FONT1 из одного куска в 64 байта по адресу addr.</summary>
        static byte[] MakeFont(uint addr)
        {
            var font = new byte[12 + 12 + 64];
            Encoding.ASCII.GetBytes("VD4FONT1").CopyTo(font, 0);
            Util.Put32(font, 8, 1);
            Util.Put32(font, 12, addr); Util.Put32(font, 16, 64); Util.Put32(font, 20, 24);
            for (int i = 0; i < 64; i++) font[24 + i] = (byte)i;
            return font;
        }

        // ---- загрузка с GitHub ---------------------------------------------------------------------------

        static void TestDownload()
        {
            Console.WriteLine("== загрузка с GitHub (подмена: файлы репозитория, текст — с LF, как в git; описи на GitHub нет)");
            string folder = Path.Combine(tmp, "dl", "firmware");
            Directory.CreateDirectory(Path.Combine(folder, "tang"));
            byte[] ownFont = MakeFont(0x200000);
            File.WriteAllBytes(Path.Combine(folder, @"tang\romfonts.bin"), ownFont);     // свой файл шрифтов
            File.WriteAllText(Path.Combine(folder, "firmware.ini"), "; старая опись\r\n");

            var gh = new FakeGithub(v4);
            var r = new ConsoleReport { Quiet = true };
            var res = Downloader.Download(gh, folder, r);
            var set = FirmwareSet.Load(folder);
            var repoIni = Ini.Parse(Manifest.MakeFromRepo(v4));
            Check(res.Commit == FakeGithub.Commit && res.GitChecked && res.Files == 5,
                  "скачано 5 образов с одного коммита " + res.Commit.Substring(0, 7) + ", каждый сверен с хэшем git (" + Util.Size(res.Bytes) + ")");
            Check(gh.Urls.Skip(1).All(u => u.Contains("/" + FakeGithub.Commit + "/") || u.EndsWith("?ref=" + FakeGithub.Commit)),
                  "списки каталогов, версии и образы — по номеру коммита, не по ветке");
            Check(gh.Urls.Count(u => u.Contains("/contents/")) == 3, "три списка каталогов (ESP, BL616, ПЛИС)");
            Check(!gh.Urls.Any(u => u.Contains("Flasher/")), "описи с GitHub не берётся");
            Check(gh.Urls.Any(u => u.Contains("ESP32%20S3%20Zero%20N8R8/Firmware/firmware/vdac4/firmware.factory.bin")), "пробелы в пути — %20");
            Check(set.Problems.Count == 0 && set.Esp != null && set.Bl616 != null && set.Tang.Count == 4,
                  "скачанный комплект без замечаний: слоты 0, 1, 3 и свои шрифты" + (set.Problems.Count > 0 ? ": " + string.Join("; ", set.Problems) : ""));
            Check(set.EspVersion == repoIni.Get("esp", "version", "-") && set.Bl616Version == repoIni.Get("bl616", "version", "-") &&
                  set.TangVersion == repoIni.Get("tang", "version", "-"), "версии — те же, что в файлах репозитория");
            Check(set.Commit == FakeGithub.Commit && set.Downloaded.Length > 0, "в описи отмечено, откуда и когда скачано");
            Check(File.ReadAllBytes(Path.Combine(folder, @"tang\romfonts.bin")).SequenceEqual(ownFont), "свой файл шрифтов цел");
            Check(!Directory.Exists(folder + ".download"), "временная папка убрана");
            Check(r.LastPercent == 100, "ход загрузки до 100 %");

            // испорченный файл и чужой хэш: ничего не заменено
            string iniBefore = File.ReadAllText(Path.Combine(folder, "firmware.ini"));
            byte[] blBefore = File.ReadAllBytes(Path.Combine(folder, @"bl616\vdac4_copro_bl616.bin"));
            gh = new FakeGithub(v4) { CorruptRepo = Manifest.SlotRepo[3] };
            Throws(() => Downloader.Download(gh, folder, new ConsoleReport { Quiet = true }), "не совпадает с git", "испорченный образ с GitHub");
            Check(File.ReadAllText(Path.Combine(folder, "firmware.ini")) == iniBefore &&
                  File.ReadAllBytes(Path.Combine(folder, @"bl616\vdac4_copro_bl616.bin")).SequenceEqual(blBefore) &&
                  !Directory.Exists(folder + ".download"), "после ошибки прежний комплект цел");
            gh = new FakeGithub(v4) { WrongBlobRepo = Manifest.Bl616Repo };
            Throws(() => Downloader.Download(gh, folder, new ConsoleReport { Quiet = true }), "не совпадает с git", "хэш git в списке другой");
            gh = new FakeGithub(v4) { MissingRepo = Manifest.SlotRepo[1] };
            Throws(() => Downloader.Download(gh, folder, new ConsoleReport { Quiet = true }), "на GitHub нет файла", "образа нет в коммите");

            // списки каталогов не пришли (предел запросов API): SHA-256 ESP по firmware.sha256 и форматы
            gh = new FakeGithub(v4) { ListingDown = true };
            r = new ConsoleReport { Quiet = true };
            res = Downloader.Download(gh, folder, r);
            Check(res.Commit == FakeGithub.Commit && !res.GitChecked && res.Files == 5 && r.Lines.Any(l => l.Contains("хэши git не получены")),
                  "без списков каталогов — скачано, в журнале: хэши git не получены");
            gh = new FakeGithub(v4) { ListingDown = true, CorruptRepo = Manifest.EspRepo };
            Throws(() => Downloader.Download(gh, folder, new ConsoleReport { Quiet = true }), "не совпадает с firmware.sha256",
                   "без хэшей git испорченный образ ESP ловит firmware.sha256");

            // API не ответил вовсе — качается ветка
            gh = new FakeGithub(v4) { ApiDown = true };
            res = Downloader.Download(gh, folder, new ConsoleReport { Quiet = true });
            Check(res.Commit == "" && !res.GitChecked && gh.Urls.Skip(1).All(u => u.Contains("/refs/heads/main/")) &&
                  FirmwareSet.Load(folder).Commit == "refs/heads/main" && FirmwareSet.Load(folder).Problems.Count == 0,
                  "без API — с ветки main, комплект без замечаний");
        }

        static void TestBackupFiles()
        {
            Console.WriteLine("== бэкап на диск");
            string root = Path.Combine(tmp, "prog", "Backup");
            var bk = new Backup(root);
            Check(Directory.Exists(root), "папка Backup создаётся при запуске, если её нет");
            string a = bk.Save("x.bin", new byte[] { 1, 2 }, 0x40000, "проба");
            string c = bk.Save("x.bin", new byte[] { 3 }, 0x40000, "проба");
            string na = Path.GetFileName(a), nc = Path.GetFileName(c);
            Check(Path.GetDirectoryName(a) == root && System.Text.RegularExpressions.Regex.IsMatch(na, @"^\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d_x\.bin$") &&
                  nc == na.Replace("_x.bin", "_x_2.bin") && File.ReadAllBytes(a).SequenceEqual(new byte[] { 1, 2 }),
                  "файлы прямо в Backup, с датой и временем (" + na + "); повтор не затирает прежний (" + nc + ")");
            string txt = File.ReadAllText(Path.Combine(root, "backup.txt"));
            Check(txt.Contains(na) && txt.Contains(nc) && txt.Contains("0x040000") && txt.Contains(FirmwareSet.Sha256(new byte[] { 3 })),
                  "backup.txt: имя, адрес, сумма");
            var bk2 = new Backup(root);
            bk2.Save("y.bin", new byte[] { 4 }, 0, "проба");
            Check(File.ReadAllText(Path.Combine(root, "backup.txt")).Contains(na) && Directory.GetFiles(root, "*.bin").Length == 3,
                  "следующий запуск дописывает, прежние бэкапы на месте");
        }

        // ---- ESP32-S3 -------------------------------------------------------------------------------

        static void TestEsp(FirmwareSet fw)
        {
            Console.WriteLine("== ESP32-S3: бэкап всей flash, стирание всей flash, сжатая запись, MD5");
            var m = new EspRomModel(8 << 20, 0x3C);
            byte[] before = (byte[])m.Flash.Clone();
            var r = new ConsoleReport();
            var rom = new EspRom(m, r);
            var bk = new Backup(Path.Combine(tmp, "bk_esp"));
            var t0 = DateTime.Now;
            rom.Flash(fw.Esp.Data, 0, 8u << 20, bk);
            Console.WriteLine("    модель: " + (DateTime.Now - t0).TotalSeconds.ToString("0.0") + " с");
            string file = bk.Files.Count == 1 ? bk.Files[0] : "";
            Check(file.EndsWith("_esp32s3_flash_8M.bin") && File.ReadAllBytes(file).SequenceEqual(before),
                  "бэкап: вся прежняя flash 8 МБ байт в байт (" + Path.GetFileName(file) + ")");
            Check(m.Reads == (8 << 20) / 64, "чтение по 64 байта: " + m.Reads + " команд");
            bool same = true;
            for (int i = 0; i < fw.Esp.Data.Length && same; i++) same = m.Flash[i] == fw.Esp.Data[i];
            Check(same, "образ лёг во flash байт в байт (" + Util.Size(fw.Esp.Data.Length) + ")");
            bool blank = true;
            for (int i = fw.Esp.Data.Length; i < m.Flash.Length && blank; i++) blank = m.Flash[i] == 0xFF;
            Check(blank, "стёрта вся flash: за образом — 0xFF (NVS, второе приложение, LittleFS)");
            Check(m.Md5s == 2 && m.FlashSizeParam == 8u << 20 && r.LastPercent == 100, "MD5 дважды (бэкап и образ), SPI_SET_PARAMS 8 МБ, ход до 100 %");
            Check(m.WatchdogResets == 0 && (m.WdtConfig0 & 0x80000000u) == 0 && (m.SwdConf & EspRom.SWD_AUTO_FEED_EN) != 0 &&
                  m.WdtProtect == 0 && m.SwdProtect == 0, "сторожа RTC: RWDT выключен, SWD кормится сам, защита снова заперта — чип не перезапускался");
            Check((m.Option1 & 1) != 0, "сброс через USB поставил флаг «загрузчик принудительно» (так у ESP32-S3)");
            rom.HardReset();
            Check(!m.InDownload && (m.Option1 & 1) == 0, "перезапуск — в записанную прошивку, не в загрузчик (флаг снят)");

            // модель сторожа настоящая: без их выключения чтение 1 МБ рвётся, как на плате 10.11.2026
            var mw = new EspRomModel(8 << 20, 0x5A);
            mw.SetDtr(true); mw.SetRts(false); mw.SetRts(true); mw.SetRts(false);
            mw.Attached = true; mw.FlashSizeParam = 8u << 20;
            Throws(() => new EspRom(mw, new ConsoleReport { Quiet = true }).ReadFlash(0, 1u << 20, 0, 40, "чтение"),
                   "не работает", "без выключения сторожей чтение 1 МБ рвётся (IOException)");
            Check(mw.WatchdogResets == 1, "  …перезапустил сторож");
            var md = new EspRomModel(8 << 20, 0x33) { DieAtCommand = 500 };
            Throws(() => new EspRom(md, new ConsoleReport { Quiet = true }).Flash(fw.Esp.Data, 0, 8u << 20, new Backup(Path.Combine(tmp, "bk_esp_usb"))),
                   "пропал с USB", "USB пропал посреди бэкапа — понятная ошибка, flash не тронута");
            Check(md.Erases == 0, "  …и flash не стёрта");

            // ошибка чтения или неверный бэкап — flash не тронута
            var m2 = new EspRomModel(8 << 20, 0x11) { CorruptReadAt = 0x123440 };
            byte[] keep = (byte[])m2.Flash.Clone();
            Throws(() => new EspRom(m2, new ConsoleReport { Quiet = true }).Flash(fw.Esp.Data, 0, 8u << 20, new Backup(Path.Combine(tmp, "bk_esp2"))),
                   "бэкап прочитан с ошибкой", "испорченное чтение — MD5 бэкапа не сходится");
            Check(m2.Erases == 0 && m2.Flash.SequenceEqual(keep), "  …и flash не стёрта");
            var m3 = new EspRomModel(8 << 20, 0x22) { RefuseReadAt = 0x40 };
            Throws(() => new EspRom(m3, new ConsoleReport { Quiet = true }).Flash(fw.Esp.Data, 0, 8u << 20, new Backup(Path.Combine(tmp, "bk_esp3"))),
                   "команда 0x0E", "отказ ПЗУ на чтении");
            Check(m3.Erases == 0, "  …и flash не стёрта");

            var m4 = new EspRomModel(8 << 20, 0);
            m4.CorruptChecksumOnce = true;
            Throws(() => new EspRom(m4, new ConsoleReport { Quiet = true }).Flash(fw.Esp.Data, 0, 8u << 20, null), "0xC1", "отказ ПЗУ (ошибка суммы) — ошибка шага");

            var small = new byte[0x10000];
            small[0] = 0xE9;
            var m5 = new EspRomModel(4 << 20, 0);
            Throws(() => new EspRom(m5, new ConsoleReport()).Flash(small, 0x3F8000, 4u << 20, null), "не помещается", "образ за краем flash");
        }

        // ---- BL616 ----------------------------------------------------------------------------------

        static void TestBl616(FirmwareSet fw)
        {
            Console.WriteLine("== BL616: ISP ПЗУ по USB CDC, бэкап и стирание раздела 0x40000…0x7FFFF");
            var m = new Bl616RomModel();
            byte[] partner = m.Flash.Take(0x40000).ToArray();
            byte[] oldPart = m.Flash.Skip(0x40000).Take(0x40000).ToArray();
            byte[] cache = m.Flash.Skip(0x80000).Take(0x10000).ToArray();
            var r = new ConsoleReport();
            var bk = new Backup(Path.Combine(tmp, "bk_bl"));
            new Bl616Isp(m, r).Flash(fw.Bl616.Data, fw.Bl616.Address, FirmwareSet.Bl616PartEnd, bk);
            string file = bk.Files.Count == 1 ? bk.Files[0] : "";
            Check(file.EndsWith("_bl616_0x40000_256K.bin") && File.ReadAllBytes(file).SequenceEqual(oldPart),
                  "бэкап: прежний раздел 256 КБ байт в байт (" + m.Reads + " чтений по 2048)");
            Check(m.Flash.Take(0x40000).SequenceEqual(partner), "FPGA Partner Sipeed (0x0…0x3FFFF) не тронут");
            Check(m.Flash.Skip(0x40000).Take(fw.Bl616.Data.Length).SequenceEqual(fw.Bl616.Data), "образ лёг с 0x40000 (" + Util.Size(fw.Bl616.Data.Length) + ")");
            Check(m.Flash.Skip(0x40000 + fw.Bl616.Data.Length).Take(0x80000 - 0x40000 - fw.Bl616.Data.Length).All(x => x == 0xFF),
                  "раздел стёрт целиком: за образом до 0x7FFFF — 0xFF");
            Check(m.EraseFrom == 0x40000 && m.EraseTo == 0x7FFFF, "стирание 0x40000…0x7FFFF");
            Check(m.Flash.Skip(0x80000).Take(0x10000).SequenceEqual(cache), "кэш ядер с 0x80000 не тронут");
            Check(m.FlashSet4 == 0x01014102 && m.FlashSet88 == 0x01014102, "параметры flash 0x01014102 (вывод 2 из сведений о загрузке)");
            Check(m.IspTimeout == 2000 && m.CheckSent && m.Erases == 1 && m.Writes == (fw.Bl616.Data.Length + 2047) / 2048,
                  "срок ISP 2 с перед записью, 0x3A после, одно стирание, куски по 2048");
            Check(r.LastPercent == 100, "ход до 100 %");
            // повторный запуск: ПЗУ уже в ISP — отвечает на 0x10 без синхронизации
            int syncs = m.Syncs55;
            m.PdLeft = 0;
            new Bl616Isp(m, new ConsoleReport { Quiet = true }).Flash(fw.Bl616.Data, fw.Bl616.Address, FirmwareSet.Bl616PartEnd, bk);
            Check(m.Syncs55 == syncs, "повтор без новой синхронизации 0x55");
            Check(bk.Files.Count == 2 && bk.Files[1].EndsWith("_bl616_0x40000_256K_2.bin"), "второй бэкап — отдельным файлом");
        }

        // ---- ПЛИС ------------------------------------------------------------------------------------

        static void TestGowin(FirmwareSet fw)
        {
            Console.WriteLine("== ПЛИС: MPSSE → JTAG GW2AR-18 → SPI flash, бэкап и стирание слотов");
            var m = new GowinTapModel();
            m.Flash.Sr = 0x1C;                                  // защита BP: должна сняться
            byte[] old = (byte[])m.Flash.Mem.Clone();
            var r = new ConsoleReport();
            var g = new Gowin(new Jtag(m), r);
            var bk = new Backup(Path.Combine(tmp, "bk_tang"));
            var t0 = DateTime.Now;
            g.Program(fw.Tang, bk);
            Console.WriteLine("    модель: " + (DateTime.Now - t0).TotalSeconds.ToString("0.0") + " с");
            foreach (int s in new[] { 0, 1, 3 }) {
                string file = bk.Files.FirstOrDefault(f => f.EndsWith("_tang_slot" + s + ".bin")) ?? "";
                Check(file.Length > 0 && File.ReadAllBytes(file).SequenceEqual(old.Skip(s << 20).Take(1 << 20)), "бэкап слота " + s + " (1 МБ) байт в байт");
            }
            Check(bk.Files.Count == 3, "слот 2 (шрифты) без бэкапа — его не пишем");
            Check(g.FlashId == 0x0B4017, "JEDEC flash 0B4017 через мост 0x16 (MISO на бит позже)");
            int[] slotOf = { 0, 1, 3 };
            for (int i = 0; i < 3; i++) {
                int s = slotOf[i], len = fw.Tang[i].Data.Length;
                Check(m.Flash.Mem.Skip(s << 20).Take(len).SequenceEqual(fw.Tang[i].Data), "слот " + s + " лёг с 0x" + (s << 20).ToString("X6"));
                Check(m.Flash.Mem.Skip((s << 20) + len).Take((1 << 20) - len).All(x => x == 0xFF), "  …за образом до конца слота — стёрто");
            }
            Check(m.Flash.Mem.Skip(0x200000).Take(0x100000).SequenceEqual(old.Skip(0x200000).Take(0x100000)), "слот 2 не тронут");
            Check(m.Flash.Mem.Skip(0x400000).SequenceEqual(old.Skip(0x400000)), "с 0x400000 до конца (слоты 4–6, настройки Tang) не тронуто");
            Check((m.Flash.Sr & 0x7C) == 0, "защита flash снята");
            Check(m.Reloads == 1 && (m.Status & 1u << 13) != 0, "RELOAD — ПЛИС загрузилась (Done)");
            Check(m.Divisor == 4, "после записи JTAG снова 6 МГц (делитель 4)");
            Check(r.LastPercent == 100, "ход до 100 %");

            // только шрифты: бэкап и стирание слота 2 целиком, прочее не тронуто
            var mf = new GowinTapModel();
            Array.Copy(fw.Tang[0].Data, mf.Flash.Mem, fw.Tang[0].Data.Length);     // в слоте 0 стоит TS-Config: RELOAD с него
            byte[] oldF = (byte[])mf.Flash.Mem.Clone();
            var bkf = new Backup(Path.Combine(tmp, "bk_font"));
            var fontParts = FirmwareSet.FontSpans(MakeFont(0x200040), "romfonts.bin");
            new Gowin(new Jtag(mf), new ConsoleReport { Quiet = true }).Program(fontParts, bkf);
            Check(bkf.Files.Count == 1 && bkf.Files[0].EndsWith("_tang_slot2.bin") &&
                  File.ReadAllBytes(bkf.Files[0]).SequenceEqual(oldF.Skip(0x200000).Take(0x100000)), "шрифты: бэкап слота 2");
            Check(mf.Flash.Mem.Skip(0x200040).Take(64).SequenceEqual(fontParts[0].Data) &&
                  mf.Flash.Mem.Skip(0x200000).Take(0x40).All(x => x == 0xFF) &&
                  mf.Flash.Mem.Skip(0x200080).Take(0x100000 - 0x80).All(x => x == 0xFF), "шрифты: слот 2 стёрт целиком, кусок лёг с 0x200040");
            Check(mf.Flash.Mem.Take(0x200000).SequenceEqual(oldF.Take(0x200000)) &&
                  mf.Flash.Mem.Skip(0x300000).SequenceEqual(oldF.Skip(0x300000)), "шрифты: слоты 0, 1, 3 и дальше не тронуты");

            var m2 = new GowinTapModel();
            m2.Flash.Jedec = 0xFFFFFF;
            Throws(() => new Gowin(new Jtag(m2), new ConsoleReport { Quiet = true }).Program(fw.Tang, null), "не отвечает", "flash без ответа — ошибка шага");
        }
    }
}
