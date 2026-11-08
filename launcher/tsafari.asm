; TSafari — веб-браузер VDAC4. Это программа для Z80 (файл TSAFARI.SPG), она работает на самом
; компьютере ZX Evolution. Сам браузер — TSAFARI.ELF — работает на ESP32 (общая схема — в
; main/ts.h). Задача этой программы — запустить его и служить ему клавиатурой и мышью:
;   1) остановить прежнее приложение ESP32 и показать на FT812 окно запуска (ftscreen.asm);
;   2) найти на SD-карте папку с TSAFARI.ELF: путь берётся из path.cfg на ESP32, а при первом
;      запуске — поиском по всей карте (directory.asm), найденный путь сохраняется;
;   3) передать TSAFARI.ELF в ESP32 кусками по 1016 байт — если его копии ещё нет в кэше ESP32;
;   4) включить M_SEL (изображение — от FT812) и попросить ESP32 запустить приложение;
;   5) дальше в цикле: пересылать в ESP32 мышь и клавиши и спрашивать, не вышел ли браузер.
; Выход — по F12: ESP32 отвечает «состояние 3», программа останавливает приложение, пишет на
; экране ZX «TSAFARI CLOSED» и ждёт сброса компьютера.
;
; Связь с ESP32 — команды и ответы по протоколу ZiFi (proto.asm, zifi_spi.asm): у каждой
; команды свой код (CMD_…), ESP32 отвечает кодом ответа (RESP_…) и данными в ProtoBuf.
; Образец — запускатель DOOM VDAC4 (launcher/doom.asm); модули поиска, path.cfg, чтения ELF,
; протокола и диагностики взяты оттуда.
;
; Для начинающего о Z80: A, B, C, D, E, H, L — 8-битные регистры; пары BC, DE, HL — 16-битные.
; «out (c),a» пишет байт A в порт с адресом BC, «in a,(c)» — читает. «call» — вызов
; подпрограммы, «ret» — возврат; флаг C (перенос) и Z (ноль) подпрограммы используют как ответ
; «ошибка» / «да». BootMark N — номер этапа: при ошибке он виден на экране (diagnostic.asm).
        DEVICE ZXSPECTRUM128
        DEFINE ZIFI_SPI
        INCLUDE "fat32.inc"
        INCLUDE "filex.inc"
        ORG #8000
Start:
        di                              ; запретить прерывания: программа всё делает сама
        ld sp,#BFF0                     ; стек — под адресом #BFF0
        ; Настройка памяти TS-Conf: порты #xxAF — регистры конфигурации, номер регистра —
        ; старший байт адреса порта. #21 (MemConfig) = #0E — в окне #0000–#3FFF ОЗУ вместо ПЗУ,
        ; запись разрешена; #10, #11, #13 — какие страницы ОЗУ видны в окнах #0000, #4000, #C000.
        ld bc,#21AF
        ld a,#0E
        out (c),a
        ld bc,#10AF
        ld a,#0F
        out (c),a
        inc b
        ld a,#0E
        out (c),a
        ld bc,#13AF
        ld a,#20
        out (c),a
        xor a
        out (#FE),a                     ; бордюр чёрный
        ld a,1
        call BootMark
        call ZiFi_Init                  ; связь с ESP32
        jp c,Fail
        ; прежнее приложение ESP — остановить до наших запросов
        ld a,2
        call BootMark
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ld a,RESP_APP_STOP
        ld b,32
        call WaitLong
        jp c,Fail
        call FtScreen_Start             ; окно «Starting TSafari...» (нет FT812 — без окна)
        ; SD-карта: драйвер FAT32 (third_party/fat32), рабочий буфер — с #3900
        ld a,3
        call BootMark
        ld hl,#3900
        xor a
        call FAT32_BIND
        call FAT32_DEVICE_INIT
        jp nz,Fail
        ld a,4
        call BootMark
        call FAT32_MOUNT
        jp nz,Fail
        ld a,5
        call BootMark
        call PathCfgLoad                ; папка из path.cfg ESP — без поиска
        jr nz,.located
        ld hl,LsDlSearch                ; первый запуск: «Looking for TSAFARI.ELF...»
        call FtShow
        call LocateGame
        jp c,Fail
        call PathCfgSave
        ld hl,LsDlLoad
        call FtShow
.located:
        ld a,6
        call BootMark
        ld hl,ElfSpec
        call FAT32_FIND                 ; найти TSAFARI.ELF в папке; DE:HL — размер файла
        jp c,FileFindError
        jp z,Fail
        ld (ElfSize),hl
        ld (ElfSize+2),de
        ld (Query+1),hl
        ld (Query+3),de
        ; дата изменения FAT — ключ кэша ESP: новый ELF на карте вытесняет старую копию
        ld hl,#3868+22
        ld de,Query+5
        ld bc,4
        ldir
        ld hl,FastMaxExtents            ; карта ELF — до #FFFF
        ld (FastExtMax),hl
        call FastOpen
        jr z,.mapped
        ld hl,MsgReadError
        ld (BootMessage),hl
        jp Fail
.mapped:
        ; Спросить ESP32: есть ли у тебя TSAFARI.ELF такого размера и с такой датой?
        ; Ответ 0 — есть в кэше, передавать не нужно; 1 — нет, присылай.
        ld a,7
        call BootMark
        ld hl,Query
        ld bc,QueryEnd-Query
        ld a,CMD_MOD_QUERY
        call Proto_Send
        ld a,RESP_MOD_QUERY
        ld b,16
        call WaitLong
        jp c,Fail
        ld a,(ProtoBuf)
        or a
        jp z,Uploaded                   ; ELF уже в кэше ESP
        cp 1
        jp nz,Fail
        ld a,1                          ; ELF передаётся: в окне полоса передачи (ftscreen.asm)
        ld (FtBar),a
        ld hl,LsDlLoad
        call FtShow
; Передача ELF: сколько осталось (размер − Offset, 32-битное число в двух парах регистров) —
; кусок по 1016 байт или меньше, если осталось меньше. Кусок читается с карты в PutBuf+4,
; в первые 4 байта PutBuf пишется смещение куска в файле, и всё уходит командой CMD_MOD_PUT.
; Каждый кусок — шаг глобуса в окне запуска (FtTick).
Upload:
        ld a,8
        call BootMark
        ld hl,(ElfSize)
        ld de,(Offset)
        or a
        sbc hl,de
        push hl
        ld hl,(ElfSize+2)
        ld de,(Offset+2)
        sbc hl,de
        ld a,h
        or l
        pop hl
        ld bc,1016
        jr nz,.length
        ld a,h
        or l
        jr z,Commit
        push hl
        or a
        sbc hl,bc
        pop hl
        jr nc,.length
        ld b,h
        ld c,l
.length:
        ld (Chunk),bc
        ld (Fx+10),bc
        ld hl,(Offset)
        ld (Fx+4),hl
        ld (PutBuf),hl
        ld hl,(Offset+2)
        ld (Fx+6),hl
        ld (PutBuf+2),hl
        ld hl,PutBuf+4
        ld (Fx+8),hl
        call ReadAt
        jp nz,Fail
        ld hl,PutBuf
        ld bc,(Chunk)
        inc bc
        inc bc
        inc bc
        inc bc
        ld a,CMD_MOD_PUT
        call Proto_Send
        ld a,RESP_MOD_PUT
        ld b,16
        call WaitLong
        jp c,Fail
        ld a,(ProtoBuf)
        cp 1
        jp nz,Fail
        ld hl,(Offset)
        ld bc,(Chunk)
        add hl,bc
        ld (Offset),hl
        ld hl,(Offset+2)
        ld bc,0
        adc hl,bc
        ld (Offset+2),hl
        call FtTick
        jp Upload
; Всё передано: ESP32 проверяет файл и кладёт его в кэш.
Commit:
        ld a,9
        call BootMark
        ld a,CMD_MOD_COMMIT
        call Proto_SendEmpty
        ld a,RESP_MOD_COMMIT
        ld b,64
        call WaitLong
        jp c,Fail
        ld a,(ProtoBuf)
        cp 1
        jp nz,Fail
Uploaded:
        ld a,10
        call BootMark
        ; M_SEL = 1 до запуска: ESP переводит Tang в Slot1 (FT812), пока Evo держит экран у FT812.
        ; M_SEL — «изображение от FT812»; включается записью 4 в порт #00AF, выключается — 0.
        ld bc,#00AF
        ld a,4
        out (c),a
        ld hl,ElfSpec+1
        ld bc,ElfSpecEnd-ElfSpec-1
        ld a,CMD_APP_START
        call Proto_Send
        ld a,RESP_APP_START
        ld b,32
        call WaitLong
        jp c,Fail
        ld a,(ProtoBuf)
        or a
        jr z,.started
        ld hl,MsgStart
        cp #FE                          ; ABI ZM_NO_MEMORY (−2)
        jr nz,.start_error
        ld hl,MsgEspMemory
.start_error:
        ld (BootMessage),hl
        jp Fail
.started:
        ld a,1
        ld (AppStarted),a
        ; FT812 теперь у ESP32: окно запуска больше не трогать. Иначе FtTick в ожиданиях ответа
        ; ESP32 (WaitLong — например, при загрузке файла) писал бы в RAM_DL видеочипа старый
        ; лист окна поверх экрана браузера.
        xor a
        ld (FtOk),a
        ld a,11
        call BootMark
; Главный цикл: принять ответ ESP32, если пришёл. Ответ #F3 — состояние браузера (его
; просит SendKeys раз в 32 посылки); событие #E7 — браузер скачал файл, его надо записать на
; карту (download.asm). Остальные ответы (#F5 на клавиши) просто пропускаются.
; KeyDivider — счётчик: клавиши и мышь отправляются раз в 256 проходов цикла.
Run:
        call Proto_Poll
        jr nc,.keys
        cp #E7
        call z,ServeDownload            ; возвращает A = 0: дальше не совпадёт
        cp #E8
        call z,ServeCache               ; кэш картинок (imgcache.asm), тоже A = 0
        cp #F3                          ; состояние браузера: 3 — вышел по F12, FF — ошибка
        jr nz,.keys
        ld a,(ProtoBuf)
        cp #FF
        jp z,AppFailed
        cp 3
        jp z,Stop
.keys:
        ld hl,KeyDivider
        dec (hl)
        jr nz,Run
        call SendKeys
        jp Run

; Браузер сообщил об ошибке: забрать её текст для экрана диагностики.
AppFailed:
        call CaptureAppError
        ld a,#FF
        jp Fail

; Клавиатура — только очередь скан-кодов PS/2 (набор 2) контроллера AVR Evo, матрица ZX не
; читается. Байты уходят как есть, разбор — в TSAFARI.ELF (input.c).
; 75 [джойстик Kempston, кнопки мыши, X мыши, Y мыши, байты PS/2 — 0..PS2_DRAIN].
PS2_DRAIN       equ 24                  ; за опрос не больше: очередь AVR — 16 байт
SendKeys:
        ld hl,KeyBuf
        in a,(#1F)                      ; джойстик Kempston; #FF — джойстика нет
        cp #FF
        jr nz,.kempston
        xor a
.kempston:
        and 31
        ld (hl),a
        ; Мышь Kempston (AVR Evo, PS/2): #FADF — кнопки (бит 0 левая, 1 правая, 2 средняя,
        ; нажатая — 0; биты 7–4 — колесо), #FBDF — X (растёт вправо), #FFDF — Y (растёт
        ; вверх). Счётчики по модулю 256 уходят как есть: разность считает ESP.
        inc hl
        ld bc,#FADF
        in a,(c)
        ld (hl),a
        inc hl
        inc b                          ; #FBDF
        in a,(c)
        ld (hl),a
        inc hl
        ld b,#FF                       ; #FFDF
        in a,(c)
        ld (hl),a
        inc hl
        ; забрать из очереди AVR накопившиеся байты клавиатуры (не больше PS2_DRAIN)
        call Ps2Open
        ld e,PS2_DRAIN
.drain:
        in a,(c)
        or a
        jr z,.send                     ; очередь пуста
        ld (hl),a
        inc hl
        dec e
        jr nz,.drain
.send:
        ; длина пакета = HL − KeyBuf (or a сбрасывает флаг переноса перед sbc)
        ld de,KeyBuf
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld hl,KeyBuf
        ld a,#75
        call Proto_Send
        ; раз в 32 пакета — ещё и вопрос о состоянии (команда #73)
        ld hl,StatusDivider
        dec (hl)
        ret nz
        ld (hl),32
        ld a,#73
        jp Proto_SendEmpty

; Очередь PS/2 AVR: открыть регистры Gluk, в первый раз — сброс буфера и приём с клавиатуры.
; (Gluk — часы и настройки компьютера Evo; через их порты #DFF7 — номер регистра и #BFF7 —
; значение — AVR отдаёт и байты клавиатуры: регистр #F0 — очередной байт.)
; Выход: BC = #BFF7, выбран #F0 — IN A,(C) даёт очередной байт (0 — пусто). HL, DE не портит.
Ps2Open:
        ld bc,#EFF7
        ld a,#80
        out (c),a
        ld a,(Ps2Ready)
        or a
        jr nz,.queue
        inc a
        ld (Ps2Ready),a
        ld b,#DF
        ld a,#0C
        out (c),a
        ld b,#BF
        ld a,1                         ; сброс буфера
        out (c),a
        ld b,#DF
        ld a,#F0
        out (c),a
        ld b,#BF
        ld a,2                         ; приём с клавиатуры
        out (c),a
.queue:
        ld b,#DF
        ld a,#F0
        out (c),a
        ld b,#BF
        ret

; Прочитать кусок ELF с карты (адрес и длина — в Fx). Быстрое чтение (fastread.asm) — если
; файл лежит на карте одним куском; иначе — через драйвер FAT32. NZ — ошибка.
ReadAt:
        ld a,(FastEnabled)
        or a
        jp nz,FastReadAt
        ld hl,Fx
        call FAT32_FILEX
        ret nz
        ld hl,(Fx+24)
        ld de,(Chunk)
        or a
        sbc hl,de
        ret

FileFindError:
        ld hl,MsgFileRead
        ld (BootMessage),hl
        jp Fail

; Выход по F12: остановить приложение ESP, экран ZX «TSAFARI CLOSED», дальше — сброс.
Stop:
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ld a,RESP_APP_STOP
        ld b,64
        call WaitLong
        call ShowClosed
        xor a
        jr Exit
; Ошибка: показать номер этапа и причину (diagnostic.asm), остановить приложение ESP32, если
; оно уже было запущено; бордюр красный.
Fail:
        ld (BootError),a
        call BootTrace
        ld a,(AppStarted)
        or a
        jr z,.border
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ld a,RESP_APP_STOP
        ld b,64
        call WaitLong
.border:
        call ShowBootError
        ld a,2
Exit:
        out (#FE),a
        ld bc,#00AF                     ; M_SEL = 0: экран снова у TS-Config
        xor a
        out (c),a
Halt:
        jr Halt                         ; дальше — только сброс компьютера
; Ждать ответа A от ESP32: B попыток по 65535 опросов. C — так и не пришёл.
WaitLong:
        ld (WaitWanted),a
.again:
        push bc
        ld a,(WaitWanted)
        ld de,#FFFF
        call Proto_WaitCmd
        pop bc
        ret nc
        djnz .again
        scf
        ret

; --- данные ---
GameDir: dd 0                           ; первый кластер папки браузера на карте
ElfSpec: db 0,"TSAFARI.ELF",0
ElfSpecEnd:
Query: db 0                             ; вопрос к кэшу ESP32: размер, дата, имя
        ds 8
        db "TSAFARI.ELF",0
QueryEnd:
ElfSize: dd 0                           ; размер ELF (32 бита)
Offset: dd 0                            ; сколько уже передано
Chunk: dw 0                             ; длина текущего куска
WaitWanted: db 0
BootStage: db 0
BootError: db 0
AppStarted: db 0
KeyDivider: db 0
StatusDivider: db 32
Ps2Ready: db 0                         ; очередь PS/2 AVR включена
KeyBuf: ds 4+PS2_DRAIN                  ; пакет #75
Fx: db 32,1,1,0                         ; запрос чтения файла для драйвера FAT32 (filex.inc)
        ds 28
PutBuf: ds 1020                         ; кусок ELF: 4 байта смещения + 1016 байт
        INCLUDE "directory.asm"
        INCLUDE "pathcfg.asm"
        INCLUDE "fastread.asm"
        INCLUDE "diagnostic.asm"
        INCLUDE "ftscreen.asm"
        INCLUDE "download.asm"
        INCLUDE "imgcache.asm"
        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"
CodeEnd:
        ASSERT CodeEnd < #BB00
        SAVEBIN "build/tsafari_z80.bin",Start,CodeEnd-Start
