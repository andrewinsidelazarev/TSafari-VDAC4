/* Сохранение скачанного файла на карту Evo.
 *
 * Если по ссылке пришла не страница, а файл (архив, образ диска…), браузер его не
 * показывает, а сохраняет на карту компьютера ZX Evolution в \zifi\downloads\ГГГГ-ММ-ДД\ —
 * туда же, куда сохраняет ZiFi SPG. ESP32 сам до карты не достаёт: файл лежит в его памяти
 * (PSRAM), а программа Z80 (launcher/download.asm) забирает его кусками и пишет на карту.
 * Как они обмениваются — в начале launcher/download.asm.
 *
 * Ход загрузки — состояния:
 *   DL_WAIT   — ESP32 сообщил программе Z80 «есть файл» и ждёт, когда она начнёт брать куски;
 *   DL_SEND   — программа Z80 берёт куски и пишет их на карту;
 *   DL_DONE   — файл записан целиком; DL_FAILED — не вышло (текст — в error). */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DL_IDLE, DL_WAIT, DL_SEND, DL_DONE, DL_FAILED };

#define DL_NAME_MAX 48                   /* самое длинное имя на карте (как у ZiFi SPG) */

/* Что показать в индикаторе. */
typedef struct {
    int state;                           /* DL_… */
    char name[DL_NAME_MAX * 2 + 1];      /* имя файла, UTF-8 */
    char path[160];                      /* куда записан: \zifi\downloads\ГГГГ-ММ-ДД\имя (DL_DONE) */
    char error[80];                      /* что не вышло (DL_FAILED) */
    int cancelled;                       /* DL_FAILED из-за отмены */
    uint32_t size, sent;                 /* размер и сколько уже отдано программе Z80 */
    uint32_t changed;                    /* когда сменилось состояние, мс */
} download_info_t;

void download_init(void);
/* Начать сохранение: body (length байт, в PSRAM) модуль забирает себе и освободит сам; имя —
 * из Content-Disposition (если сервер его дал) или из последней части адреса. 0 — уже идёт
 * другое сохранение (body тогда освобождён). */
int download_start(uint8_t *body, size_t length, const char *url, const char *disposition);
/* Раз в проход цикла браузера: отправить событие программе Z80, следить за сроками. */
void download_tick(uint32_t now);
/* Файл не докачался из сети (ошибка или отмена) — до записи на карту дело не дошло: показать
 * это в индикаторе тем же состоянием DL_FAILED. */
void download_note(const char *url, const char *disposition, const char *error, int cancelled);
/* Отменить текущее сохранение (программа Z80 получит отказ и закроет файл). */
void download_cancel(void);
/* 1 — сохранение идёт (DL_WAIT или DL_SEND). */
int download_busy(void);
/* Команды программы Z80 0x76 (дай кусок) и 0x77 (итог); вызывает обмен прошивки. */
int download_command(uint8_t cmd, const uint8_t *p, uint16_t n);
/* Копия состояния для индикатора. */
void download_info(download_info_t *out);
/* Имя файла на карте (CP866, не длиннее DL_NAME_MAX) по правилам драйвера FAT32 и ZiFi SPG. */
void download_name(const char *url, const char *disposition, char *out, size_t cap);
