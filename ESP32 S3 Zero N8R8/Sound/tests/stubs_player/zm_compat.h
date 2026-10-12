/* Модули ESP (C, GCC) на ПК под MSVC: что у GCC встроено.
 *
 * Подключается ко всем исходникам модулей ключом /FI. Атомарные операции
 * кольца плеера — над volatile: MSVC на x86 даёт таким обращениям семантику
 * acquire/release (/volatile:ms), большего кольцу одного производителя и
 * одного потребителя не нужно.
 */
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __atomic_load_n(pointer, order) (*(pointer))
#define __atomic_store_n(pointer, value, order) (*(pointer) = (value))
/* Сравнить и заменить 32 бита, как у GCC: 1 — заменено, иначе в *expected — что там. */
static __inline int zm_compat_cas32(volatile unsigned int* pointer, unsigned int* expected,
                                    unsigned int desired) {
  const long old = _InterlockedCompareExchange((volatile long*)pointer, (long)desired,
                                               (long)*expected);
  if ((unsigned int)old == *expected) {
    return 1;
  }
  *expected = (unsigned int)old;
  return 0;
}
#define __atomic_compare_exchange_n(pointer, expected, desired, weak, success, failure) \
  zm_compat_cas32((volatile unsigned int*)(pointer), (unsigned int*)(expected), (unsigned int)(desired))
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
