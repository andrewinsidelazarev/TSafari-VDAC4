; Окно запуска TSafari на FT812: то же окно, что у браузера на старте (дизайн Codex, «стартовое
; окно Wi-Fi»), пока программа Z80 ищет папку и передаёт ELF. Хост FT812 здесь — сам Z80, по SPI
; Evo (#77 — выбор, #57 — данные), как экран загрузки DOOM VDAC4 (launcher/ftscreen.asm DOOM:
; запуск FT812 и обмен взяты оттуда).
;
; Образ RAM_G окна (статический пакет Codex и маски строк L4) и оба листа готовит на ПК
; tools/gen_launcher_screen.py: build/launcher_screen.inc и страницы SPG с LS_PAGE0. Адреса — те
; же, что у TSAFARI.ELF: когда ESP возьмёт FT812, браузер пишет в те же места свои строки.
;
; КАК Z80 ГОВОРИТ С FT812. Видеочип подключён по SPI — последовательной шине: байты уходят и
; приходят по одному через порт #57, а порт #77 выбирает, с кем говорим (FtOn — с FT812,
; FtOff — отпустить). Обращение к памяти FT812 (её устройство — main/ft.h) начинается с трёх
; байтов адреса (22 бита); старший бит первого байта: 1 — запись, 0 — чтение. Дальше идут
; данные. Кроме памяти, есть «команды хоста» (FtCmd): 3 байта — включить, сбросить, выбрать
; частоту. Сейчас хост FT812 — Z80; когда запустится браузер, хостом станет ESP32.
;
; FtScreen_Start — сразу после остановки прежнего приложения ESP: M_SEL, команды хоста (в слоте
; TS-Config ACTIVE переводит Tang в ядро FT812), ждём REG_ID, 1024×768@59, ждём шрифты ПЗУ (их
; в это время заливает ESP — порт FT812 не делим), образ RAM_G, окно «Starting TSafari...».
; FtShow — окно с другими строками (HL — лист). FtTick — кадр глобуса раз в ~100 мс; его зовут
; ожидания загрузчика (поиск по карте, обмен с ESP, передача ELF). FT812 не ответил — окна нет,
; загрузка идёт как без него. FtBar = 1 — ELF передаётся в ESP: под «Starting TSafari...» полоса,
; заполненная на долю Offset / ElfSize (пересчёт — с каждым кадром глобуса).
        INCLUDE "launcher_screen.inc"

LS_PAGE0        EQU #40                 ; первая страница образа окна в SPG (tools/package.py)
FT_REG_HI       EQU #30                 ; регистры #3020xx, RAM_DL #30xxxx
REG_ID_LO       EQU #2000
REG_FRAMES_LO   EQU #2004
REG_DLSWAP_LO   EQU #2054
GLOBE_FR        EQU 6                   ; кадр глобуса — раз в 6 кадров FT812 (~100 мс)

; --- SPI FT812 ----------------------------------------------------------------------------
; Выбрать FT812 на шине SPI (сначала шину берёт Evo, затем выбор FT812).
FtOn:
        ld a,#02                        ; как BootTrace: шина — Evo, затем выбор FT812
        out (#77),a
        ld a,#07
        out (#77),a
        ret
; Отпустить FT812: конец обращения.
FtOff:
        ld a,#03
        out (#77),a
        ret

; Команда хоста: B — команда, C — параметр.
FtCmd:
        call FtOn
        ld a,b
        out (#57),a
        ld a,c
        out (#57),a
        xor a
        out (#57),a
        jr FtOff

; Запись: A — разряды 21…16 адреса, DE — 15…0, HL — откуда, BC — байтов (не 0).
; Выход: HL — за записанным. Портит AF, BC, DE.
FtWrite:
        push af
        call FtOn
        pop af
        or #80
        out (#57),a
        ld a,d
        out (#57),a
        ld a,e
        out (#57),a
        ; otir — «вывести B байтов из (HL) в порт C, HL растёт»; B = 0 значит 256 байтов.
        ; Поэтому длину BC шлём так: сначала остаток C байтов, потом B раз по 256.
        ld a,c
        ld d,b
        ld c,#57
        or a
        jr z,.blocks
        ld b,a
        otir                            ; остаток (BC mod 256)
.blocks:
        ld a,d
        or a
        jr z,.done
.block:
        ld b,0
        otir                            ; по 256
        dec d
        jr nz,.block
.done:
        jr FtOff

; Чтение: A — разряды 21…16, DE — 15…0, HL — куда, B — байтов (1…255).
; При чтении после адреса FT812 нужен один пустой байт: за это время он достаёт данные.
; inir — «ввести B байтов из порта C в (HL), HL растёт».
FtRead:
        push bc
        push af
        call FtOn
        pop af
        and #3F
        out (#57),a
        ld a,d
        out (#57),a
        ld a,e
        out (#57),a
        xor a
        out (#57),a                     ; пустой байт после адреса
        in a,(#57)                      ; холостое чтение: начинает обмен первого байта
        pop bc
        ld c,#57
        inir
        jr FtOff

; Слово по адресу A:DE → FtBuf.
FtRd32:
        ld hl,FtBuf
        ld b,4
        jr FtRead

; Регистр #30:DE ← HL (32 бита, старшие — 0).
FtWrReg:
        ld (FtBuf),hl
        ld hl,0
        ld (FtBuf+2),hl
        ld a,FT_REG_HI
        ld hl,FtBuf
        ld bc,4
        jr FtWrite

; ~26 тактов на BC.
FtDelay:
        dec bc
        ld a,b
        or c
        jr nz,FtDelay
        ret

; --- запуск окна --------------------------------------------------------------------------
; Порядок, как у настоящего FT812: включить M_SEL, сбросить ядро (CORERST), внешний генератор
; (CLKEXT), сон, частота, ACTIVE — работать. Потом ждать, пока в REG_ID появится #7C (FT812
; отвечает), задать развёртку 1024×768 (FtModeTab), показать пустой фон и залить картинки окна.
FtScreen_Start:
        xor a
        ld (FtOk),a
        ld (FtFrame),a
        ; кадр — от FT812 (M_SEL): ядро FT812 при M_SEL = 0 вернулось бы в слот TS-Config
        ld bc,#00AF
        ld a,4
        out (c),a
        ld bc,#6800                     ; CORERST
        call FtCmd
        ld bc,#4400                     ; CLKEXT
        call FtCmd
        ld bc,#4100                     ; SLEEP
        call FtCmd
        ld bc,#61C8                     ; CLKSEL ×8: 64 МГц
        call FtCmd
        ld bc,#0000                     ; ACTIVE
        call FtCmd
        ; REG_ID = #7C: ядро FT812 отвечает (переход в слот FT812 — ~0,2 с); ждать до ~3 с
        ld hl,300
.id:
        push hl
        ld bc,5000                      ; ~10 мс
        call FtDelay
        ld a,FT_REG_HI
        ld de,REG_ID_LO
        call FtRd32
        pop hl
        ld a,(FtBuf)
        cp #7C
        jr z,.found
        dec hl
        ld a,h
        or l
        jr nz,.id
        ret                             ; FT812 нет — без окна
.found:
        ; таблица развёртки: пары «регистр, значение», конец — 0
        ld hl,FtModeTab
.mode:
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        ld a,d
        or e
        jr z,.mode_end
        ld c,(hl)
        inc hl
        ld b,(hl)
        inc hl
        push hl
        ld h,b
        ld l,c
        call FtWrReg
        pop hl
        jr .mode
.mode_end:
        ; пока окно не готово — пустой фон бумаги (иначе на экране мусор из RAM_DL)
        ld hl,FtDlPaper
        ld bc,FtDlPaperEnd-FtDlPaper
        call FtSendDlRaw
        ; шрифты ПЗУ заливает ESP сразу после загрузки ядра (~0,4 с): не мешать ему в RAM_G,
        ; ждать корень шрифтов (ROM_FONTROOT #2FFFFC ≠ 0) не дольше ~3 с
        ld hl,300
.fonts:
        push hl
        ld a,#2F
        ld de,#FFFC
        call FtRd32
        pop hl
        ld a,(FtBuf+2)
        or a
        jr nz,.image
        push hl
        ld bc,5000
        call FtDelay
        pop hl
        dec hl
        ld a,h
        or l
        jr nz,.fonts
.image:
        call FtUploadImage
        ld a,1
        ld (FtOk),a
        ld hl,LsDlLoad
        ; FtShow

; Окно с листом HL (LsDlLoad — запуск, LsDlSearch — поиск по карте).
FtShow:
        ld (FtTpl),hl
        ld a,(FtOk)
        or a
        ret z
        jr FtDraw

; Кадр глобуса — если прошло GLOBE_FR кадров FT812. Портит всё.
; Время меряем счётчиком кадров FT812 (REG_FRAMES, 59 раз в секунду): берём младший байт и
; сравниваем с прошлым — вычитание байтов верно и при переходе 255 → 0.
FtTick:
        ld a,(FtOk)
        or a
        ret z
        ld a,FT_REG_HI
        ld de,REG_FRAMES_LO
        call FtRd32
        ld a,(FtBuf)
        ld hl,FtLastFrame
        sub (hl)
        cp GLOBE_FR
        ret c
        ld a,(FtFrame)
        inc a
        cp LS_GLOBE_FRAMES
        jr c,.frame
        xor a
.frame:
        ld (FtFrame),a
        ; FtDraw

; Лист FtTpl с кадром глобуса FtFrame — в RAM_DL и смена. Смена ещё ждёт кадра — пропустить.
; Готовые дисплей-листы окна сделал на ПК tools/gen_launcher_screen.py; здесь лист копируется
; в FtDl, и в нём меняется только адрес картинки глобуса — так глобус вращается.
FtDraw:
        ld a,FT_REG_HI
        ld de,REG_DLSWAP_LO
        call FtRd32
        ld a,(FtBuf)
        and 3
        ret nz
        ld a,FT_REG_HI
        ld de,REG_FRAMES_LO
        call FtRd32
        ld a,(FtBuf)
        ld (FtLastFrame),a
        ld hl,(FtTpl)
        ld de,FtDl
        ld bc,LS_DL_BYTES
        ldir
        ; BITMAP_SOURCE глобуса: LS_GLOBE0 + кадр · 512 — меняется только байт 1 адреса
        ASSERT (LS_GLOBE0 & #1FF) == 0 && ((LS_GLOBE0 >> 8) & 255) + 2 * LS_GLOBE_FRAMES < 256
        ld a,(FtFrame)
        add a,a
        add a,(LS_GLOBE0 >> 8) & 255
        ld (FtDl+LS_GLOBE_WORD+1),a
        ; Полоса передачи ELF — последняя в листе. Передачи нет — на месте её первого слова конец
        ; листа (DISPLAY = 0), и FT812 её не рисует.
        ld a,(FtBar)
        or a
        jr nz,.bar
        ld hl,0
        ld (FtDl+LS_BAR_WORD),hl
        ld (FtDl+LS_BAR_WORD+2),hl
        jr .send
.bar:
        ; Правая вершина заполнения: X = LS_BAR_X0 + доля. В слове VERTEX2F X стоит как X·16,
        ; сдвинутое на 15 бит, то есть байт 2 слова — младший байт X·8, байт 3 — старший | #40
        ; (код команды). Байты 0–1 (Y) не меняются.
        call FtBarPx
        ld de,LS_BAR_X0
        add hl,de
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,l
        ld (FtDl+LS_BAR_FILL+2),a
        ld a,h
        or #40
        ld (FtDl+LS_BAR_FILL+3),a
.send:
        ld hl,FtDl
        ld bc,LS_DL_BYTES
        ; FtSendDlRaw

; Лист HL (BC байтов) — в RAM_DL, REG_DLSWAP ← FRAME, след загрузчика — снова в FT_SEEN.
FtSendDlRaw:
        ld a,FT_REG_HI                  ; RAM_DL #300000
        ld de,#0000
        call FtWrite
        ld hl,2                         ; REG_DLSWAP ← FRAME
        ld de,REG_DLSWAP_LO
        call FtWrReg
        jp BootTrace

; Доля переданного ELF в точках полосы: HL = Offset · 320 / ElfSize (0…LS_BAR_SPAN).
; Сначала частное q = Offset / (ElfSize / 256) — сколько 256-х долей файла уже ушло (0…256),
; потом q + q/4 — те же доли в точках (256 + 64 = 320). Деление «в столбик», как на бумаге, только
; в двоичной системе: 24 раза сдвигаем делимое на бит влево, его старший бит переходит в остаток;
; если остаток не меньше делителя — вычитаем делитель и пишем в частное 1, иначе 0. Цифры частного
; въезжают в освободившиеся младшие биты делимого, так что в конце там само частное. ELF меньше
; 8 МБ (ESP берёт модули до 2 МБ): делитель меньше #8000, и сдвинутый остаток влезает в 16 бит.
; Портит AF, BC, DE (IX бережёт — им пользуется чтение карты).
FtBarPx:
        ld de,(ElfSize+1)               ; делитель DE = размер / 256 (байты 1 и 2 размера)
        ld hl,LS_BAR_SPAN
        ld a,d
        or e
        ret z                           ; ELF меньше 256 байт — сразу вся полоса
        push ix
        ld ix,(Offset)                  ; делимое C:IX = Offset (24 бита)
        ld a,(Offset+2)
        ld c,a
        ld hl,0                         ; остаток
        ld b,24
.div:
        add ix,ix                       ; делимое влево; бит 0 свободен — под цифру частного
        rl c
        adc hl,hl                       ; старший бит делимого — в остаток
        or a
        sbc hl,de
        jr nc,.one
        add hl,de                       ; не вычлось — остаток как был, цифра 0
        djnz .div
        jr .done
.one:
        inc ix                          ; цифра частного 1
        djnz .div
.done:
        push ix
        pop hl                          ; частное (C = 0: оно меньше 512)
        pop ix
        ld de,257                       ; больше 256 — только от округления размера: вся полоса
        or a
        sbc hl,de
        add hl,de
        jr c,.fit
        ld hl,256
.fit:
        ld d,h                          ; HL = q + q/4
        ld e,l
        srl d
        rr e
        srl d
        rr e
        add hl,de
        ret

; Образ RAM_G окна — со страниц SPG LS_PAGE0… через окно #C000 (потом окно — снова #20).
; Образ больше 16 КБ, а в окно памяти Z80 #C000–#FFFF помещается одна страница ОЗУ (16 КБ).
; Поэтому страницы образа по очереди включаются в это окно (порт #13AF) и отправляются в FT812.
FtUploadImage:
        ld a,LS_PAGE0
        ld (FtPage),a
        ld hl,LS_BASE & #FFFF
        ld (FtAddr),hl
        ld hl,LS_BYTES & #FFFF
        ld (FtLeft),hl
.page:
        ld a,(FtPage)
        ld bc,#13AF
        out (c),a
        ld hl,(FtLeft)
        ld de,16384
        or a
        sbc hl,de
        jr c,.last
        ld (FtLeft),hl
        ld bc,16384
        jr .send
.last:
        ld bc,(FtLeft)
        ld hl,0
        ld (FtLeft),hl
.send:
        ld a,b
        or c
        jr z,.done
        push bc
        ld hl,#C000
        ld de,(FtAddr)
        ld a,(LS_BASE >> 16) & 255
        call FtWrite
        pop bc
        ld hl,(FtAddr)
        add hl,bc
        ld (FtAddr),hl
        ld hl,FtPage
        inc (hl)
        ld hl,(FtLeft)
        ld a,h
        or l
        jr nz,.page
.done:
        ld bc,#13AF                     ; окно #C000 — снова буфер загрузчика
        ld a,#20
        out (c),a
        ret

; 1024×768@59, как у ESP (ft_host::boot1024x768): регистр (младшие 16 бит), значение.
; Развёртка: H… — по горизонтали (в точках), V… — по вертикали (в строках): ширина импульса
; синхронизации, где начинается картинка, её размер и полная длина строки или кадра.
; REG_PCLK = 0 — остановить вывод на время настройки, в конце 1 — запустить.
FtModeTab:
        dw #2070,0                      ; REG_PCLK
        dw #2038,24                     ; REG_HSYNC0
        dw #203C,24+136                 ; REG_HSYNC1
        dw #2030,24+136+160             ; REG_HOFFSET
        dw #2034,1024                   ; REG_HSIZE
        dw #202C,1344                   ; REG_HCYCLE
        dw #204C,2                      ; REG_VSYNC0
        dw #2050,8                      ; REG_VSYNC1
        dw #2044,37                     ; REG_VOFFSET
        dw #2048,768                    ; REG_VSIZE
        dw #2040,806                    ; REG_VCYCLE
        dw #206C,0                      ; REG_PCLK_POL
        dw #2068,0                      ; REG_CSPREAD
        dw #2070,1                      ; REG_PCLK
        dw 0

FtDlPaper:                              ; CLEAR_COLOR бумаги, CLEAR, DISPLAY
        dd #02F7F3E7,#26000007,0
FtDlPaperEnd:

FtOk: db 0                              ; 1 — FT812 ответил, окно показано
FtFrame: db 0                           ; кадр вращения глобуса
FtBar: db 0                             ; 1 — ELF передаётся: в окне полоса доли Offset / ElfSize
FtLastFrame: db 0                       ; REG_FRAMES при прошлой смене кадра
FtPage: db 0                            ; FtUploadImage: страница образа, адрес в RAM_G, остаток
FtAddr: dw 0
FtLeft: dw 0
FtTpl: dw LsDlLoad                      ; какой лист окна сейчас показан
FtBuf: ds 4                             ; прочитанное слово FT812
FtDl: ds LS_DL_BYTES                    ; копия листа с нужным кадром глобуса
