// SPDX-License-Identifier: Apache-2.0
// Бэкап прежней прошивки: всё, что шаг сотрёт, программа сначала читает с устройства и кладёт в папку Backup
// рядом с программой (папку создаёт при запуске, если её нет). Имя файла — с датой и временем запуска
// программы: 2026-11-07_20-15-03_esp32s3_flash_8M.bin. Каждый файл после записи читается с диска и сверяется
// по SHA-256; опись — Backup\backup.txt (файл, что, с какого адреса, сколько байтов, сумма), строки
// дописываются. Шаг стирает устройство только после того, как бэкап лёг на диск. Повтор шага не затирает
// прежний бэкап: новый файл получает номер (_2, _3…), первый — прошивка, что стояла до программы.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Vdac4Flasher
{
    public class Backup
    {
        public readonly string Folder;
        readonly string stamp = DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss");
        /// <summary>Файлы, сохранённые за этот запуск (полные пути).</summary>
        public readonly List<string> Files = new List<string>();

        public Backup(string folder)
        {
            Folder = folder;
            try { Directory.CreateDirectory(Folder); } catch (Exception) { }    // не вышло — скажет Save, до стирания
        }

        /// <summary>Сохранить прочитанное с устройства; вернуть путь файла. Не вышло — FlashException (стирать нельзя).</summary>
        public string Save(string name, byte[] data, uint address, string what)
        {
            try {
                Directory.CreateDirectory(Folder);
                string baseName = stamp + "_" + Path.GetFileNameWithoutExtension(name), ext = Path.GetExtension(name);
                string path = Path.Combine(Folder, baseName + ext);
                for (int k = 2; File.Exists(path); k++) path = Path.Combine(Folder, baseName + "_" + k + ext);
                File.WriteAllBytes(path, data);
                string sha = FirmwareSet.Sha256(data);
                if (FirmwareSet.Sha256(File.ReadAllBytes(path)) != sha)
                    throw new IOException(Lang.T("файл на диске не совпал с прочитанным", "the file on disk differs from what was read"));
                File.AppendAllText(Path.Combine(Folder, "backup.txt"),
                                   string.Format("{0}  {1}  0x{2:X6}  {3} B  SHA-256 {4}\r\n", Path.GetFileName(path), what, address, data.Length, sha),
                                   new UTF8Encoding(false));
                Files.Add(path);
                return path;
            } catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) {
                throw new FlashException(Lang.T("бэкап не сохранён в ", "backup not saved into ") + Folder +
                                         Lang.T(", устройство не тронуто: ", ", the device is untouched: ") + e.Message, e);
            }
        }
    }
}
