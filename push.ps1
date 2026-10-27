# push.ps1 -- push one page of text / an image to the e-paper panel
#
# Usage:
#   .\push.ps1 "first line|second line|third line"
#   .\push.ps1 -File notes.txt
#   .\push.ps1 -Image photo.jpg
#   .\push.ps1 "only preview" -Preview out.png          (no hardware needed)
#   .\push.ps1 "bigger" -Size 20 -Center
#   .\push.ps1 -Book book.txt -Page 3
#
# NOTE: ASCII-only on purpose (PowerShell 5.1 reads .ps1 as ANSI/GBK).
param(
    [Parameter(Position = 0, ParameterSetName = 'text')]
    [string]$Text = '',

    [Parameter(ParameterSetName = 'file')]
    [string]$File = '',

    [Parameter(ParameterSetName = 'image')]
    [string]$Image = '',

    [Parameter(ParameterSetName = 'book')]
    [string]$Book = '',

    [int]$Page = 1,
    [ValidateSet('29', '42', '426')]
    [string]$Target = '29',
    [string]$Port = '',
    [int]$Size = 0,
    [string]$Font = '',
    [switch]$Center,
    [switch]$Bold,
    [switch]$NoBorder,
    [switch]$Invert,
    [string]$Preview = '',
    [switch]$NoPush
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvPy = Join-Path $root '.venv\Scripts\python.exe'
$tool = Join-Path $root 'tools\push.py'

if (-not (Test-Path $venvPy)) { Write-Host "venv not found: $venvPy" -ForegroundColor Red; exit 1 }

# no --port given -> pick the first serial port, so nobody has to know COM numbers
if (-not $Port -and -not $NoPush) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 0) {
        Write-Host "No serial port found. Plug the board in, or add -Preview to render only." -ForegroundColor Yellow
        exit 1
    }
    $Port = $ports[0]
}

$argv = @($tool, '--model', $Target)
switch ($PSCmdlet.ParameterSetName) {
    'file'  { $argv += @('--text-file', $File) }
    'image' { $argv += @('--image', $Image) }
    'book'  { $argv += @('--book', $Book, '--page', "$Page") }
    default { $argv += @('--text', $Text) }
}
if ($Size -gt 0) { $argv += @('--size', "$Size") }
if ($Font)       { $argv += @('--font', $Font) }
if ($Center)     { $argv += @('--align', 'center') }
if ($Bold)       { $argv += @('--bold') }
if ($NoBorder)   { $argv += @('--no-border') }
if ($Invert)     { $argv += @('--invert') }
if ($Preview)    { $argv += @('--preview', $Preview) }
if ($Port)       { $argv += @('--port', $Port) }

Write-Host ("push -> {0}  (model {1}, port {2})" -f $root, $Target, $Port) -ForegroundColor Cyan
& $venvPy @argv
exit $LASTEXITCODE
