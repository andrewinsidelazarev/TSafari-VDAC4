# Сборка TSafari: TSAFARI.ELF (ESP-IDF, цель elf загрузчика модулей VDAC4), программа Z80
# (sjasmplus + драйвер FAT32) в TSAFARI.SPG, пакет шрифтов TSAFARI.FNT. Результат — dist/TSAFARI.
param([switch]$SkipEsp, [switch]$SkipFonts, [string]$Vdac4='E:\zx\Tang Nano 20k VDAC\VDAC4')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
if (-not $env:IDF_PYTHON_ENV_PATH) { . 'E:\zx\esp\idf-env.ps1' }
$python=Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$tools='E:\zx\esp\pio-venv\Scripts\python.exe'            # Pillow — для шрифтов
New-Item -ItemType Directory -Force dist/TSAFARI, build | Out-Null
if (-not $SkipEsp) {
    & $tools tools/gen_font.py
    if($LASTEXITCODE) { throw 'Font atlas failed' }
    & $tools tools/gen_ui.py
    if($LASTEXITCODE) { throw 'UI assets failed' }
    & $python "$env:IDF_PATH\tools\idf.py" -B build -D IDF_TARGET=esp32s3 -D "VDAC4_ROOT=$Vdac4" reconfigure
    if($LASTEXITCODE) { throw 'ESP configure failed' }
    & cmake --build build --target elf
    if($LASTEXITCODE) { throw 'ESP build failed' }
    Copy-Item build/tsafari.app.elf dist/TSAFARI/TSAFARI.ELF -Force
    & $python "$Vdac4\ESP32 S3 Zero N8R8\Sound\tools\check_module_symbols.py" dist/TSAFARI/TSAFARI.ELF
    if($LASTEXITCODE) { throw 'ELF loader compatibility check failed' }
    & $tools tests/check_elf_pointers.py dist/TSAFARI/TSAFARI.ELF
    if($LASTEXITCODE) { throw 'ELF pointer check failed' }
}
if (-not $SkipFonts) {
    & $tools tools/gen_fontpack.py
    if($LASTEXITCODE) { throw 'Font pack failed' }
}
& $tools tools/gen_launcher_screen.py
if($LASTEXITCODE) { throw 'Launcher screen failed' }
$env:SJASMPLUS='E:\zx\z80\zuma\sjasmplus.exe'
& $python third_party/fat32/build.py
if($LASTEXITCODE) { throw 'FAT32 build failed' }
& $env:SJASMPLUS --nologo --inc=launcher --inc=build --inc=third_party/fat32/include --sym=build/tsafari_z80.sym --lst=build/tsafari_z80.lst launcher/tsafari.asm
if($LASTEXITCODE) { throw 'Z80 build failed' }
& $python tools/package.py
if($LASTEXITCODE) { throw 'SPG packaging failed' }
Write-Host 'Built dist/TSAFARI: TSAFARI.SPG, TSAFARI.ELF, TSAFARI.FNT'
