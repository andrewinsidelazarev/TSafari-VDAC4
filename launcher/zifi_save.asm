; Запись загрузки на карту — код ZiFi SPG (VDAC4/ESP32 S3 Zero N8R8/Network/ZiFi SPG/
; fat32_window.asm), проверенный там на модели SD-карты: временный «имя.$$$», подмена FILEX MOVE,
; каталоги zifi/downloads/ГГГГ-ММ-ДД, время файла по часам Evo. Взяты части без разбора имени
; (имя готовит браузер на ESP32). У ZiFi этот код живёт в странице окон #C000; у TSafari — в
; странице программы (#8000…#BFFF), где драйвер и принимает блок параметров FILEX.
; Подробности о том, зачем временный файл, — в launcher/download.asm.
; ---------------------------------------------------------------------------
; Перед записью: временный «имя.$$$» в текущем каталоге (каталоге даты),
; созданный и открытый. Цель — каталог с тем же именем или файл «только для
; чтения» — отказ до создания временного: FILEX MOVE их не заменит. Остаток
; прошлой попытки удаляется, кроме одного случая: подмена прервалась, когда
; запись цели уже смотрела на цепочку временного, а его запись ещё не была
; удалена, — цепочка у них общая, и DELETE освободил бы её из-под цели. Такой
; том не трогаем (FILEX_STATUS_FAT; общую цепочку чинит chkdsk).
; Выход: Z — готово; NZ — код в A.
fat_prepare_target
		call fat_tmp_name
		ld hl,FILE+4
		ld (hl),#10		; [#10][имя] — поиск каталога
		call FAT32_FIND
		ld hl,FILE+4
		ld (hl),0		; [0][имя] — поиск файла
		jr c,fat_fail_code
		ld a,FILEX_STATUS_EXISTS
		ret nz
		ld hl,0
		ld (fat_target_cluster),hl
		ld (fat_target_cluster+2),hl
		ld hl,FILE+4
		call FAT32_FIND
		jr c,fat_fail_code
		jr z,.tmp		; файла ещё нет
		call fat_capture
		ld (fat_target_cluster),hl
		ld (fat_target_cluster+2),de
		ld a,(fat_entry+11)
		and 1
		ld a,FILEX_STATUS_READ_ONLY
		ret nz
.tmp		ld hl,fat_tmp+4
		call FAT32_FIND
		jr c,fat_fail_code
		jr z,.create
		call fat_capture
		ld a,h
		or l
		or d
		or e
		jr z,.delete		; пустой: цепочки нет
		ld bc,(fat_target_cluster)
		or a
		sbc hl,bc
		jr nz,.delete
		ld hl,(fat_target_cluster+2)
		or a
		sbc hl,de
		jr nz,.delete
		ld a,FILEX_STATUS_FAT	; общая цепочка с целью
		or a
		ret
.delete		ld hl,fat_tmp+4
		call FAT32_DELETE
		ret nz
.create		ld hl,fat_tmp
		jp FAT32_CREATE

; После закрытия временного: FILEX MOVE с заменой — временный получает имя цели
; (цели не было — просто имя), затем время по часам. COMMITTED_CLEANUP: перенос
; лёг, а прежняя цепочка цели не освободилась — файл новый и целый, на томе
; лишь потерянные кластеры; это успех.
; Выход: Z — готово; NZ — код в A (у FILEX — статус).
fat_commit_replace
		ld hl,fat_filex_move
		call fat_filex
		jr z,.stamp
		cp FILEX_STATUS_COMMITTED_CLEANUP
		ret nz			; перенос не лёг: прежний файл цел
.stamp		ld hl,FILE+4
		call FAT32_FIND
		jr c,fat_fail_code
		jp nz,fat_stamp_selected
		ld a,FAT32_NO_FILE
		or a
		ret

; CF=1 от FIND: код в A. Выход: NZ с тем же кодом (нулевой — #FF).
fat_fail_code	or a
		ret nz
		ld a,#ff
		or a
		ret

; Запись найденного (после FIND) — в fat_entry: вход 18 (TENTRY) копирует её
; из ENTRY драйвера. Нужны атрибут и первый кластер.
; Выход: HL — младшее слово первого кластера, DE — старшее.
fat_capture	ld de,fat_entry
		call FAT32_CAPTURE_ENTRY
		ld hl,(fat_entry+26)
		ld de,(fat_entry+20)
		ret

; fat_tmp: [атрибут 0][размер 0][FILE_NAME].$$$ — блок CREATE; с +4 это
; [флаг 0][имя][0] для FIND, DELETE и FILEX.
fat_tmp_name	ld hl,FILE_NAME
		ld de,fat_tmp+5
.copy		ld a,(hl)
		or a
		jr z,.suffix
		ld (de),a
		inc hl
		inc de
		jr .copy
.suffix		ld hl,fat_tmp_suffix
		ld bc,fat_tmp_suffix_end-fat_tmp_suffix
		ldir
		ret
fat_tmp_suffix	db ".$$$",0
fat_tmp_suffix_end

; Время выбранной записи (после FIND) по часам Evo — FILEX SET_METADATA:
; создание (с сотыми долями нечётной секунды), доступ, изменение; атрибуты не
; меняются. Выбор файла после этого сброшен (README драйвера): дальше — только
; новый FIND. Выход: A — статус FILEX, Z — записано.
fat_stamp_selected
		call fat_clock_now
		ld (fat_meta+FILEX_META_O_CREATE_TIME),hl
		ld (fat_meta+FILEX_META_O_WRITE_TIME),hl
		ld (fat_meta+FILEX_META_O_CREATE_DATE),de
		ld (fat_meta+FILEX_META_O_ACCESS_DATE),de
		ld (fat_meta+FILEX_META_O_WRITE_DATE),de
		ld a,(fat_rtc_now)
		and 1
		jr z,.tenth
		ld a,100
.tenth		ld (fat_meta+FILEX_META_O_CREATE_TENTH),a
		ld hl,fat_filex_meta

; FILEX по шаблону HL. Блок параметров драйвер принимает только в #8000..#BFFF,
; то есть в странице программы, — шаблон копируется туда (fat_filex_block).
; Выход: A — статус, Z — успех.
fat_filex	ld de,fat_filex_block
		ld bc,FILEX_BLOCK_SIZE
		ldir
		ld hl,fat_filex_block
		jp FAT32_FILEX

fat_filex_move	db FILEX_BLOCK_SIZE,FILEX_API_VERSION,FILEX_OP_MOVE_RENAME
		db FILEX_FLAG_REPLACE|FILEX_FLAG_CURRENT_DIR
		dd 0
		dw fat_tmp+4,NAME_MAX+6		; источник: [0][имя.$$$][0]
		dw FILE+4,NAME_MAX+2		; цель: [0][имя][0]
		dd 0,0,0
		db 0,0
		dw 0
fat_filex_meta	db FILEX_BLOCK_SIZE,FILEX_API_VERSION,FILEX_OP_SET_METADATA,0
		dd 0
		dw fat_meta,FILEX_META_SIZE
		dw 0,0
		dd 0,0,0
		db 0,0
		dw 0
		ASSERT $-fat_filex_move == 2*FILEX_BLOCK_SIZE

fat_meta	db FILEX_META_SIZE
		db 0,0			; маска и значение атрибутов: не менять
		db FILEX_META_TIME_CREATE|FILEX_META_TIME_ACCESS|FILEX_META_TIME_WRITE
		db 0			; сотые доли секунды создания
		dw 0,0			; время и дата создания
		dw 0			; дата доступа
		dw 0,0			; время и дата изменения
		db 0			; итоговые атрибуты (выход)
		ASSERT $-fat_meta == FILEX_META_SIZE

; ---------------------------------------------------------------------------
; Каталог загрузок zifi/downloads/ГГГГ-ММ-ДД от корня; недостающие создаются и
; получают время по часам. Выход: Z — каталог даты стал текущим; NZ — код в A.
fat_download_dir
		call FAT32_SET_ROOT
		jp c,fat_fail_code
		ld hl,DIR_zifi
		call fat_ensure_dir
		ret nz
		ld hl,DIR_download
		call fat_ensure_dir
		ret nz
		ld hl,DIR_date
; Войти в каталог [#10][имя] текущего, при нужде создав его. SET_METADATA
; сбрасывает выбор, поэтому новый каталог после времени ищется заново.
fat_ensure_dir
		push hl
		call FAT32_FIND
		pop hl
		jp c,fat_fail_code
		jp nz,FAT32_SET_DIR
		push hl
		inc hl
		call FAT32_MKDIR
		pop hl
		ret nz
		push hl
		call FAT32_FIND
		pop hl
		jp c,fat_fail_code
		ld a,FAT32_NO_FILE
		jr z,.lost
		push hl
		call fat_stamp_selected
		pop hl
		ret nz
		jr fat_ensure_dir
.lost		or a
		ret

; ---------------------------------------------------------------------------
; Текущие время и дата FAT по часам Evo: HL — время, DE — дата; fat_rtc_now —
; поля (секунды … год от 2000). Как CLOCK_NOW у WC Improved: два снимка подряд
; должны совпасть (часы могли шагнуть посреди чтения), у каждого поля
; проверяются цифры BCD и диапазон, у дня — длина месяца, год — 2026..2099
; (после севшей батарейки часы идут с 2000-го). Двоичный режим (регистр B,
; бит 2) и 12-часовой (бит 1 сброшен: у часов бит 7 — PM, значения 1..12) тоже
; поняты; write_rtc ставит 24-часовой, но при time:none режим остаётся прежним.
; Сбитые часы — дата каталога загрузок DIR_date (её дал NTP, без него — часы
; или 2026-01-01) и 00:00:00.
fat_clock_now
		ld bc,#eff7
		ld a,#80		; доступ к часам, как в write_rtc
		out (c),a
		ld a,#0b
		call aux_rtc_read
		ld c,a
		and 4
		ld (fat_rtc_binary),a
		ld a,c
		and 2
		ld (fat_rtc_24h),a
		ld a,4			; попыток согласованного снимка
.snapshot	ld (fat_rtc_tries),a
		ld hl,fat_rtc_now
		call fat_rtc_fields
		jr c,.broken
		ld hl,fat_rtc_check
		call fat_rtc_fields
		jr c,.broken
		ld hl,fat_rtc_now
		ld de,fat_rtc_check
		ld b,6
.same		ld a,(de)
		cp (hl)
		jr nz,.moved
		inc hl
		inc de
		djnz .same
		call fat_rtc_off
		jr fat_clock_pack
.moved		ld a,(fat_rtc_tries)
		dec a
		jr nz,.snapshot
.broken		call fat_rtc_off
		ld hl,fat_rtc_now
		xor a
		ld (hl),a		; секунды
		inc hl
		ld (hl),a		; минуты
		inc hl
		ld (hl),a		; часы
		inc hl
		ld de,DIR_date+9	; ДД
		call fat_two_digits
		jr c,fat_clock_default
		ld (hl),a
		inc hl
		ld de,DIR_date+6	; ММ
		call fat_two_digits
		jr c,fat_clock_default
		ld (hl),a
		inc hl
		ld de,DIR_date+3	; ГГ из 20ГГ
		call fat_two_digits
		jr c,fat_clock_default
		ld (hl),a
		ld a,(fat_rtc_now+4)
		dec a
		cp 12
		jr nc,fat_clock_default	; месяца нет
		ld a,(fat_rtc_now+3)
		or a
		jr z,fat_clock_default	; дня 0 нет

; fat_rtc_now (месяц 1..12, день от 1) — в HL время и DE дату FAT.
fat_clock_pack
		ld a,(fat_rtc_now+4)
		ld e,a
		ld d,0
		ld hl,fat_month_days-1
		add hl,de
		ld b,(hl)
		cp 2
		jr nz,.length
		ld a,(fat_rtc_now+5)
		and 3
		jr nz,.length
		inc b			; февраль високосного года (до 2099 — каждый четвёртый)
.length		ld a,(fat_rtc_now+3)
		dec a
		cp b
		jr nc,fat_clock_default	; дня нет в месяце
		; дата: (год−1980)<<9 | месяц<<5 | день
		ld a,(fat_rtc_now+4)
		rrca
		rrca
		rrca
		ld c,a			; биты 7..5 — месяц и 7, бит 0 — месяц>>3
		and 1
		ld d,a
		ld a,(fat_rtc_now+5)
		add a,20
		add a,a
		or d
		ld d,a
		ld a,c
		and #e0
		ld e,a
		ld a,(fat_rtc_now+3)
		or e
		ld e,a
		; время: часы<<11 | минуты<<5 | секунды/2
		ld a,(fat_rtc_now+1)
		rrca
		rrca
		rrca
		ld c,a			; биты 7..5 — минуты и 7, биты 2..0 — минуты>>3
		and 7
		ld b,a
		ld a,(fat_rtc_now+2)
		add a,a
		add a,a
		add a,a
		or b
		ld h,a
		ld a,c
		and #e0
		ld c,a
		ld a,(fat_rtc_now)
		srl a
		or c
		ld l,a
		ret

fat_clock_default
		xor a
		ld (fat_rtc_now),a	; секунды: долей секунды создания нет
		ld hl,0
		ld de,((2026-1980)<<9)|(1<<5)|1
		ret

; Шесть полей часов (регистры 0, 2, 4, 7, 8, 9) двоичными значениями в (HL);
; часы — 0..23 и в 12-часовом режиме.
; Выход: CF=1 — поле испорчено (не BCD или вне диапазона).
fat_rtc_fields
		ld de,fat_rtc_table
		ld b,6
.field		push bc
		ld a,(de)		; регистр
		inc de
		ld (fat_rtc_reg),a
		call aux_rtc_read
		ld c,a
		xor a
		ld (fat_rtc_pm),a
		call fat_rtc_12h
		jr z,.mode_ready
		ld a,c			; 12-часовой: бит 7 — PM
		and #80
		ld (fat_rtc_pm),a
		res 7,c
.mode_ready	ld a,(fat_rtc_binary)
		or a
		ld a,c
		jr nz,.value
		and #0f			; BCD: единицы
		cp 10
		jr nc,.bad
		ld b,a
		ld a,c
		and #f0
		cp #a0
		jr nc,.bad
		rrca			; десятки×8
		ld c,a
		rrca
		rrca			; десятки×2
		add a,c
		add a,b
.value		ld c,a
		call fat_rtc_12h
		ld a,c
		jr z,.ranged
		dec a			; 12-часовой: 1..12, где 12 — полночь или полдень
		cp 12
		jr nc,.bad
		inc a
		cp 12
		jr nz,.half
		xor a
.half		ld c,a
		ld a,(fat_rtc_pm)
		or a
		ld a,c
		jr z,.ranged
		add a,12
.ranged		ex de,hl		; HL — таблица, DE — поле
		cp (hl)
		jr c,.bad_swap		; меньше наименьшего
		inc hl
		ld c,a
		ld a,(hl)
		cp c
		jr c,.bad_swap		; больше наибольшего
		inc hl
		ld a,c
		ld (de),a
		inc de
		ex de,hl
		pop bc
		djnz .field
		or a
		ret
.bad_swap	ex de,hl
.bad		pop bc
		scf
		ret

; NZ — читается регистр часов (4), а часы в 12-часовом режиме. Портит A.
fat_rtc_12h	ld a,(fat_rtc_reg)
		cp 4
		jr nz,.no
		ld a,(fat_rtc_24h)
		or a
		jr nz,.no
		inc a			; NZ: часы в 12-часовом режиме
		ret
.no		xor a			; Z
		ret

fat_rtc_off	ld bc,#eff7
		xor a
		out (c),a
		ret

; Две цифры ASCII в (DE) — A, DE += 1. CF=1 — не цифры. HL не портит.
fat_two_digits
		ld a,(de)
		sub "0"
		cp 10
		ccf
		ret c
		ld c,a
		add a,a
		add a,a
		add a,c
		add a,a			; десятки×10
		ld c,a
		inc de
		ld a,(de)
		sub "0"
		cp 10
		ccf
		ret c
		add a,c
		ret

;			регистр, наименьшее, наибольшее
fat_rtc_table	db 0,0,59		; секунды
		db 2,0,59		; минуты
		db 4,0,23		; часы
		db 7,1,31		; день
		db 8,1,12		; месяц
		db 9,26,99		; год от 2000
fat_month_days	db 31,28,31,30,31,30,31,31,30,31,30,31
fat_name_forbidden	db '"*/:<>?\|',0
fat_name_default	db "download",0
fat_name_default_end

fat_rtc_binary	db 0
fat_rtc_24h	db 0		; регистр B, бит 1: ненулевое — 24-часовой режим
fat_rtc_reg	db 0
fat_rtc_pm	db 0
fat_rtc_tries	db 0
fat_rtc_now	ds 6		; секунды, минуты, часы, день, месяц, год от 2000
fat_rtc_check	ds 6
fat_target_cluster	ds 4
fat_entry	ds 32
fat_tmp		db 0		; атрибут
		dw 0,0		; размер; +4 — флаг 0
		ds NAME_MAX+5	; имя, «.$$$» и ноль

; Блок параметров FILEX: драйвер принимает его только в #8000…#BFFF; шаблоны копируются сюда,
; чтобы драйвер не испортил их своими ответами.
fat_filex_block	ds FILEX_BLOCK_SIZE
