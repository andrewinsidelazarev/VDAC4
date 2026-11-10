// SPDX-License-Identifier: Apache-2.0
// Первая прошивка VDAC4: общие типы — отчёт о ходе, ошибки, мелкие помощники.
using System;
using System.Text;

namespace Vdac4Flasher
{
    /// <summary>Куда шаг прошивки пишет журнал и ход (окно программы или проверка на ПК).</summary>
    public interface IReport
    {
        void Log(string text);
        /// <summary>Ход шага: 0…100 и короткая подпись этапа.</summary>
        void Progress(int percent, string stage);
        /// <summary>Пользователь нажал «Стоп»: шаг должен закончиться при первой возможности.</summary>
        bool Cancelled { get; }
    }

    /// <summary>Отказ шага прошивки: текст — для человека, на языке окна.</summary>
    public class FlashException : Exception
    {
        public FlashException(string message) : base(message) { }
        public FlashException(string message, Exception inner) : base(message, inner) { }
    }

    public static class Util
    {
        public static string Hex(byte[] data, int offset = 0, int count = -1)
        {
            if (count < 0) count = data.Length - offset;
            var sb = new StringBuilder(count * 2);
            for (int i = 0; i < count; i++) sb.Append(data[offset + i].ToString("x2"));
            return sb.ToString();
        }

        public static void Put32(byte[] b, int at, uint v)
        {
            b[at] = (byte)v; b[at + 1] = (byte)(v >> 8); b[at + 2] = (byte)(v >> 16); b[at + 3] = (byte)(v >> 24);
        }

        public static uint Get32(byte[] b, int at)
        {
            return (uint)(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24);
        }

        public static ushort Get16(byte[] b, int at) { return (ushort)(b[at] | b[at + 1] << 8); }

        public static uint ParseNumber(string text)
        {
            text = text.Trim();
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return Convert.ToUInt32(text.Substring(2), 16);
            return uint.Parse(text);
        }

        /// <summary>Байт задом наперёд: JTAG сдвигает младшим битом вперёд, SPI — старшим.</summary>
        public static byte Reverse(byte b)
        {
            b = (byte)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
            b = (byte)((b & 0xCC) >> 2 | (b & 0x33) << 2);
            b = (byte)((b & 0xAA) >> 1 | (b & 0x55) << 1);
            return b;
        }

        public static string Size(long bytes)
        {
            if (bytes >= 1024 * 1024) return (bytes / 1048576.0).ToString("0.0") + Lang.T(" МБ", " MB");
            if (bytes >= 1024) return (bytes / 1024.0).ToString("0.0") + Lang.T(" КБ", " KB");
            return bytes + Lang.T(" байт", " B");
        }
    }
}
