// SPDX-License-Identifier: Apache-2.0
// Прошивки — с GitHub перед прошивкой: образы ESP, BL616 и слотов ПЛИС из репозитория VDAC4 (ветка main) по путям,
// которые знает программа (Manifest). Всё качается с одного коммита: номер последнего коммита ветки даёт API
// GitHub, файлы идут с raw.githubusercontent.com по этому номеру — образы и версии не разойдутся, даже если кто-то
// запушит посреди загрузки.
// Целость: у каждого образа — хэш git того же коммита (API GitHub, список файлов каталога: «sha» — SHA-1 от
// «blob <длина>\0» + байты); скачанные байты дают тот же хэш, или файл не берётся. Образ ESP ещё и сверяется с
// SHA-256 из firmware.sha256 рядом с ним (его пишет сборка прошивки). Не ответил API — берётся ветка как есть:
// тогда сверяются SHA-256 образа ESP и форматы всех образов, о чём пишется в журнал.
// До 10.11.2026 программа брала с GitHub опись Flasher/firmware.ini с суммами; её надо было обновлять при каждом
// пуше образов, она отставала — и загрузка падала («GitHub ответил 404»). Теперь описи на GitHub нет.
// Файлы сначала ложатся в папку firmware.download, и только когда все пришли и сошлись — переносятся в папку
// firmware вместе с описью firmware.ini (Manifest.Make). Ошибка загрузки прежний комплект не портит. Свои файлы
// (шрифты romfonts.bin) остаются.
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Vdac4Flasher
{
    /// <summary>Получить файл по адресу (HTTPS; в проверках на ПК — подмена).</summary>
    public interface IFetch
    {
        /// <summary>Тело ответа. progress(получено, всего или −1); cancelled — пользователь нажал «Стоп».</summary>
        byte[] Get(string url, string accept, Action<long, long> progress, Func<bool> cancelled);
    }

    public class HttpFetch : IFetch
    {
        static HttpFetch()
        {
            // .NET 4.8 берёт протоколы TLS из Windows; TLS 1.2 (его требует GitHub) — явно на всякий случай
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        }

        public byte[] Get(string url, string accept, Action<long, long> progress, Func<bool> cancelled)
        {
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.UserAgent = "VDAC4-Flasher";
            if (accept != null) req.Accept = accept;
            req.Timeout = 30000;
            req.ReadWriteTimeout = 30000;
            req.AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate;
            try {
                using (var resp = (HttpWebResponse)req.GetResponse())
                using (var s = resp.GetResponseStream())
                using (var o = new MemoryStream()) {
                    long total = resp.ContentLength;
                    var buf = new byte[65536];
                    int n;
                    while ((n = s.Read(buf, 0, buf.Length)) > 0) {
                        if (cancelled != null && cancelled()) throw new OperationCanceledException();
                        o.Write(buf, 0, n);
                        if (progress != null) progress(o.Length, total);
                    }
                    return o.ToArray();
                }
            } catch (WebException e) {
                var r = e.Response as HttpWebResponse;
                if (r != null)
                    throw new FlashException(string.Format(Lang.T("GitHub ответил {0} на {1}", "GitHub answered {0} for {1}"),
                                                           (int)r.StatusCode, url), e);
                throw new FlashException(Lang.T("нет связи с GitHub: ", "no connection to GitHub: ") + e.Message, e);
            }
        }
    }

    public static class Github
    {
        public const string Repo = "andrewinsidelazarev/VDAC4", Branch = "main";

        /// <summary>Путь репозитория для адреса: каждая часть — с %-заменами (пробелы — %20).</summary>
        static string Escape(string path)
        {
            var parts = path.Split('/');
            for (int i = 0; i < parts.Length; i++) parts[i] = Uri.EscapeDataString(parts[i]);
            return string.Join("/", parts);
        }

        /// <summary>Адрес файла репозитория на raw.githubusercontent.com.</summary>
        public static string RawUrl(string reference, string path)
        {
            return "https://raw.githubusercontent.com/" + Repo + "/" + reference + "/" + Escape(path);
        }

        public static string CommitUrl { get { return "https://api.github.com/repos/" + Repo + "/commits/" + Branch; } }

        /// <summary>Список файлов каталога репозитория на коммите (API GitHub, с хэшами git).</summary>
        public static string ContentsUrl(string commit, string folder)
        {
            return "https://api.github.com/repos/" + Repo + "/contents/" + Escape(folder) + "?ref=" + commit;
        }

        /// <summary>Хэш git файла: SHA-1 от «blob длина\0» и самих байтов — так git хранит и называет файлы.</summary>
        public static string GitBlobSha1(byte[] data)
        {
            byte[] head = Encoding.ASCII.GetBytes("blob " + data.Length + "\0");
            using (var h = SHA1.Create()) {
                h.TransformBlock(head, 0, head.Length, null, 0);
                h.TransformFinalBlock(data, 0, data.Length);
                return Util.Hex(h.Hash);
            }
        }

        /// <summary>Ответ API на список каталога (JSON-массив объектов) → путь файла → хэш git. Объекты верхнего
        /// уровня выделяются по скобкам (вложенный «_links» внутри), в объекте берутся «path» и «sha».</summary>
        public static Dictionary<string, string> ParseListing(string json)
        {
            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            int depth = 0, start = -1;
            bool str = false, esc = false;
            for (int i = 0; i < json.Length; i++) {
                char c = json[i];
                if (str) {
                    if (esc) esc = false;
                    else if (c == '\\') esc = true;
                    else if (c == '"') str = false;
                    continue;
                }
                if (c == '"') str = true;
                else if (c == '{') { if (depth++ == 0) start = i; }
                else if (c == '}' && depth > 0 && --depth == 0) {
                    string obj = json.Substring(start, i - start + 1);
                    // ключи верхнего уровня объекта: вложенные объекты из текста убираются
                    string top = Regex.Replace(obj.Substring(1, obj.Length - 2), @"\{[^{}]*\}", "");
                    var p = Regex.Match(top, @"""path""\s*:\s*""((?:[^""\\]|\\.)*)""");
                    var s = Regex.Match(top, @"""sha""\s*:\s*""([0-9a-fA-F]{40})""");
                    if (p.Success && s.Success) map[Unescape(p.Groups[1].Value)] = s.Groups[1].Value.ToLowerInvariant();
                }
            }
            return map;
        }

        static string Unescape(string s)
        {
            if (s.IndexOf('\\') < 0) return s;
            var sb = new StringBuilder();
            for (int i = 0; i < s.Length; i++) {
                if (s[i] != '\\' || i + 1 >= s.Length) { sb.Append(s[i]); continue; }
                char c = s[++i];
                if (c == 'u' && i + 4 < s.Length) { sb.Append((char)Convert.ToInt32(s.Substring(i + 1, 4), 16)); i += 4; }
                else if (c == 'n') sb.Append('\n');
                else if (c == 't') sb.Append('\t');
                else sb.Append(c);           // \" \\ \/
            }
            return sb.ToString();
        }
    }

    public class DownloadResult
    {
        public string Commit = "";      // 40 знаков; пусто — качалось с ветки без номера коммита
        public bool GitChecked;         // каждый образ сверен с хэшем git (без API — только SHA-256 ESP и форматы)
        public int Files;
        public long Bytes;
    }

    public static class Downloader
    {
        /// <summary>Скачать комплект в папку firmware (ход — 0…100 %). Ошибка — FlashException, прежний комплект цел.</summary>
        public static DownloadResult Download(IFetch fetch, string folder, IReport report)
        {
            var result = new DownloadResult();
            Func<bool> cancelled = () => report.Cancelled;
            report.Progress(0, Lang.T("Связь с GitHub", "Contacting GitHub"));

            // 1. номер последнего коммита ветки (одна строка, 40 знаков)
            string reference = "refs/heads/" + Github.Branch;
            try {
                string sha = Encoding.ASCII.GetString(fetch.Get(Github.CommitUrl, "application/vnd.github.sha", null, cancelled)).Trim();
                if (sha.Length == 40 && IsHex(sha)) { reference = sha; result.Commit = sha; }
            } catch (FlashException e) {
                report.Log(Lang.T("номер коммита не получен (", "no commit number (") + e.Message +
                           Lang.T("), качаю с ветки ", "), downloading from the branch ") + Github.Branch);
            }
            report.Log("GitHub: " + Github.Repo + ", " + (result.Commit.Length > 0 ? Lang.T("коммит ", "commit ") + result.Commit
                                                                               : Lang.T("ветка ", "branch ") + Github.Branch));
            var entries = Manifest.Entries();

            // 2. хэши git образов — списки их каталогов на том же коммите
            Dictionary<string, string> blobs = null;
            if (result.Commit.Length > 0) {
                try {
                    blobs = new Dictionary<string, string>(StringComparer.Ordinal);
                    var folders = new List<string>();
                    foreach (var e in entries) {
                        string dir = e.Repo.Substring(0, e.Repo.LastIndexOf('/'));
                        if (!folders.Contains(dir)) folders.Add(dir);
                    }
                    foreach (string dir in folders) {
                        byte[] json = fetch.Get(Github.ContentsUrl(result.Commit, dir), "application/vnd.github+json", null, cancelled);
                        foreach (var kv in Github.ParseListing(Encoding.UTF8.GetString(json))) blobs[kv.Key] = kv.Value;
                    }
                } catch (FlashException e) {
                    blobs = null;
                    report.Log(Lang.T("хэши git не получены (", "no git hashes (") + e.Message +
                               Lang.T("): сверяю SHA-256 образа ESP и форматы", "): checking the ESP SHA-256 and the formats"));
                }
                if (blobs != null)
                    foreach (var e in entries)
                        if (!blobs.ContainsKey(e.Repo))
                            throw new FlashException(Lang.T("на GitHub нет файла ", "GitHub has no file ") + e.Repo);
            }
            result.GitChecked = blobs != null;
            report.Progress(2, Lang.T("Версии прошивок", "Firmware versions"));

            // 3. версии и SHA-256 образа ESP — из файлов репозитория того же коммита
            Func<string, string> text = repo => Encoding.UTF8.GetString(fetch.Get(Github.RawUrl(reference, repo), null, null, cancelled));
            var versions = Manifest.ReadVersions(text(Manifest.EspSumsRepo), text(Manifest.Bl616MainRepo), text(Manifest.TangReadmeRepo));
            if (versions.EspFactorySha256.Length != 64)
                throw new FlashException(Lang.T("в firmware.sha256 на GitHub нет суммы firmware.factory.bin",
                                                "firmware.sha256 on GitHub has no firmware.factory.bin sum"));
            report.Log(string.Format(Lang.T("версии: ESP {0}, BL616 {1}, ПЛИС {2}", "versions: ESP {0}, BL616 {1}, FPGA {2}"),
                                     versions.Esp, versions.Bl616, versions.Tang));

            // 4. образы — во временную папку, каждый со сверкой
            string stage = folder.TrimEnd('\\', '/') + ".download";
            if (Directory.Exists(stage)) Directory.Delete(stage, true);
            Directory.CreateDirectory(stage);
            try {
                for (int i = 0; i < entries.Count; i++) {
                    var e = entries[i];
                    int from = 4 + i * 94 / entries.Count, span = 94 / entries.Count;
                    string name = Path.GetFileName(e.Local);
                    report.Progress(from, Lang.T("Загрузка ", "Downloading ") + name);
                    int lastPct = from;
                    byte[] data = fetch.Get(Github.RawUrl(reference, e.Repo), null, (got, total) => {
                        if (total <= 0) return;
                        int pct = from + (int)Math.Min(span, got * span / total);
                        if (pct > lastPct) { lastPct = pct; report.Progress(pct, Lang.T("Загрузка ", "Downloading ") + name); }
                    }, cancelled);
                    if (blobs != null) {
                        string git = Github.GitBlobSha1(data);
                        if (git != blobs[e.Repo])
                            throw new FlashException(string.Format(Lang.T(
                                "{0} с GitHub не совпадает с git (хэш {1}, в коммите {2}) — файл пришёл испорченным, повторите загрузку.",
                                "{0} from GitHub does not match git (hash {1}, commit has {2}) — the file arrived damaged, download again."),
                                name, git.Substring(0, 12), blobs[e.Repo].Substring(0, 12)));
                    }
                    try {
                        e.Sha256 = FirmwareSet.ContentSha256(e.Local, data);
                        byte[] content = e.Local.EndsWith(".fs", StringComparison.OrdinalIgnoreCase) ? FirmwareSet.ParseFs(data, name) : data;
                        if (i == 0) FirmwareSet.CheckEsp(content);
                        else if (i == 1) FirmwareSet.CheckBl616(content);
                        else FirmwareSet.CheckGowin(content);
                    } catch (FlashException ex) {
                        throw new FlashException(Lang.T("файл с GitHub испорчен: ", "the file from GitHub is damaged: ") + ex.Message);
                    }
                    if (i == 0 && !e.Sha256.Equals(versions.EspFactorySha256, StringComparison.OrdinalIgnoreCase))
                        throw new FlashException(string.Format(Lang.T(
                            "{0} с GitHub не совпадает с firmware.sha256 (SHA-256 {1}, там {2}) — повторите загрузку позже.",
                            "{0} from GitHub does not match firmware.sha256 (SHA-256 {1}, listed {2}) — retry later."),
                            name, e.Sha256.Substring(0, 16), versions.EspFactorySha256.Substring(0, 16)));
                    string path = Path.Combine(stage, e.Local);
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    File.WriteAllBytes(path, data);
                    result.Files++;
                    result.Bytes += data.Length;
                    report.Log(string.Format(blobs != null ? Lang.T("{0}: {1}, хэш git сходится", "{0}: {1}, git hash matches")
                                                           : Lang.T("{0}: {1}", "{0}: {1}"), e.Repo, Util.Size(data.Length)));
                }
                // опись — с отметкой, откуда и когда скачано
                string ini = Manifest.Make(entries, versions, result.Commit.Length > 0 ? result.Commit : "refs/heads/" + Github.Branch,
                                           DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
                File.WriteAllText(Path.Combine(stage, "firmware.ini"), ini, new UTF8Encoding(false));

                // 5. всё пришло — в папку firmware (поверх прежних файлов комплекта; свои файлы остаются)
                foreach (var e in entries) {
                    string dst = Path.Combine(folder, e.Local);
                    Directory.CreateDirectory(Path.GetDirectoryName(dst));
                    File.Copy(Path.Combine(stage, e.Local), dst, true);
                }
                File.Copy(Path.Combine(stage, "firmware.ini"), Path.Combine(folder, "firmware.ini"), true);
            } finally {
                try { Directory.Delete(stage, true); } catch (Exception) { }
            }
            report.Progress(100, Lang.T("Скачано", "Downloaded"));
            return result;
        }

        static bool IsHex(string s)
        {
            foreach (char c in s)
                if (!(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F')) return false;
            return true;
        }
    }
}
