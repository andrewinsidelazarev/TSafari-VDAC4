; Кэш картинок браузера на карте Evo: папка Cache\Images рядом с TSAFARI.SPG и TSAFARI.ELF
; (в папке программы, GameDir). Картинка, пришедшая из сети, остаётся на карте файлом, и в
; следующий раз браузер берёт её отсюда. Что лежит в кэше, когда что читать, писать и удалять
; (кольцо на 100 МБ), решает браузер — main/imgcache.c; здесь — только работа с картой.
;
; ЗАЧЕМ Z80. ESP32 до карты не достаёт: писать и читать её умеет только эта программа через
; драйвер FAT32 (как и с загрузками, download.asm).
;
; ОБМЕН (числа — младшим байтом вперёд):
;   E8 [операция][номер16][размер32][имя, 0] — событие ESP32. Операции:
;        1 — список файлов папки (создать её, если нет);
;        2 — прочитать файл «имя» и прислать кусками;
;        3 — записать файл «имя» размером «размер»: куски — 76/F6, как у загрузки
;            (download.asm), время файла — по часам Evo;
;        4 — удалить файл «имя».
;        У номера старший бит — 1: браузер так отличает доклады кэша от докладов загрузок.
;   77 [номер16][итог][код][дата] — доклад, как у загрузок: 3 — «принято, начинаю» (сразу),
;        0 — сделано, 1 — ошибка карты (код драйвера), 2 — ESP32 сказал «хватит» или не ответил.
;   78 [номер16][записи…] → F8 [номер16][1 дальше / 0 хватит]
;        — кусок списка. Запись — как её даёт драйвер (NXTETY), без первого кластера:
;          [размер32][дата16][время16][признак][имя, 0].
;   79 [номер16][смещение32][данные до 1016 байт] → F9 [номер16][1 / 0]
;        — кусок файла (кадр к ESP32 — до 1024 байт, обратно — до 256).
;        С прошивкой 0.1.121+ (канал SPI версии 3) — порцией DMA до 8 КБ: запрос
;        [79][длина LE16][номер16][смещение32] (номер и смещение прошивка кладёт перед
;        данными — браузер получает то же тело 79), данные — одной передачей DMA «ОЗУ → SPI»
;        без суммы, как звук плееру VDAC4MP3 (fast_media.asm). Кадрами по 1 КБ с ответом на
;        каждый на плате выходило ~30 КБ/с (журнал 07.11: 1163 КБ за 36,8 с).
; Между кадрами программа пересылает браузеру мышь и клавиши (SendKeys), чтобы курсор не
; замирал.

CACHE_PIECE     equ 1016                ; данных в одном 79
CACHE_FRAME     equ #C000               ; кадр 78/79 — в странице буфера загрузок (DL_PAGE)
CACHE_ENTRY     equ #C400               ; запись каталога от драйвера: 13 байт и имя до 256
CACHE_FILL_MAX  equ 1000                ; записей в кадре списка — не больше стольких байт
CACHE_RAW       equ 8192                ; порция DMA: 16 секторов (больше прошивка не берёт)
CACHE_RAW_BUF   equ #E000               ; порция — в странице DL_PAGE с #E000 (чётный адрес): ниже
                                        ; буфер сектора и FAT (#C000…#C3FF) и карта секторов файла
                                        ; (#C400…#DFFF, fastread.asm)
LINK_F_MEDIA    equ #10                 ; запрос канала: порция DMA без суммы (raw media)
; DMA TS-Config (как в zifi_spi.asm под ZIFI_SPI_DMA — там он только для плагинов WC: номер
; страницы окна знает WC; у нас страница порции своя — DL_PAGE)
CDMA_SADDR      equ #1AAF               ; адрес источника: младший байт, за ним (#1BAF, #1CAF)
                                        ; старший в пределах страницы и номер страницы
CDMA_LEN        equ #26AF               ; слов в блоке минус один
CDMA_CTR        equ #27AF               ; запуск; при чтении бит 7 — DMA занят
CDMA_NUM        equ #28AF               ; блоков минус один
CDMA_RAM_SPI    equ #82                 ; режим «ОЗУ → SPI»
CSPI_WRDMA      equ #03                 ; команды ведомого SPI ESP32: запись DMA,
CSPI_WR_DONE    equ #07                 ; конец записи

; Событие E8 пришло в главном цикле (tsafari.asm, Run): сделать и доложить. Выход: A = 0.
ServeCache:
        ld hl,(ProtoRxLen)
        ld de,8
        or a
        sbc hl,de
        jp c,CacheIgnore                ; короче [операция, номер, размер, 0] — не наше
        ld hl,(ProtoBuf+1)
        ld (DlSeq),hl
        ld hl,(ProtoBuf+3)
        ld (DlSize),hl
        ld hl,(ProtoBuf+5)
        ld (DlSize+2),hl
        ld hl,ProtoBuf+7                ; имя: до NAME_MAX знаков и ноль
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
        ld a,(ProtoBuf)
        cp 3
        jr nz,.other
        ld a,1                          ; запись — как загрузка, но в Cache\Images
        ld (DlToCache),a
        jp DlWrite
.other:
        ld (CacheOp),a
        ld hl,0
        ld (CacheWaits),hl
        ld a,3                          ; «принято, начинаю»
        ld c,0
        call DlReport
        ld a,DL_PAGE                    ; кадры — в окне #C000
        ld bc,#13AF
        out (c),a
        call CacheDir
        jp nz,CacheCard
        ld a,(CacheOp)
        cp 1
        jp z,CacheList
        cp 2
        jp z,CacheRead
        cp 4
        jp z,CacheDelete
        ld a,FAT32_BAD_ARGUMENT         ; неизвестная операция
        jp CacheCard

; Папка Cache\Images в папке программы; недостающие создаются (fat_ensure_dir, zifi_save.asm).
; Выход: Z — она текущая; NZ — код в A.
CacheDir:
        ld hl,GameDir
        call FAT32_SET_DIR_CLUSTER      ; без кода итога (API 10)
        ld hl,DIR_cache
        call fat_ensure_dir
        ret nz
        ld hl,DIR_images
        jp fat_ensure_dir

; Список: записи каталога (драйвер — NXTETY) — кадрами 78 до CACHE_FILL_MAX байт.
CacheList:
        ld hl,0
        ld (CacheFill),hl
.next:
        ld de,CACHE_ENTRY
        call FAT32_NEXT_ENTRY
        jr z,.end                       ; каталог кончился
        ld hl,CACHE_ENTRY+13            ; длина имени с нулём — в BC
        ld bc,0
.len:
        ld a,(hl)
        inc hl
        inc bc
        or a
        jr nz,.len
        ld a,b                          ; длиннее NAME_MAX — не наше, пропустить
        or a
        jr nz,.next
        ld a,c
        cp NAME_MAX+2
        jr nc,.next
        ld hl,9                         ; запись: 9 байт и имя с нулём
        add hl,bc
        ld (CacheRec),hl
        ld de,(CacheFill)
        add hl,de
        ld de,CACHE_FILL_MAX+1
        or a
        sbc hl,de
        jr c,.fits                      ; fill + запись ≤ CACHE_FILL_MAX
        call CacheFlush
        jp c,CacheLost
.fits:
        ld hl,(CacheFill)               ; запись — в кадр после номера
        ld de,CACHE_FRAME+2
        add hl,de
        ex de,hl
        ld hl,CACHE_ENTRY+4             ; без первого кластера
        ld bc,(CacheRec)
        ldir
        ld hl,(CacheFill)
        ld bc,(CacheRec)
        add hl,bc
        ld (CacheFill),hl
        jr .next
.end:
        ld hl,(CacheFill)
        ld a,h
        or l
        jp z,CacheDone
        call CacheFlush
        jp c,CacheLost
        jp CacheDone

; Кадр списка 78 [номер][записи] → F8. CF=1 — ESP32 не ответил или сказал «хватит».
CacheFlush:
        ld hl,(DlSeq)
        ld (CACHE_FRAME),hl
        ld hl,(CacheFill)
        inc hl
        inc hl
        ld b,h
        ld c,l
        ld hl,CACHE_FRAME
        ld a,#78
        call Proto_Send
        ld a,#F8
        ld b,4
        call WaitLong
        ret c
        call CacheAnswer
        ret c
        ld hl,0
        ld (CacheFill),hl
        call SendKeys                   ; браузер не замирает
        or a
        ret

; Чтение: файл — порциями CACHE_RAW через DMA (прошивка 0.1.121+) или кусками CACHE_PIECE
; кадрами 79 (FILEX READ_AT). Порцию ESP не взяла (браузер старый и порции не разрешил) —
; тот же кусок и всё дальше — кадрами.
CacheRead:
        ld hl,FILE+4
        ld (hl),0                       ; [0][имя] — поиск файла
        call FAT32_FIND
        jp c,CacheCard
        jr nz,.found
        ld a,FAT32_NO_FILE
        jp CacheCard
.found:
        ld (DlSize),hl                  ; размер — DE:HL
        ld (DlSize+2),de
        ld hl,0
        ld (DlAt),hl
        ld (DlAt+2),hl
        ld (CacheUnacked),hl
        call CacheProbe                 ; умеет ли прошивка порции DMA (раз за запуск)
        xor a
        ld (FastEnabled),a
        ld a,(CacheRaw)
        cp 1
        jr nz,.piece
        ; Файл — по карте «сектор файла → сектор карты», как TSAFARI.ELF при запуске (fastread.asm):
        ; FILEX драйвера на каждый кусок заново проходит цепочку кластеров от начала файла (на
        ; плате 07.11 порция 8 КБ — ~110 мс, ~70 КБ/с). Карта — до #DFFF: с #E000 — порция.
        ld hl,(#E000-FastExtents)/8
        ld (FastExtMax),hl
        call FastOpen                   ; не вышло (файл сильно раздроблен) — FILEX
.piece:
        call DlRemaining
        jr nz,.more
        call CacheWaitAll               ; конец файла: ответы на все порции
        jp c,CacheLost
        jp CacheDone
.more:
        ld de,CACHE_PIECE               ; кусок: порция DMA или кадр
        ld a,(CacheRaw)
        cp 1
        jr nz,.unit
        ld de,CACHE_RAW
.unit:
        ld hl,(DlSize)                  ; остаток: младшее слово — в BC, старшее — в HL
        push de
        ld de,(DlAt)
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld hl,(DlSize+2)
        ld de,(DlAt+2)
        sbc hl,de
        pop de
        ld a,h
        or l
        jr nz,.full                     ; остаток ≥ 64 КБ
        ld h,d
        ld l,e
        or a
        sbc hl,bc
        jr nc,.len                      ; остаток ≤ куска — он и есть кусок
.full:
        ld b,d
        ld c,e
.len:
        ld (CacheLen),bc
        ld (CacheFx+10),bc              ; FILEX READ_AT: длина,
        ld hl,(DlAt)                    ; смещение (и оно же — в кадр),
        ld (CacheFx+4),hl
        ld (CACHE_FRAME+2),hl
        ld hl,(DlAt+2)
        ld (CacheFx+6),hl
        ld (CACHE_FRAME+4),hl
        ld hl,CACHE_FRAME+6             ; кадром — сразу за номером и смещением,
        ld a,(CacheRaw)
        cp 1
        jr nz,.buffer
        ld hl,CACHE_RAW_BUF             ; порцией — с начала страницы (номер и смещение — в запросе)
.buffer:
        ld (CacheFx+8),hl
        ld a,(FastEnabled)
        or a
        jr z,.filex
        ld hl,CacheFx+4                 ; блок Fx (fastread.asm): смещение, адрес, длина — те же
        ld de,Fx+4
        ld bc,8
        ldir
        xor a                           ; #C002…#C005 (смещение кадра) — в буфере сектора:
        ld (FastCacheValid),a           ; его «последний сектор» больше не верен
        call FastReadAt
        jp nz,CacheCard
        jr .read
.filex:
        ld hl,CacheFx
        call FAT32_FILEX
        jp nz,CacheCard
        ld hl,(CacheFx+24)              ; прочитано — сколько просили
        ld de,(CacheLen)
        or a
        sbc hl,de
        ld a,#FF
        jp nz,CacheCard
.read:
        ld a,(CacheRaw)
        cp 1
        jr nz,.frame
        call CacheRawSend               ; порция — одной передачей DMA
        jr nc,.raw
        call CacheWaitAll               ; не взяла: сначала ответы на прежние порции,
        jp c,CacheLost
        ld a,2                          ; потом дальше кадрами, этот кусок — заново
        ld (CacheRaw),a
        xor a                           ; кадр — в #C006, там буфер сектора карты: только FILEX
        ld (FastEnabled),a
        jp .piece
.raw:
        ; Ответа F9 на порцию не ждём (как плеер: порции подряд): забираем те, что уже пришли, —
        ; среди них может быть «хватит». Очередь ESP сама держит темп: порция не войдёт, пока
        ; браузер не забрал прошлую (CacheRawSend ждёт «готова»).
        ld hl,(CacheUnacked)
        inc hl
        ld (CacheUnacked),hl
        call CacheDrain
        jp c,CacheLost
        jr .advance
.frame:
        ld hl,(DlSeq)                   ; 79 [номер][смещение][данные]
        ld (CACHE_FRAME),hl
        ld hl,(CacheLen)
        ld bc,6
        add hl,bc
        ld b,h
        ld c,l
        ld hl,CACHE_FRAME
        ld a,#79
        call Proto_Send
        ld a,#F9
        ld b,4
        call WaitLong
        jp c,CacheLost
        call CacheAnswer
        jp c,CacheLost
.advance:
        ld hl,(DlAt)                    ; DlAt += длина (32 бита)
        ld bc,(CacheLen)
        add hl,bc
        ld (DlAt),hl
        ld hl,(DlAt+2)
        ld bc,0
        adc hl,bc
        ld (DlAt+2),hl
        call SendKeys
        jp .piece

; Умеет ли прошивка порции DMA в команды приложения: признак и версия канала SPI (байты 61 и
; 62 общего буфера, после байта состояния) — версия 3+ (прошивка 0.1.121). Раз за запуск:
; CacheRaw 0 — не проверяли, 1 — порциями, 2 — кадрами.
CacheProbe:
        ld a,(CacheRaw)
        or a
        ret nz
        ld a,2
        ld (CacheRaw),a
        ld b,LINK_STATE_ADDR
        call SpiLink_OpenRead           ; A — байт состояния
        in a,(c)                        ; признак прошивки
        cp #D7
        jr nz,.close
        in a,(c)                        ; версия канала
        cp 3
        jr c,.close
        ld a,1
        ld (CacheRaw),a
.close:
        jp SpiLink_Close

; Порция CacheLen байт с CACHE_RAW_BUF — в ESP одной передачей DMA (как Fast_Send плеера):
; запрос LINK_F_MEDIA [79][длина][номер][смещение]; ESP отвечает «готова» (LINK_S_PUT_READY),
; когда в её очереди есть место, — ждём не дольше ~1 с; потом DMA «ОЗУ → SPI» секторами
; (ESP отбросит хвост сектора за длиной) и запрос «записала» (LINK_F_PUT_DONE) — итог
; LINK_S_PUT_OK. CF=1 — не взяла (порции этой команде не разрешены, ESP не отвечает).
CacheRawSend:
        ld a,(SpiTxLen)                 ; накопленное в канале — раньше порции
        or a
        jr z,.empty
        call ZiFi_Flush
        ret c
        ld a,(SpiTxLen)
        or a
        scf
        ret nz
.empty:
        ld a,#79                        ; управление: команда, длина, номер, смещение
        ld (SpiTxBuf),a
        ld hl,(CacheLen)
        ld (SpiTxBuf+1),hl
        ld hl,(DlSeq)
        ld (SpiTxBuf+3),hl
        ld hl,(DlAt)
        ld (SpiTxBuf+5),hl
        ld hl,(DlAt+2)
        ld (SpiTxBuf+7),hl
        ld hl,(CacheLen)                ; секторов: (длина − 1) / 512 + 1
        dec hl
        ld a,h
        srl a
        inc a
        ld (CacheSectors),a
        ld hl,600                       ; попыток по ~1,7 мс
        ld (CacheTries),hl
.ask:
        ld a,9                          ; запрос забирает только управление
        ld (SpiTxLen),a
        ld hl,SpiRsp+3
        ld (SpiDest),hl
        xor a
        ld (SpiRoom),a
        ld a,LINK_F_MEDIA
        call SpiLink_Turn
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_READY
        jr nz,.ready
        ld hl,(CacheWaits)              ; замер (итог 77): сколько раз ESP сказала «пока нельзя»
        inc hl
        ld (CacheWaits),hl
        ld hl,(CacheTries)              ; очередь ESP полна — подождать
        dec hl
        ld (CacheTries),hl
        ld a,h
        or l
        scf
        ret z
        ld bc,1000                      ; ~1,7 мс при 14 МГц (прерывания запрещены — без HALT)
.pause:
        dec bc
        ld a,b
        or c
        jr nz,.pause
        jr .ask
.ready:
        ld hl,CACHE_RAW_BUF             ; источник DMA: окно #C000, страница DL_PAGE
        ld bc,CDMA_SADDR
        out (c),l
        inc b
        ld a,h
        and #3F
        out (c),a
        inc b
        ld a,DL_PAGE
        out (c),a
        ld b,high CDMA_LEN
        ld a,255                        ; блок — 256 слов (сектор)
        out (c),a
        ld b,high CDMA_NUM
        ld a,(CacheSectors)
        dec a
        out (c),a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,CSPI_WRDMA
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)            ; пошёл байт адреса
        in a,(ZIFI_SPI_DATA)            ; пошёл пустой байт
        ld bc,CDMA_CTR
        ld a,CDMA_RAM_SPI
        out (c),a
.dma:
        in a,(c)
        rla
        jr c,.dma
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ld a,SPI_SELECT_ESP             ; конец записи — отдельная транзакция
        out (ZIFI_SPI_CFG),a
        ld a,CSPI_WR_DONE
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ld a,LINK_F_PUT_DONE            ; «записала»: ESP ставит порцию в очередь
        call SpiLink_Send
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_OK
        ret nz                          ; CF=0
        scf
        ret

; Удаление файла «имя».
CacheDelete:
        ld hl,FILE+4
        ld (hl),0
        call FAT32_DELETE
        jp nz,CacheCard
        jp CacheDone

; Ответы F9 на порции — без ожидания: забрать те, что уже пришли (прочие кадры пропускаются, как
; в WaitLong). CF=1 — «хватит» или чужой номер.
CacheDrain:
        ld hl,(CacheUnacked)
        ld a,h
        or l
        ret z                           ; ждать нечего; CF=0
        call Proto_Poll                 ; CF=1, A — команда: кадр готов
        jr nc,.none
        cp #F9
        jr nz,CacheDrain
        call CacheAnswer
        ret c
        ld hl,(CacheUnacked)
        dec hl
        ld (CacheUnacked),hl
        jr CacheDrain
.none:
        or a
        ret

; Дождаться ответов F9 на все отданные порции. CF=1 — «хватит», чужой номер или ESP молчит.
CacheWaitAll:
        ld hl,(CacheUnacked)
        ld a,h
        or l
        ret z
        ld a,#F9
        ld b,4
        call WaitLong
        ret c
        call CacheAnswer
        ret c
        ld hl,(CacheUnacked)
        dec hl
        ld (CacheUnacked),hl
        jr CacheWaitAll

; Ответ F8/F9 [номер][1 дальше / 0 хватит]: CF=1 — чужой номер или «хватит».
CacheAnswer:
        ld hl,(ProtoBuf)
        ld de,(DlSeq)
        or a
        sbc hl,de
        scf
        ret nz
        ld a,(ProtoBuf+2)
        cp 1
        ret z                           ; CF=0
        scf
        ret

; Доклады 77: сделано; ESP32 не ответил или сказал «хватит»; ошибка карты (код в A).
CacheDone:
        ld hl,(CacheWaits)              ; код доклада «сделано» — замер: ожидания «готова» (до 255)
        ld a,h
        or a
        ld a,l
        jr z,.code
        ld a,255
.code:
        ld c,a
        xor a
        jr CacheReport
CacheLost:
        ld c,0
        ld a,2
        jr CacheReport
CacheCard:
        ld c,a                          ; код драйвера; 0 не бывает — тогда #FF
        or a
        jr nz,.code
        ld c,#FF
.code:
        ld a,1
CacheReport:
        call DlReport
CacheIgnore:
        xor a
        ret

CacheOp: db 0                           ; операция события E8
CacheFill: dw 0                         ; байт записей в кадре списка
CacheRec: dw 0                          ; длина текущей записи
CacheLen: dw 0                          ; длина текущего куска файла
CacheRaw: db 0                          ; 0 — не проверяли, 1 — порциями DMA, 2 — кадрами
CacheSectors: db 0                      ; секторов в порции DMA
CacheTries: dw 0                        ; сколько ещё спрашивать место в очереди ESP
CacheWaits: dw 0                        ; сколько раз за операцию ESP сказала «пока нельзя»
CacheUnacked: dw 0                      ; порций отдано, ответа F9 на них ещё нет
CacheFx: db FILEX_BLOCK_SIZE,FILEX_API_VERSION,FILEX_OP_READ_AT,0   ; FILEX: чтение по смещению
        ds FILEX_BLOCK_SIZE-4
DIR_cache: db #10,"Cache",0
DIR_images: db #10,"Images",0
