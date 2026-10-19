<#
.SYNOPSIS
    用 OpenOCD + CMSIS-DAP 把固件下载到 STM32H750 开发板（下载 → 校验 → 复位 → 运行）。

.DESCRIPTION
    等价于这条命令（把文件名换成本工程的 ELF）：

        openocd -f interface/cmsis-dap.cfg -f target/stm32h7x.cfg ^
                -c "program {build/RelWithDebInfo/STM32synthesizer.elf} verify reset exit"

    方便之处：
      * 不指定 -Elf 时自动挑「最近一次编译出来的」STM32synthesizer.elf
      * -DryRun 只打印将要执行的命令
      * 失败时给出常见原因（DAPLink 没插好 / 速率太高 / 目标被占用）

.PARAMETER Elf
    要下载的 ELF，默认取仓库下最新的 build/**/STM32synthesizer.elf。

.PARAMETER Interface
    OpenOCD 接口配置，默认 interface/cmsis-dap.cfg（WeAct 板载 DAPLink）。
    用 ST-Link 时改成 interface/stlink.cfg。

.PARAMETER Target
    OpenOCD 目标配置，默认 target/stm32h7x.cfg。

.PARAMETER Speed
    SWD 速率（kHz），0 = 用 OpenOCD/调试器默认值。下载不稳定时试 -Speed 1000。

.PARAMETER NoVerify
    跳过 verify（不建议；H7 的 flash 写入偶发失败，verify 能立刻发现）。

.PARAMETER NoReset
    下载完不复位、不运行（只烧写，停下来调试用）。

.PARAMETER DryRun
    只打印命令，不真的执行。

.EXAMPLE
    pwsh tools/flash.ps1
    pwsh tools/flash.ps1 -Elf build/Release/STM32synthesizer.elf -Speed 1000
    pwsh tools/flash.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string]$Elf,
    [string]$Interface = 'interface/cmsis-dap.cfg',
    [string]$Target = 'target/stm32h7x.cfg',
    [int]$Speed = 0,
    [switch]$NoVerify,
    [switch]$NoReset,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------- 找 ELF
if (-not $Elf) {
    $cand = Get-ChildItem -Path $Root -Filter 'STM32synthesizer.elf' -Recurse -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cand) { throw "没找到 STM32synthesizer.elf，先跑 tools\build.ps1（或直接给我 -Elf 路径）。" }
    $Elf = $cand.FullName
}
elseif (-not [System.IO.Path]::IsPathRooted($Elf)) {
    $Elf = Join-Path $Root $Elf
}
if (-not (Test-Path $Elf)) { throw "找不到 ELF：$Elf" }
$Elf = (Resolve-Path $Elf).Path
$elfInfo = Get-Item $Elf

# ---------------------------------------------------------------- 前置检查
$openocdCmd = Get-Command 'openocd' -ErrorAction SilentlyContinue
if (-not $openocdCmd) { throw "PATH 里找不到 openocd。装 OpenOCD 并把 bin 目录加入 PATH。" }
$openocd = $openocdCmd.Source

# OpenOCD 按自己的 scripts\ 目录找 -f 的文件时不会报错，这里提前确认一下
$scriptsDir = Join-Path (Split-Path -Parent $openocd) '..\share\openocd\scripts'
foreach ($cfg in @($Interface, $Target)) {
    if ($cfg -notmatch '[\\/]') { continue }   # 只给了文件名就交给 OpenOCD 自己找
    if (-not (Test-Path (Join-Path $scriptsDir $cfg)) -and -not (Test-Path $cfg)) {
        Write-Host "[warn] 找不到配置文件 $cfg（既不在 $scriptsDir 也不在当前目录），继续尝试。" -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------- 组装命令
# Tcl 里用 {} 包住路径，避免 Windows 反斜杠被当转义符
$elfForTcl = $Elf -replace '\\', '/'
$programCmd = 'program {' + $elfForTcl + '}'
if (-not $NoVerify) { $programCmd += ' verify' }
if (-not $NoReset) { $programCmd += ' reset' }
$programCmd += ' exit'

$openocdArgs = @('-f', $Interface, '-f', $Target)
if ($Speed -gt 0) { $openocdArgs += @('-c', "adapter speed $Speed") }
$openocdArgs += @('-c', $programCmd)

Write-Host ("[flash] {0}  ({1:N0} B, {2})" -f $elfInfo.FullName, $elfInfo.Length, $elfInfo.LastWriteTime) -ForegroundColor Cyan
Write-Host ("[cmd]   {0} {1}" -f $openocd, ($openocdArgs -join ' ')) -ForegroundColor DarkGray

if ($DryRun) { Write-Host '[dry-run] 未执行。' -ForegroundColor Yellow; exit 0 }

# ---------------------------------------------------------------- 执行
& $openocd @openocdArgs
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host ''
    Write-Host "[fail] openocd 退出码 $code。常见原因：" -ForegroundColor Red
    Write-Host '  * DAPLink 没插好 / 驱动没装（设备管理器里应有 CMSIS-DAP 或 USB 串行设备）'
    Write-Host '  * 板子没供电，或 SWD 排线没插'
    Write-Host '  * SWD 速率太高：加 -Speed 1000 再试'
    Write-Host '  * 目标被别的 openocd/gdb 或调试器占着：关掉它们再试'
    Write-Host '  * 之前烧的程序把 SWD 引脚复用掉了：按住复位键执行本脚本，看到 "Examining" 后松手'
}
else {
    Write-Host '[ok] 下载并校验完成，目标已复位运行。' -ForegroundColor Green
}
exit $code
