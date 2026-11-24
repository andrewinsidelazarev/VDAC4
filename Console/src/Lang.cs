// SPDX-License-Identifier: Apache-2.0
// Язык окна: русский или английский. Тексты — прямо в местах вызова: Lang.T("по-русски", "in English").
// Язык выбирается по языку Windows; ссылка в окне переключает его на ходу.
using System.Globalization;

namespace Vdac4Console
{
    public static class Lang
    {
        /// <summary>true — русский (по умолчанию — по языку Windows: русский, украинский, белорусский).</summary>
        public static bool Russian = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru" ||
                                     CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "uk" ||
                                     CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "be";

        /// <summary>Строка на языке окна: первая — русская, вторая — английская.</summary>
        public static string T(string ru, string en) { return Russian ? ru : en; }
    }
}
