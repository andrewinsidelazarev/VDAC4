/* VDAC4: управление libxmp из TRACKER.ELF сверх API xmp.h (только сборка с
 * LIBXMP_ESP_MODULE; реализация — esp/xmp_esp_shim.c). */
#ifndef XMP_ESP_H
#define XMP_ESP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Сжатие отсчётов при загрузке, когда памяти ESP не хватает на все: флаги
 * действуют на загрузки после вызова, пока их не снимут (0). Петли и частота
 * сэмпла не меняются — они в кадрах. */
#define XMP_ESP_REDUCE_MONO 1   /* стерео сэмпл сводится в моно (среднее каналов) */
#define XMP_ESP_REDUCE_8BIT 2   /* 16 бит — в 8 (с округлением) */
void xmp_esp_reduce(int flags);

#ifdef __cplusplus
}
#endif

#endif /* XMP_ESP_H */
