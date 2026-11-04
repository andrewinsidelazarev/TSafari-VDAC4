/* Настройка libwebp для TSafari (своя, не из libwebp).
 *
 * Сборка задаёт HAVE_CONFIG_H, и libwebp читает этот файл вместо того, чтобы сам угадывать
 * возможности процессора. Здесь не объявлено ни одного WEBP_HAVE_SSE2 / SSE41 / NEON: у ESP32-S3
 * (Xtensa) таких наборов команд нет, а на стенде ПК (x86) это нарочно — чтобы и стенд считал тем
 * же кодом на чистом C, что и ESP32. Потоков (WEBP_USE_THREAD) тоже нет. */
#ifndef WEBP_WEBP_CONFIG_H_
#define WEBP_WEBP_CONFIG_H_
#endif
