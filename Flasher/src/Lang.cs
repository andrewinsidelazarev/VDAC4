// SPDX-License-Identifier: Apache-2.0
// Язык окна: русский или английский. Тексты — прямо в местах вызова: Lang.T("по-русски", "in English").
using System.Globalization;

namespace Vdac4Flasher
{
    public static class Lang
    {
        /// <summary>true — русский (по умолчанию — по языку Windows).</summary>
        public static bool Russian = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru" ||
                                     CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "uk" ||
                                     CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "be";

        public static string T(string ru, string en) { return Russian ? ru : en; }
    }
}
