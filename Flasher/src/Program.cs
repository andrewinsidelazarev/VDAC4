// SPDX-License-Identifier: Apache-2.0
// Первая прошивка VDAC4 — мастер для Windows: ESP32-S3 Zero, BL616 и ПЛИС модуля Tang Nano 20K.
// Прошивки — в папке firmware рядом с программой (опись firmware.ini пишет сама программа, скачав их с GitHub). Ни Python, ни ESP-IDF, ни Gowin
// не нужны: протоколы загрузчиков ESP32-S3 и BL616 и JTAG ПЛИС — в самой программе.
using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace Vdac4Flasher
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
            string folder = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "firmware");
            if (args.Length >= 2 && args[0] == "--snapshot") {
                Snapshot(args[1], folder);
                return;
            }
            Application.Run(new Wizard(folder));
        }

        /// <summary>Картинки всех страниц на обоих языках (для описания), окно — за пределами экрана.</summary>
        static void Snapshot(string outDir, string folder)
        {
            Directory.CreateDirectory(outDir);
            foreach (bool ru in new[] { true, false }) {
                Lang.Russian = ru;
                // страницы 0…4 как есть; шаги 1…3 ещё и «тесно»: длинная ошибка и открытый журнал
                for (int k = 0; k <= 7; k++) {
                    int p = k <= 4 ? k : k - 4;
                    bool crowded = k > 4;
                    using (var w = new Wizard(folder, true)) {
                        w.StartPosition = FormStartPosition.Manual;
                        w.Location = new Point(-20000, -20000);
                        w.ShowInTaskbar = false;
                        w.Show();
                        w.ShowSnapshot(p, crowded);
                        Application.DoEvents();
                        using (var bmp = new Bitmap(w.Width, w.Height)) {
                            w.DrawToBitmap(bmp, new Rectangle(0, 0, w.Width, w.Height));
                            bmp.Save(Path.Combine(outDir, (ru ? "ru" : "en") + "_" + p + (crowded ? "_crowded" : "") + ".png"));
                        }
                        w.Close();
                    }
                }
            }
        }
    }
}
