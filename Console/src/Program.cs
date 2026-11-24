// SPDX-License-Identifier: Apache-2.0
// Консоль VDAC4 — окно для Windows: журнал ESP платы VDAC4 по сети, без Python и без USB.
// Подключается к сетевой консоли ESP (порт 23) и показывает её вывод вживую; плату находит сама.
// Нужна только Windows 10/11 (.NET Framework 4.8 там уже есть).
using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace Vdac4Console
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.ThreadException += (s, e) =>
                MessageBox.Show(e.Exception.ToString(), "VDAC4", MessageBoxButtons.OK, MessageBoxIcon.Error);
            if (args.Length >= 2 && args[0] == "--snapshot") {
                Snapshot(args[1]);
                return;
            }
            var settings = new Settings(Settings.DefaultPath());
            settings.Load();
            if (settings.Language == "ru") Lang.Russian = true;
            if (settings.Language == "en") Lang.Russian = false;
            Application.Run(new MainForm(settings));
        }

        /// <summary>Картинки окна на обоих языках (для описания); окно — за пределами экрана, без сети.</summary>
        static void Snapshot(string outDir)
        {
            Directory.CreateDirectory(outDir);
            string sample =
                "VDAC4 console, vdac4-0.1.167. Commands: help, mem, ver, log, tang\r\n" +
                "I (812) zifi: VDAC4 ESP vdac4-0.1.167\r\n" +
                "I (2210) zifi-console: listening on port 23\r\n" +
                "I (2305) tang: LINK up, core 01010146 (TS-Config 1.46), 30000 kHz\r\n" +
                "I (2410) ft_rom: font image 285992 bytes, 2 spans -> PSRAM\r\n" +
                "I (31950) cores: core NES: BL616 cache hit, LOAD seq 12\r\n" +
                "I (33020) cores: core NES: VERSION 04000010, started\r\n" +
                "\r\n> ver\r\n" +
                "vdac4-0.1.167, up 241 s, 31 tasks; chip 52.5 C, RSSI -48 dBm, wifi drops 0 (last reason 0)\r\n";
            foreach (bool ru in new[] { true, false }) {
                Lang.Russian = ru;
                for (int k = 0; k < 2; k++) {
                    using (var w = new MainForm(new Settings(Path.Combine(outDir, "unused.ini")), true)) {
                        w.StartPosition = FormStartPosition.Manual;
                        w.Location = new Point(-20000, -20000);
                        w.ShowInTaskbar = false;
                        w.Show();
                        if (k == 0) w.ShowSample(sample, Lang.T("Подключено к 192.168.1.110:23", "Connected to 192.168.1.110:23"), 0);
                        else w.ShowSample("", Lang.T(
                            "Подключено, но консоль молчит: скорее всего, её держит другая программа (консоль принимает одного клиента). Как только та отключится, вывод пойдёт сюда.",
                            "Connected, but the console is silent: most likely another program holds it (the console takes one client). Output appears here as soon as it disconnects."), 1);
                        Application.DoEvents();
                        using (var bmp = new Bitmap(w.Width, w.Height)) {
                            w.DrawToBitmap(bmp, new Rectangle(0, 0, w.Width, w.Height));
                            bmp.Save(Path.Combine(outDir, (ru ? "ru" : "en") + "_" + k + ".png"));
                        }
                        w.Close();
                    }
                }
            }
        }
    }
}
