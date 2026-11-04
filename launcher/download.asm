; Загрузки: файл, скачанный браузером (TSAFARI.ELF на ESP32), — на карту Evo в
; \zifi\downloads\ГГГГ-ММ-ДД\, как у ZiFi SPG.
;
; ЗАЧЕМ ЭТО ДЕЛАЕТ Z80. ESP32 сам до карты Evo не достаёт: карта подключена к компьютеру ZX
; Evolution, и писать на неё умеет только программа Z80 через драйвер FAT32. Поэтому файл
; лежит в памяти ESP32 (PSRAM), а эта программа забирает его кусками и пишет на карту.
;
; ОБМЕН (все числа — младшим байтом вперёд):
;   E7 [номер16][размер32][имя, 0]   — событие ESP32: «есть файл». Имя уже приведено
;                                      браузером к правилам драйвера (знаки CP866, ≤ 48).
;   76 [номер16][смещение32]         — Z80: «дай кусок с этого места».
;   F6 [номер16][1][смещение32][число16][данные]
;                                    — ESP32: кусок до 246 байт (кадр от ESP32 к Z80 —
;                                      не больше 256 байт, больше теряется на плате).
;                                      [номер16][0]… — отказ (загрузку отменили).
;   77 [номер16][итог][код][ГГГГ-ММ-ДД] — Z80: чем кончилось. Итог: 0 — записано,
;                                      1 — ошибка карты (код драйвера FAT32 или FILEX),
;                                      2 — ESP32 перестал отдавать куски (или отменили),
;                                      3 — «событие принято, начинаю» (сразу после E7: пока
;                                      драйвер ищет место на большой карте, Z80 молчит
;                                      долго, и браузер должен знать, что программа жива).
;   «Номер» — у каждой загрузки свой: ответ на прежнюю загрузку не спутать с новой.
;
; КАК ПИШЕТСЯ ФАЙЛ (код ZiFi SPG, fat32_window.asm, проверенный на модели карты):
;   1) каталоги zifi, downloads и каталог даты — от корня; недостающие создаются;
;   2) файл пишется во временный «имя.$$$»: куски собираются в буфер 16 КБ (страница #20 в
;      окне #C000) и дописываются на карту блоками (FAT32_APPEND);
;   3) когда всё записано, «имя.$$$» одним действием получает настоящее имя (FILEX MOVE с
;      заменой). Поэтому под настоящим именем на карте лежит либо прежний файл, либо новый
;      целиком — недописанного файла с настоящим именем не бывает;
;   4) файл получает время по часам Evo.
; Между блоками программа пересылает браузеру мышь и клавиши (SendKeys), чтобы курсор не
; замирал на время записи.

NAME_MAX        equ 48                  ; самое длинное имя файла (как у ZiFi SPG)
DL_PIECE        equ 246                 ; данных в одном F6
DL_TRIES        equ 3                   ; попыток на кусок
DL_PAGE         equ #20                 ; страница буфера в окне #C000
DL_BUF          equ #C000
DL_BUF_MAX      equ 16384

; Событие E7 пришло в главном цикле (tsafari.asm, Run): записать файл и доложить итог.
ServeDownload:
        ld hl,(ProtoRxLen)
        ld de,7
        or a
        sbc hl,de
        jp c,.ignore                    ; короче [номер, размер, 0] — не наше
        ld hl,(ProtoBuf)
        ld (DlSeq),hl
        ld hl,(ProtoBuf+2)
        ld (DlSize),hl
        ld hl,(ProtoBuf+4)
        ld (DlSize+2),hl
        ld hl,ProtoBuf+6                ; имя: до NAME_MAX знаков и ноль
        ld de,FILE_NAME
        ld b,NAME_MAX
.name:
        ld a,(hl)
        ld (de),a
        or a
        jr z,.named
        inc hl
        inc de
        djnz .name
        xor a
        ld (de),a
.named:
        ld a,(FILE_NAME)
        or a
        jr nz,.place
        ld hl,fat_name_default          ; пустое имя — «download»
        ld de,FILE_NAME
        ld bc,fat_name_default_end-fat_name_default
        ldir
.place:
        xor a
        ld (DlOpen),a
        ld a,3                          ; «принято, начинаю»
        ld c,0
        call DlReport
        ld a,DL_PAGE                    ; буфер — в окне #C000
        ld bc,#13AF
        out (c),a
        call DlDate                     ; DIR_date: по NTP или по часам Evo
        call fat_download_dir           ; каталог даты стал текущим
        jp nz,.card
        ld hl,0                         ; APPEND пишет ровно столько, сколько принято
        ld (FILE+1),hl
        ld (FILE+3),hl                  ; заодно флаг поиска FILE+4 = 0
        call fat_prepare_target         ; «имя.$$$» создан и открыт
        jp nz,.card
        ld a,1
        ld (DlOpen),a
        ld hl,0
        ld (DlAt),hl
        ld (DlAt+2),hl
.block:
        call DlRemaining
        jr z,.close
        ld hl,0
        ld (DlFill),hl
.piece:
        ; буфер — до последнего целого куска; конец файла — тоже конец блока
        call DlRemaining
        jr z,.flush
        ld hl,(DlFill)
        ld de,DL_BUF_MAX-DL_PIECE+1
        or a
        sbc hl,de
        jr nc,.flush
        call DlPull
        jr c,.lost
        jr .piece
.flush:
        ld hl,DL_BUF
        ld bc,(DlFill)
        call FAT32_APPEND
        jr nz,.card
        call SendKeys                   ; браузер не должен замирать на время записи
        jr .block
.close:
        call FAT32_CLOSE
        jr nz,.card
        xor a
        ld (DlOpen),a
        call fat_commit_replace         ; «имя.$$$» → «имя», время файла
        jr nz,.card
        xor a                           ; итог 0, код 0
        ld c,a
        jr .report
.lost:
        ld c,0
        ld a,2
        jr .closing
.card:
        ld c,a                          ; код драйвера; 0 не бывает — тогда #FF
        or a
        jr nz,.card_code
        ld c,#FF
.card_code:
        ld a,1
.closing:
        push af
        push bc
        ld a,(DlOpen)                   ; недописанный «имя.$$$» закрыть: он останется и
        or a                            ; удалится при повторной загрузке того же файла
        call nz,FAT32_CLOSE
        pop bc
        pop af
.report:
        call DlReport
.ignore:
        xor a
        ret

; Доклад 77 [номер][итог A][код C][ГГГГ-ММ-ДД] и ожидание ответа F7 (его содержимое не важно).
DlReport:
        ld (DlResult+2),a
        ld a,c
        ld (DlResult+3),a
        ld hl,(DlSeq)
        ld (DlResult),hl
        ld hl,DIR_date+1
        ld de,DlResult+4
        ld bc,10
        ldir
        ld hl,DlResult
        ld bc,14
        ld a,#77
        call Proto_Send
        ld a,#F7
        ld b,4
        jp WaitLong

; Z — получено всё (DlAt ≥ DlSize, сравнение 32-битных чисел: вычитание младших слов, потом
; старших с переносом). Портит HL, DE.
DlRemaining:
        ld hl,(DlAt)
        ld de,(DlSize)
        or a
        sbc hl,de
        ld hl,(DlAt+2)
        ld de,(DlSize+2)
        sbc hl,de
        jr c,.more                      ; DlAt < DlSize
        xor a
        ret
.more:
        or 1
        ret

; Кусок с DlAt — в буфер (#C000 + DlFill); DlAt и DlFill растут. CF=1 — ESP32 не отдал
; кусок за DL_TRIES попыток или отказал. Ответ с чужим номером или чужим смещением —
; запоздавший ответ на прежний запрос: его пропускаем и ждём свой.
DlPull:
        ld a,DL_TRIES
        ld (DlTries),a
.try:
        ld hl,(DlSeq)                   ; 76 [номер][смещение]
        ld (DlReq),hl
        ld hl,(DlAt)
        ld (DlReq+2),hl
        ld hl,(DlAt+2)
        ld (DlReq+4),hl
        ld hl,DlReq
        ld bc,6
        ld a,#76
        call Proto_Send
.wait:
        ld a,#F6
        ld b,1
        call WaitLong
        jr nc,.got
        ld hl,DlTries                   ; кадр не дошёл — переспросить тот же кусок
        dec (hl)
        jr nz,.try
        scf
        ret
.stale:
        ld hl,DlTries
        dec (hl)
        jr nz,.wait
        scf
        ret
.got:
        ld hl,(ProtoBuf)                ; номер загрузки
        ld de,(DlSeq)
        or a
        sbc hl,de
        jr nz,.stale
        ld a,(ProtoBuf+2)
        cp 1
        jr nz,.refused
        ld hl,ProtoBuf+3                ; смещение — то, что просили
        ld de,DlAt
        ld b,4
.offset:
        ld a,(de)
        cp (hl)
        jr nz,.stale
        inc hl
        inc de
        djnz .offset
        ld bc,(ProtoBuf+7)              ; число: 1…DL_PIECE, и кадр ровно 9 + число байт
        ld a,b
        or c
        jr z,.refused
        ld hl,DL_PIECE
        or a
        sbc hl,bc
        jr c,.refused
        ld hl,(ProtoRxLen)
        ld de,9
        or a
        sbc hl,de
        or a
        sbc hl,bc
        jr nz,.refused
        ld hl,(DlFill)                  ; данные — в буфер
        ld de,DL_BUF
        add hl,de
        ex de,hl
        ld hl,ProtoBuf+9
        push bc
        ldir
        pop bc
        ld hl,(DlFill)
        add hl,bc
        ld (DlFill),hl
        ld hl,(DlAt)                    ; DlAt += число (32 бита)
        add hl,bc
        ld (DlAt),hl
        ld hl,(DlAt+2)
        ld de,0
        adc hl,de
        ld (DlAt+2),hl
        or a
        ret
.refused:
        scf
        ret

; Дата каталога загрузок — в DIR_date («ГГГГ-ММ-ДД»). Сначала точное время у ESP32 (NTP,
; часовой пояс — из zifi.ini, как у ZiFi SPG; один раз за запуск), иначе — часы Evo, а
; сбитые часы — 2026-01-01 (fat_clock_now).
DlDate:
        ld a,(DlDateKnown)
        or a
        ret nz
        ld a,CMD_NET_NTP
        call Proto_SendEmpty
        ld a,RESP_NET_NTP
        ld b,8
        call WaitLong
        jr c,.rtc
        ld hl,(ProtoRxLen)
        ld de,14
        or a
        sbc hl,de
        jr nz,.rtc
        ld a,(ProtoBuf)                 ; «00000000000000» — NTP не ответил
        cp "2"
        jr nz,.rtc
        ld hl,ProtoBuf                  ; ГГГГММДД… → ГГГГ-ММ-ДД
        ld de,DIR_date+1
        ld bc,4
        ldir
        ld a,"-"
        ld (de),a
        inc de
        ldi
        ldi
        ld (de),a
        inc de
        ldi
        ldi
        ld a,1
        ld (DlDateKnown),a
        ret
.rtc:
        call fat_clock_now              ; DE — дата FAT: (год−1980)<<9 | месяц<<5 | день
        ld hl,DIR_date+1
        ld (hl),"2"
        inc hl
        ld (hl),"0"
        inc hl
        ld a,d
        srl a
        sub 20                          ; год − 2000 (часы проверены: 2026…2099)
        call DlTwo
        ld (hl),"-"
        inc hl
        ld a,d                          ; месяц: бит 0 старшего байта и три старших бита
        and 1                           ; младшего
        ld b,a
        ld a,e
        rlca
        rlca
        rlca
        and 7
        bit 0,b
        jr z,.month
        or 8
.month:
        call DlTwo
        ld (hl),"-"
        inc hl
        ld a,e
        and 31
        ; две цифры числа A (0…99) — в (HL), HL += 2
DlTwo:
        ld c,"0"-1
.tens:
        inc c
        sub 10
        jr nc,.tens
        add a,10+"0"
        ld (hl),c
        inc hl
        ld (hl),a
        inc hl
        ret

DlSeq: dw 0                             ; номер загрузки
DlSize: dd 0                            ; размер файла
DlAt: dd 0                              ; сколько уже получено
DlFill: dw 0                            ; сколько в буфере
DlTries: db 0
DlOpen: db 0                            ; «имя.$$$» открыт
DlDateKnown: db 0                       ; дата получена по NTP
DlReq: ds 6                             ; запрос 76
DlResult: ds 14                         ; доклад 77

; Имя файла для драйвера: [атрибут][размер32] перед именем — блок CREATE, с FILE+4 —
; [флаг поиска][имя] для FIND, FILEX и DELETE (как у ZiFi SPG).
FILE: db 0
        dw 0,0
FILE_NAME: ds NAME_MAX
        db 0
DIR_zifi: db #10,"zifi",0
DIR_download: db #10,"downloads",0
DIR_date: db #10,"2026-01-01",0

; Чтение регистра часов Evo (чип Gluk): A — номер регистра, выход: A — значение.
aux_rtc_read:
        ld bc,#dff7
        out (c),a
        ld b,#bf
        in a,(c)
        ret

        INCLUDE "zifi_save.asm"
