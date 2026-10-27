# build.ps1 -- compile / upload the e-paper firmware
#
# Usage:
#   .\build.ps1                              # 4.2" + C3 (default)
#   .\build.ps1 -Board s3                    # 4.2" + S3
#   .\build.ps1 -Target 29 -Board s3         # 2.9" + S3
#   .\build.ps1 -Target 29 -Board s3 -Upload -Monitor
#   .\build.ps1 -Target 29 -Board s3 -Clean  # clean before build
#
# NOTE: this file is ASCII-only on purpose.
# Windows PowerShell 5.1 reads .ps1 files as ANSI (GBK on zh-CN Windows)
# when the file has no BOM, so any Chinese text breaks the parser with
# "MissingEndCurlyBrace" style errors. Keeping it ASCII makes the script
# immune to that class of problem no matter which editor rewrites it.
param(
    [ValidateSet('29', '42', '426')]
    [string]$Target = '42',

    [ValidateSet('c3', 's3')]
    [string]$Board = '',

    [switch]$Upload,
    [switch]$Monitor,
    [switch]$Clean,

    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvPy = Join-Path $root '.venv\Scripts\python.exe'

if (-not (Test-Path $venvPy)) {
    Write-Host "Python venv not found: $venvPy" -ForegroundColor Yellow
    Write-Host "Create it once (needs Python >= 3.8):"
    Write-Host '  $py = "C:\Users\LX\AppData\Local\Programs\Python\Python312\python.exe"'
    Write-Host '  & $py -m venv .venv'
    Write-Host '  .\.venv\Scripts\python.exe -m pip install platformio pillow pyserial'
    exit 1
}

# screen + board -> platformio env name
if (-not $Board) {
    $Board = if ($Target -eq '426') { 's3' } else { 'c3' }
}

$envName = switch ("$Target-$Board") {
    '29-c3'  { 'esp32-c3-29' }
    '29-s3'  { 'esp32-s3-29' }
    '42-c3'  { 'esp32-c3' }
    '42-s3'  { 'esp32-s3' }
    '426-s3' { 'esp32-s3-426' }
    default  {
        Write-Host "No such combination: $Target inch + $Board board" -ForegroundColor Yellow
        Write-Host "Available: 29+c3 / 29+s3 / 42+c3 / 42+s3 / 426+s3"
        exit 1
    }
}

$fw = Join-Path $root 'firmware'

if ($Clean) {
    Write-Host "Cleaning $envName ..." -ForegroundColor DarkGray
    & $venvPy -m platformio run -d $fw -e $envName -t clean | Out-Null
}

Write-Host ("Build env {0}  (screen {1}, board {2})" -f $envName, $Target, $Board) -ForegroundColor Cyan

$pioArgs = @('-m', 'platformio', 'run', '-d', $fw, '-e', $envName)
if ($Upload) { $pioArgs += @('-t', 'upload') }
if ($Port)   { $pioArgs += @('--upload-port', $Port) }

& $venvPy @pioArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Monitor) {
    & $venvPy -m platformio device monitor -d $fw -e $envName --baud 115200
}
