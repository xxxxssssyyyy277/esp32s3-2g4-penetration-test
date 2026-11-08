# Build board B firmware (STA + 1.54in ST7789 LCD) from a pure-ASCII mirror.
#
# Why: ESP-IDF v5.1.2 on zh-CN Windows cannot build when the project path
# contains non-ASCII characters. kconfgen decodes sdkconfig/config.env with the
# GBK locale codec, idf.py decodes cmake output with GBK, and the Xtensa GCC /
# ninja binaries use ANSI Win32 file APIs. A path such as D:\<Chinese>\...\board b
# fails with "Failed to determine sizeof(time_t)" or UnicodeDecodeError before
# any of our code is compiled.
#
# This script mirrors the sources (no build dirs, no sdkconfig) into
#   <MirrorRoot>\esp32s3_link_b
# and runs idf.py there. Build artefacts stay in the mirror; edit the real
# project and re-run.
#
# Usage (from any PowerShell):
#   .\tools\build_ascii.ps1 -BuildDir build_b
#   .\tools\build_ascii.ps1 -BuildDir build_b -Action flash -Port COM6
#   .\tools\build_ascii.ps1 -Action build -Role AP -Disp OLED -BuildDir build_a
param(
    [ValidateSet('STA', 'AP')]
    [string]$Role = 'STA',

    [ValidateSet('LCD', 'OLED')]
    [string]$Disp = 'LCD',

    [string]$BuildDir = 'build_b',

    [ValidateSet('build', 'flash', 'clean', 'fullclean', 'monitor')]
    [string]$Action = 'build',

    [string]$Port = '',

    [string]$MirrorRoot = (Join-Path $env:PUBLIC 'esp32_build'),

    # ESP-IDF installation; only needed when idf.py is not on PATH yet.
    [string]$IdfPath = $env:IDF_PATH
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $PSScriptRoot
$mirrorDir = Join-Path $MirrorRoot 'esp32s3_link_b'

if ($mirrorDir -match '[^\x20-\x7E]') {
    throw "mirror path '$mirrorDir' is not ASCII: pass -MirrorRoot C:\esp32_build"
}
if ($projectDir -match '[^\x20-\x7E]') {
    Write-Host "project path is not ASCII, mirroring to $mirrorDir" -ForegroundColor Yellow
}

# 1. Mirror the sources. /MIR keeps the mirror identical to the project,
#    excluding build output and the generated sdkconfig.
$null = New-Item -ItemType Directory -Force -Path $mirrorDir
$robo = robocopy $projectDir $mirrorDir /MIR /XD build_a build_b .git /XF sdkconfig /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}

# 2. Drop the generated sdkconfig so sdkconfig.defaults is authoritative.
$mirrorSdkconfig = Join-Path $mirrorDir 'sdkconfig'
if (Test-Path -LiteralPath $mirrorSdkconfig) {
    Remove-Item -LiteralPath $mirrorSdkconfig -Force
}

# 3. Make sure the ESP-IDF environment is available.
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    if (-not $IdfPath) {
        $default = 'D:\Espressif\frameworks\esp-idf-v5.1.2'
        if (Test-Path -LiteralPath (Join-Path $default 'export.ps1')) {
            $IdfPath = $default
        }
    }
    if (-not $IdfPath) {
        throw "idf.py not found: run this from an 'ESP-IDF PowerShell' prompt or pass -IdfPath."
    }
    . (Join-Path $IdfPath 'export.ps1') *> $null
}

# 4. Build / flash inside the ASCII mirror.
Set-Location -LiteralPath $mirrorDir
$idfArgs = @('-B', $BuildDir, "-DAPP_ROLE=$Role", "-DDISP_KIND=$Disp")
if ($Action -eq 'flash' -and -not $Port) {
    throw "-Port is required when -Action flash"
}
if ($Port -and ($Action -eq 'flash' -or $Action -eq 'monitor')) {
    $idfArgs += @('-p', $Port, '-b', '460800')
}
$idfArgs += @($Action)

$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& idf.py @idfArgs
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap

Set-Location -LiteralPath $projectDir
if ($code -ne 0) {
    throw "idf.py $Action failed with exit code $code (mirror: $mirrorDir)"
}
Write-Host "done: $mirrorDir\$BuildDir" -ForegroundColor Green
