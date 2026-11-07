# Сопроцессор FT812 для стенда ПК

Копия переносимого ядра сопроцессора BL616 (`VDAC4/Tang Nano 20k/BL616/copro` проекта
«Tang Nano 20k VDAC»: `ft_copro.c`, `ft_copro.h`, `ft_image.c`, `ft_inflate.c`, `ft_3d.c`,
`vdac3d.h`; лицензия Apache-2.0, как у оригинала) и её libwebp с правками автора прошивки
(`libwebp/`, `libwebp/README.VDAC4.md`; BSD). Версия — **прошивка BL616 40–41** (07.11.2026, взята
из рабочей копии VDAC4 до коммита; у 41 код сопроцессора тот же; на плате — 41): прогрессивный
JPEG, PNG всех видов, WebP, `CMD_LOADIMAGE_BOX` (картинка «в рамку» с уменьшением),
`CMD_IMAGEINFO`, отчёт об ошибке в RAM_CMD. Описание — `VDAC4/Tang Nano 20k/BL616/docs/images.md`.

У браузера своя libwebp (`third_party/libwebp`) с теми же именами функций, поэтому
`tests/run_host.sh` даёт именам libwebp сопроцессора приставку `bl616_` (`llvm-objcopy
--redefine-syms`).

На плате эту работу делает прошивка BL616 модуля Tang, здесь — тот же код в стенде
`tests/host.c`: браузер кладёт картинки JPEG и PNG в кольцо RAM_CMD командой
`CMD_LOADIMAGE_BOX` (со старой прошивкой — `CMD_LOADIMAGE`), и стенд раскладывает их в RAM_G
так же, как BL616.

`ft_inflate.c` включает `third_party/miniz/miniz_tinfl.c` (тот же файл, что у браузера), поэтому
стенд собирает распаковку только из него, без отдельного `miniz_tinfl.c` (`tests/run_host.sh`).

Обновлять копию — когда меняется код сопроцессора BL616 (новая прошивка BL616 на плате).
