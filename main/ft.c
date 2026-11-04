/* Работа с видеочипом FT812: включить его, писать в его память, менять дисплей-лист.
 * Что такое RAM_G, RAM_DL и смена листа — в ft.h. Образец — video.c из DOOM VDAC4. */
#include "ft.h"
#include "ts.h"

/* Переключить Tang в режим FT812 и стать его хозяином.
 * Сначала проверяем, что прошивка ESP32 вообще умеет ft_open: таблица zm_core_t растёт от версии
 * к версии, и в старой прошивке нужного поля может не быть. Поле есть, если размер таблицы
 * (ts_core->size) доходит до его конца — так проверяют все новые функции прошивки.
 * Сама ft_open ждёт до timeout_ms, пока ПЛИС перезагрузится в ядро FT812. */
int ft_init(uint32_t timeout_ms) {
    if (ts_core->size < offsetof(zm_core_t, ft_close) + sizeof(ts_core->ft_close) || !ts_core->ft_open)
        return ZM_ERROR;
    return ts_core->ft_open(timeout_ms);
}

/* Записать байты в память видеочипа (адрес и длина кратны 4 — память пишется словами).
 * Пишем блоками по 1024 слова (4 КБ): на каждый блок прошивка тратит время на установку адреса
 * и проверку, и чем крупнее блок, тем меньше эти расходы на каждое слово (замер DOOM VDAC4).
 * Перед каждым блоком смотрим ts_running: если браузер закрывают, бросаем запись. */
int ft_put(uint32_t address, const void *data, size_t bytes) {
    const uint32_t *p = (const uint32_t *)data;
    size_t words = bytes / 4;
    while (words) {
        if (!__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) return 0;
        size_t n = words > 1024 ? 1024 : words;
        if (ts_core->ft_write(address, p, (uint32_t)n) != ZM_OK) return 0;
        address += (uint32_t)n * 4;
        p += n;
        words -= n;
    }
    return 1;
}

/* Счётчик кадров видеочипа: растёт на 1 каждый кадр (59 раз в секунду). По нему удобно
 * отмерять анимацию и проверять, что видеочип жив. */
uint32_t ft_frames(void) {
    uint32_t v = 0;
    return ts_core->ft_read(FT_REG_FRAMES, &v) == ZM_OK ? v : 0;
}

/* Дождаться, пока видеочип возьмёт прошлый дисплей-лист: он берёт его только в конце кадра,
 * а до этого RAM_DL трогать нельзя — испортим список, по которому сейчас рисуется экран.
 * Регистр REG_DLSWAP: 2 — «смена заказана», 0 — «смена прошла». Ждём не дольше 2 секунд:
 * если дольше — видеочип не работает, пишем об этом в журнал. */
int ft_swap_wait(void) {
    uint32_t swap = 1, start = ts_core->millis();
    for (;;) {
        if (!__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) return 0;
        if (ts_core->ft_read(FT_REG_DLSWAP, &swap) != ZM_OK) return 0;
        if (!swap) return 1;
        if ((uint32_t)(ts_core->millis() - start) > 2000) {
            ts_log("tsafari: FT812 frame swap timeout");
            return 0;
        }
        ts_core->delay(1);                       /* отдать процессор другим задачам на 1 мс */
    }
}

/* Показать новый дисплей-лист: дождаться прошлой смены, записать список в RAM_DL и заказать
 * смену в конце кадра (2 = DLSWAP_FRAME). Возвращает 0, если что-то не вышло. */
int ft_dl_commit(const uint32_t *dl, unsigned words) {
    static const uint32_t swap = 2;              /* DLSWAP_FRAME */
    if (!words || words > FT_DL_WORDS) return 0;
    if (!ft_swap_wait()) return 0;
    if (!ft_put(FT_RAM_DL, dl, words * 4u)) return 0;
    return ft_put(FT_REG_DLSWAP, &swap, 4);
}

/* Описать картинку для текущей «ручки» (handle — номер описания картинки, 0…31): где она лежит в
 * RAM_G, в каком формате, сколько байт в строке (stride), ширина и высота. Команд пять:
 *   BITMAP_SOURCE   — адрес;
 *   BITMAP_LAYOUT   — формат, младшие биты stride и высоты;
 *   BITMAP_LAYOUT_H — старшие биты stride и высоты (нужны, если больше 1023 байт / 511 строк);
 *   BITMAP_SIZE     — младшие биты размера на экране (фильтр NEAREST — без сглаживания,
 *                     BORDER — за краем картинки ничего не рисовать);
 *   BITMAP_SIZE_H   — старшие биты размера.
 * Старшие слова _H пишем ВСЕГДА, даже если они нулевые: ручка помнит их от прошлой картинки.
 * Без этого картинка после большой (страница: шаг и ширина 1024) получала бы чужие старшие биты
 * — на стенде ядра фото, иконки и курсор после страницы выходили мусором. */
void dl_bitmap(ft_dl_t *d, uint32_t address, unsigned format, unsigned stride, unsigned w, unsigned h) {
    dl_put(d, dl_source(address));
    dl_put(d, (0x07u << 24) | (format << 19) | ((stride & 0x3FFu) << 9) | (h & 0x1FFu));
    dl_put(d, (0x28u << 24) | (((stride >> 10) & 3u) << 2) | ((h >> 9) & 3u));
    dl_put(d, (0x08u << 24) | ((w & 0x1FFu) << 9) | (h & 0x1FFu));
    dl_put(d, (0x29u << 24) | (((w >> 9) & 3u) << 2) | ((h >> 9) & 3u));
}
