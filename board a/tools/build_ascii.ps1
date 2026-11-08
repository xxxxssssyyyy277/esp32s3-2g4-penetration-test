# Build board A/B firmware from a mirror directory whose path is pure ASCII.
#
# Why: ESP-IDF v5.1.2 on zh-CN Windows cannot build when the project path
# contains non-ASCII characters. kconfgen decodes sdkconfig/config.env with the
# GBK locale codec, idf.py decodes cmake output with GBK, and the Xtensa GCC /
# ninja binaries use ANSI Win32 file APIs. Any of these fails first on a path
# such as D:\<Chinese>\...\board a.
#
# This script mirrors the sources (no build dirs, no sdkconfig) into
#   <MirrorRoot>\esp32s3_link
# and runs idf.py there. Build artefacts stay in the mirror.
#
# Usage (from an "ESP-IDF PowerShell" or any PowerShell):
#   .\tools\build_ascii.ps1 -Role AP  -Disp OLED -BuildDir build_a
#   .\tools\build_ascii.ps1 -Role STA -Disp LCD  -BuildDir build_b
#   .\tools\build_ascii.ps1 -Role AP  -Disp OLED -BuildDir build_a -Action flash -Port COM3
#   .\tools\build_ascii.ps1 -Role AP  -Disp OLED -BuildDir build_a -Port COM3 -Action monitor
param(
    [ValidateSet('AP', 'STA')]
    [string]$Role = 'AP',

    [ValidateSet('OLED', 'LCD')]
    [string]$Disp = 'OLED',

    [string]$BuildDir = 'build_a',

    # monitor = 只看串口（默认 115200），配合 -Port 使用
    [ValidateSet('build', 'flash', 'monitor', 'clean', 'fullclean')]
    [string]$Action = 'build',

    [string]$Port = '',

    # Override the OLED software-I2C pins (component defaults: SCL = GPIO1, SDA = GPIO2).
    # 0 means "keep the component default".
    [int]$SclPin = 0,
    [int]$SdaPin = 0,

    # Diagnostic build: scan every safe GPIO pair for the OLED (-DOLED_SCAN_ALL=1)
    [switch]$ScanAll,

    [string]$MirrorRoot = (Join-Path $env:PUBLIC 'esp32_build'),

    # ESP-IDF installation; only needed when idf.py is not on PATH yet.
    [string]$IdfPath = $env:IDF_PATH
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $PSScriptRoot
$mirrorDir = Join-Path $MirrorRoot 'esp32s3_link'

if ($mirrorDir -match '[^\x20-\x7E]') {
    throw "mirror path '$mirrorDir' is not ASCII: pass -MirrorRoot C:\esp32_build"
}
if ($projectDir -match '[^\x20-\x7E]') {
    Write-Host "project path is not ASCII, mirroring to $mirrorDir" -ForegroundColor Yellow
}

# 1. Mirror the sources. /MIR keeps the mirror identical to the project,
#    excluding build output and the generated sdkconfig.
$null = New-Item -ItemType Directory -Force -Path $mirrorDir
$robo = robocopy $projectDir $mirrorDir /MIR /XD build_a build_a_scan build_b .git /XF sdkconfig /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}

# 2. Drop the generated sdkconfig so sdkconfig.defaults is authoritative in the
#    mirror too (the project itself does not carry a sdkconfig).
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
if ($SclPin -gt 0) {
    $idfArgs += "-DOLED_SCL_PIN=$SclPin"
}
if ($SdaPin -gt 0) {
    $idfArgs += "-DOLED_SDA_PIN=$SdaPin"
}
# 显式给值，避免上一次诊断构建的 cmake 缓存残留
$idfArgs += if ($ScanAll) { '-DOLED_SCAN_ALL=1' } else { '-DOLED_SCAN_ALL=0' }
if ($Action -eq 'flash') {
    if (-not $Port) {
        throw "-Port is required when -Action flash"
    }
}
# flash / monitor 都带上 -p，省得 idf.py 自己挑端口（工程里 COM 号不固定）
if ($Port -and ($Action -eq 'flash' -or $Action -eq 'monitor')) {
    $idfArgs += @('-p', $Port, '-b', '460800')
}
$idfArgs += @($Action)
# idf.py 会往 stderr 打正常信息（如 "Compiler supported targets: ..."），
# 而本脚本开着 $ErrorActionPreference='Stop'，PowerShell 5.1 会把它当致命错误，
# 所以调用 idf.py 期间临时放宽；真正的失败仍由下面的 $LASTEXITCODE 判断。
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
