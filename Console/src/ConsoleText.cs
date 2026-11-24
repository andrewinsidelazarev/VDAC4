// SPDX-License-Identifier: Apache-2.0
// Байты консоли ESP → текст для окна Windows.
//
// Что здесь делается и зачем:
//  1. Байты — это UTF-8. Сеть режет поток на куски где попало, и русская буква (2 байта) может
//     разорваться между двумя кусками. Поэтому декодер «с памятью» (Decoder): недостающий хвост буквы
//     он держит у себя до следующего куска, а не превращает в «�».
//  2. ESP заканчивает строки одним «\n» (как Linux), а текстовое поле Windows понимает только «\r\n».
//     Любой конец строки («\n», «\r» или «\r\n») превращаем в «\r\n».
//  3. Цветные коды терминала (ESC [ … m) окну не нужны — их вырезаем. В прошивке VDAC4 цвета журнала
//     выключены (CONFIG_LOG_COLORS), но другая сборка может их включить.
//  4. Прочие управляющие байты (кроме табуляции) выбрасываем: в окне они видны как квадратики.
using System.Text;

namespace Vdac4Console
{
    public sealed class ConsoleText
    {
        // false, false: без метки BOM в начале и без исключений на битых байтах (вместо них «�»)
        readonly Decoder utf8 = new UTF8Encoding(false, false).GetDecoder();
        bool lastWasCr;      // прошлый знак был «\r»: следующий «\n» — тот же конец строки, второй не нужен
        int escape;          // 0 — обычный текст, 1 — пришёл ESC, 2 — внутри «ESC [ … буква»

        /// <summary>Следующий кусок байтов из сети → готовый текст (может быть пустым).</summary>
        public string Feed(byte[] data, int offset, int count)
        {
            // Один байт UTF-8 даёт не больше одного знака; плюс до 3 байтов, отложенных с прошлого куска.
            var chars = new char[count + 4];
            int n = utf8.GetChars(data, offset, count, chars, 0, false);
            var sb = new StringBuilder(n + n / 16 + 2);
            for (int i = 0; i < n; i++) {
                char c = chars[i];
                if (escape == 1) {                       // после ESC: «[» начинает код цвета, иное — короткий код
                    escape = c == '[' ? 2 : 0;
                    continue;
                }
                if (escape == 2) {                       // код идёт до буквы (знаки 0x40…0x7E), её тоже выбросить
                    if (c >= '@' && c <= '~') escape = 0;
                    continue;
                }
                if (c == '\x1b') { escape = 1; continue; }
                if (c == '\r') { sb.Append("\r\n"); lastWasCr = true; continue; }
                if (c == '\n') {
                    if (!lastWasCr) sb.Append("\r\n");
                    lastWasCr = false;
                    continue;
                }
                lastWasCr = false;
                if (c < ' ' && c != '\t') continue;      // NUL, звонок и прочее — не для окна
                sb.Append(c);
            }
            return sb.ToString();
        }
    }
}
