// SPDX-License-Identifier: Apache-2.0
// Запоминание адреса платы между запусками: файл VDAC4Console.ini рядом с программой.
// Формат — строки «ключ=значение». Не удалось прочитать или записать (папка только для чтения) —
// не беда: программа просто не вспомнит адрес в следующий раз.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Vdac4Console
{
    public sealed class Settings
    {
        public string Address = "";        // адрес платы, например 192.168.1.110
        public string Local = "";          // адрес ПК, с которого плата ответила при поиске
        public string Language = "";       // "ru", "en" или пусто — по языку Windows

        readonly string path;

        public Settings(string path) { this.path = path; }

        public static string DefaultPath()
        {
            return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "VDAC4Console.ini");
        }

        public void Load()
        {
            try {
                if (!File.Exists(path)) return;
                var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (string line in File.ReadAllLines(path, Encoding.UTF8)) {
                    int eq = line.IndexOf('=');
                    if (eq > 0) map[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
                }
                string v;
                if (map.TryGetValue("address", out v)) Address = v;
                if (map.TryGetValue("local", out v)) Local = v;
                if (map.TryGetValue("language", out v)) Language = v;
            } catch (Exception) { }
        }

        public void Save()
        {
            try {
                File.WriteAllText(path, "address=" + Address + "\r\nlocal=" + Local + "\r\nlanguage=" + Language + "\r\n",
                                  new UTF8Encoding(false));
            } catch (Exception) { }
        }
    }
}
