<#
.SYNOPSIS
    读回板子上的诊断状态（g_app_status 等），解码成人话 —— 不用串口、不用 IDE。

.DESCRIPTION
    等价于这条命令（地址自动从 ELF 里查，所以不会因为重新编译而失效）：

        openocd -f interface/cmsis-dap.cfg -f target/stm32h7x.cfg ^
                -c "init; halt; mdw 0x240000b4 17; resume; shutdown"

    各字段的含义见 Core/Src/main.c 里的 app_status_t；里面有：
      时钟自检、故障位、synth/USB/音频的初始化结果、USB 是否枚举成功、
      MIDI 收到多少字节、启动次数（判断复位循环）、复位原因、枚举/掉线次数。

.PARAMETER Elf
    从哪个 ELF 里查符号地址，默认取仓库下最新的 build/**/STM32synthesizer.elf。
    ⚠ 必须是「此刻正在板子上跑的那次编译」的 ELF，否则读到的地址是错的。

.PARAMETER Interface
    OpenOCD 接口配置，默认 interface/cmsis-dap.cfg（WeAct 板载 DAPLink）。

.PARAMETER Target
    OpenOCD 目标配置，默认 target/stm32h7x.cfg。

.PARAMETER Speed
    SWD 速率（kHz），0 = 默认值。连不上时试 -Speed 1000。

.PARAMETER NoResume
    读完后让 CPU 停在 halt（默认读完 resume，板子继续跑）。

.EXAMPLE
    pwsh tools/status.ps1
    pwsh tools/status.ps1 -Speed 1000 -NoResume
#>
[CmdletBinding()]
param(
    [string]$Elf,
    [string]$Interface = 'interface/cmsis-dap.cfg',
    [string]$Target = 'target/stm32h7x.cfg',
    [int]$Speed = 0,
    [string]$DumpDir,
    [switch]$NoResume
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------- 找 ELF
if (-not $Elf) {
    $cand = Get-ChildItem -Path $Root -Filter 'STM32synthesizer.elf' -Recurse -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cand) { throw "没找到 STM32synthesizer.elf，先跑 tools\build.ps1（或用 -Elf 指定）。" }
    $Elf = $cand.FullName
}
elseif (-not [System.IO.Path]::IsPathRooted($Elf)) { $Elf = Join-Path $Root $Elf }
if (-not (Test-Path $Elf)) { throw "找不到 ELF：$Elf" }
$Elf = (Resolve-Path $Elf).Path

# ---------------------------------------------------------------- 找符号地址
$nmCmd = Get-Command 'arm-none-eabi-nm' -ErrorAction SilentlyContinue
if (-not $nmCmd) { throw "PATH 里找不到 arm-none-eabi-nm（Arm GNU Toolchain 的 bin 目录）。" }
$openocdCmd = Get-Command 'openocd' -ErrorAction SilentlyContinue
if (-not $openocdCmd) { throw "PATH 里找不到 openocd。" }

$nmOut = & $nmCmd.Source -S $Elf
function Get-Sym {
    param([string]$Name)
    $line = $nmOut | Select-String -Pattern ("\s" + [regex]::Escape($Name) + "$") | Select-Object -First 1
    if (-not $line) { throw "ELF 里找不到符号 $Name（是不是烧的和编译的不是同一份？）" }
    $m = [regex]::Match($line.Line.Trim(), '^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\S)\s+(\S+)$')
    if (-not $m.Success) { throw "解析 nm 输出失败：$($line.Line)" }
    return @{ addr = [Convert]::ToUInt32($m.Groups[1].Value, 16); size = [Convert]::ToUInt32($m.Groups[2].Value, 16) }
}

$st   = Get-Sym 'g_app_status'
$xrun = Get-Sym 'g_synth_audio_xruns'
$rx   = Get-Sym 'g_synth_midi_rx_bytes'
$words = [int]($st.size / 4)

# g_fault（HardFault 现场，定义在 Core/Src/main.c）：老固件里没有这个符号 → 允许缺失
$flt = $null
try { $flt = Get-Sym 'g_fault' } catch { $flt = $null }
$fltWords = 0
if ($flt) { $fltWords = [int]($flt.size / 4) }

# ---------------------------------------------------------------- 读内存
# ⚠ 不能只靠 openocd 的 stdout：实测（2026-10-18）用 `-c "init; halt; mdw …; shutdown"` 时，
#    mdw 的结果没有进 PowerShell 的捕获（横幅/Info 这些走 stderr 的都在，走 stdout 的命令输出丢了）。
#    所以改成让 openocd 自己把内存 dump 成二进制文件，再用 BitConverter 解码 —— 不看文本输出。
#    仍然照发一遍 mdw：万一在别的机器上 stdout 能抓到，就当作兜底。
$tmpDir = if ($DumpDir) { $DumpDir } else { Join-Path ([System.IO.Path]::GetTempPath()) ("synth_status_{0}" -f $PID) }
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$binSt = Join-Path $tmpDir 'st.bin'
$binXr = Join-Path $tmpDir 'xr.bin'
$binRx = Join-Path $tmpDir 'rx.bin'
$binFl = Join-Path $tmpDir 'flt.bin'
Remove-Item $binSt, $binXr, $binRx, $binFl -ErrorAction SilentlyContinue

# 前后各留 64 字节余量：万一板子上的固件和这个 ELF 不是同一次构建（符号地址会整体偏移），
# 还能靠内容特征（sysclk=480M / hclk=240M / audio_ck=24.576M）把结构体找回来，并把不一致明确报出来。
$winPad   = 0x40
$winStart = [uint32]($st.addr - $winPad)
$winLen   = [int]($st.size + 2 * $winPad)

$tcl = @(
    'init', 'halt',
    ("dump_image {{{0}}} 0x{1:x8} {2}" -f ($binSt -replace '\\', '/'), $winStart, $winLen),
    ("dump_image {{{0}}} 0x{1:x8} 4" -f ($binXr -replace '\\', '/'), $xrun.addr),
    ("dump_image {{{0}}} 0x{1:x8} 4" -f ($binRx -replace '\\', '/'), $rx.addr)
)
if ($flt) {
    $tcl += ("dump_image {{{0}}} 0x{1:x8} {2}" -f ($binFl -replace '\\', '/'), $flt.addr, $flt.size)
}
$tcl += @(
    ("mdw 0x{0:x8} {1}" -f $st.addr, $words),
    ("mdw 0x{0:x8} 1" -f $xrun.addr),
    ("mdw 0x{0:x8} 1" -f $rx.addr)
)
if (-not $NoResume) { $tcl += 'resume' }
$tcl += 'shutdown'

$ocArgs = @('-f', $Interface, '-f', $Target)
if ($Speed -gt 0) { $ocArgs += @('-c', "adapter speed $Speed") }
$ocArgs += @('-c', ($tcl -join '; '))

Write-Host ("[status] {0}" -f (Split-Path -Leaf $Elf)) -ForegroundColor Cyan
Write-Host ("[cmd]    openocd -f {0} -f {1} -c `"… dump_image → {2} …`"" -f $Interface, $Target, $tmpDir) -ForegroundColor DarkGray

# openocd 把横幅/日志写到 stderr；PowerShell 5.1 在 $ErrorActionPreference='Stop' 下
# 会把原生命令的 stderr 当成终止错误（NativeCommandError），所以这里临时放宽再还原。
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & $openocdCmd.Source @ocArgs 2>&1
$ocExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($ocExit -ne 0) {
    Write-Host ''
    Write-Host "[fail] openocd 退出码 $ocExit。常见原因：" -ForegroundColor Red
    Write-Host '  * DAPLink 没插好 / 板子没供电'
    Write-Host '  * SWD 速率太高：加 -Speed 1000'
    Write-Host '  * 目标被别的 openocd/gdb 占着'
    $out | ForEach-Object { Write-Host "  $_" }
    exit $ocExit
}

$v = $null
$skew = 0              # 结构体相对符号地址的偏移（字节）；0 = 完全一致，$null = 没找到特征
if ((Test-Path $binSt) -and (Test-Path $binXr) -and (Test-Path $binRx)) {
    $bst = [System.IO.File]::ReadAllBytes($binSt)
    $winWords = [int]($bst.Length / 4)
    $win = New-Object 'uint32[]' $winWords
    for ($i = 0; $i -lt $winWords; $i++) { $win[$i] = [BitConverter]::ToUInt32($bst, 4 * $i) }

    $idx = -1
    for ($i = 0; $i -le ($winWords - $words); $i++) {
        if ($win[$i] -eq 480000000 -and $win[$i + 1] -eq 240000000 -and $win[$i + 3] -eq 24576000) { $idx = $i; break }
    }
    if ($idx -ge 0) { $skew = [int]($idx * 4 - $winPad) }
    else { $idx = [int]($winPad / 4); $skew = $null }   # 找不到特征 → 按符号地址硬解并警告

    $all = New-Object 'uint32[]' ($words + 2)
    for ($i = 0; $i -lt $words; $i++) { $all[$i] = $win[$idx + $i] }
    $all[$words]     = [BitConverter]::ToUInt32([System.IO.File]::ReadAllBytes($binXr), 0)
    $all[$words + 1] = [BitConverter]::ToUInt32([System.IO.File]::ReadAllBytes($binRx), 0)
    $v = $all
}
else {
    # 兜底：从 openocd 的文本输出里解析 mdw 的结果
    $vals = New-Object System.Collections.Generic.List[uint32]
    foreach ($ln in $out) {
        $m = [regex]::Match([string]$ln, '^\s*0x([0-9a-fA-F]+):\s*((?:[0-9a-fA-F]{1,8}\s*)+)$')
        if ($m.Success) {
            foreach ($w in ($m.Groups[2].Value -split '\s+' | Where-Object { $_ })) {
                $vals.Add([Convert]::ToUInt32($w, 16))
            }
        }
    }
    if ($vals.Count -ge ($words + 2)) {
        $v = $vals.ToArray()
        $skew = $null        # 兜底路径没有余量窗口，无法验证符号地址 → 走"可能不可信"提醒
    }
    else {
        Write-Host '[fail] 两种读法都失败：dump_image 没有生成文件，openocd 的文本输出里也没有 mdw 结果。' -ForegroundColor Red
        Write-Host ("       dump 目录：{0}" -f $tmpDir) -ForegroundColor Red
        Write-Host '       openocd 原始输出：' -ForegroundColor Red
        $out | ForEach-Object { Write-Host "  $_" }
        exit 2
    }
}

# 符号地址自检：板子上跑的固件必须和这个 ELF 是同一次构建，否则读到的全是错位数据。
if ($v) {
    if ($skew -gt 0) {
        Write-Host ''
        Write-Host ("! 板子上的 g_app_status 实际在 0x{0:X8}，比这个 ELF 的符号地址（0x{1:X8}）高 {2} 字节" -f ([uint32]($st.addr + $skew)), $st.addr, $skew) -ForegroundColor Red
        Write-Host '  → 板子上跑的固件和这次构建不是同一份。下面的数值已按内容特征对齐解码（可信），' -ForegroundColor Red
        Write-Host '    但 g_fault / g_synth_* 这些符号仍可能对不上 —— 请 tools\build.ps1 -Flash 重烧后立刻再读。' -ForegroundColor Red
    }
    elseif ($null -eq $skew) {
        Write-Host ''
        Write-Host '! 符号地址附近没找到 g_app_status 的内容特征（sysclk=480000000 / hclk=240000000）——' -ForegroundColor Red
        Write-Host '  下面按符号地址硬解，数值可能不可信：确认板子上跑的就是 tools\build.ps1 -Flash 烧进去的那份。' -ForegroundColor Red
    }
}

# HardFault 现场（可能没有：老固件 / 还没发生过）
$fv = $null
if ($flt -and (Test-Path $binFl)) {
    $bfl = [System.IO.File]::ReadAllBytes($binFl)
    if ($bfl.Length -ge (4 * $fltWords)) {
        $fv = New-Object 'uint32[]' $fltWords
        for ($i = 0; $i -lt $fltWords; $i++) { $fv[$i] = [BitConverter]::ToUInt32($bfl, 4 * $i) }
    }
}
Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- 解码
function Fmt-Hz([uint32]$hz) {
    if ($hz -eq 0) { return '0' }
    return ("{0:N0} Hz ({1:N3} MHz)" -f $hz, ($hz / 1000000.0))
}

# uint32 → int32（PowerShell 5.1 里 [int32]0xFFFFFFFF 会抛异常；0x80000000 又会被解析成负的
# Int32，让 >= 比较恒真 —— 所以用十进制 2147483648，它会被当成 Int64，比较才正确）
function To-I32([uint32]$u) {
    if ($u -ge 2147483648) { return ([int64]$u - 4294967296) }
    return ([int64]$u)
}

# ---- HardFault 现场用的小工具 ----
$addr2lineCmd = Get-Command 'arm-none-eabi-addr2line' -ErrorAction SilentlyContinue
function Resolve-Addr([uint32]$a) {
    if (-not $addr2lineCmd) { return '' }
    if ($a -lt 0x08000000 -or $a -ge 0x08200000) { return '' }   # 只在 Flash 里查（代码段）
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $r = & $addr2lineCmd.Source -f -C -e $Elf ('0x{0:x8}' -f $a) 2>&1
    $ErrorActionPreference = $prev
    if ($r -and $r.Count -ge 2) { return ('{0}  ←  {1}' -f $r[1], $r[0]) }
    return ''
}

$excNames = @{
    2 = 'NMI'; 3 = 'HardFault'; 4 = 'MemManage'; 5 = 'BusFault'; 6 = 'UsageFault'
    11 = 'SVCall'; 14 = 'PendSV'; 15 = 'SysTick'
    27 = 'IRQ11 = DMA1_Stream0（SAI3_B 的音频 TX DMA）'
    117 = 'IRQ101 = OTG_FS（TinyUSB）'
    130 = 'IRQ114 = SAI3'
}
function Get-ExcName([int]$n) {
    if ($n -eq 0) { return '线程模式（主循环 / 普通代码，不在中断里）' }
    if ($excNames.ContainsKey($n)) { return $excNames[$n] }
    if ($n -ge 16) { return ('IRQ{0}（异常号 {1}）' -f ($n - 16), $n) }
    return ('异常号 {0}' -f $n)
}

$cfsrBits = @(
    @{ b = 0;  n = 'IACCVIOL   取指违例（MPU 挡住 / XN 不可执行）' },
    @{ b = 1;  n = 'DACCVIOL   数据访问违例（MPU 权限或 NO_ACCESS 区域）' },
    @{ b = 3;  n = 'MUNSTKERR  异常返回出栈失败' },
    @{ b = 4;  n = 'MSTKERR    异常入栈失败（栈越界/被 MPU 挡住）' },
    @{ b = 5;  n = 'MLSPERR    浮点出栈失败' },
    @{ b = 7;  n = 'MMARVALID  → MMFAR 有效' },
    @{ b = 8;  n = 'IBUSERR    取指总线错误' },
    @{ b = 9;  n = 'PRECISERR  精确总线错误（BFAR 有效，能定位到地址）' },
    @{ b = 10; n = 'IMPRECISERR 不精确总线错误（写缓冲延迟，BFAR 无效）' },
    @{ b = 11; n = 'UNSTKERR   出栈时总线错误' },
    @{ b = 12; n = 'STKERR     入栈时总线错误' },
    @{ b = 13; n = 'LSPERR     浮点出栈总线错误' },
    @{ b = 15; n = 'BFARVALID  → BFAR 有效' },
    @{ b = 16; n = 'UNDEFINSTR 未定义指令' },
    @{ b = 17; n = 'INVSTATE   非法状态（跳转没置 Thumb 位）' },
    @{ b = 18; n = 'INVPC      非法 PC / EXC_RETURN' },
    @{ b = 19; n = 'NOCP       协处理器不可用（FPU 没开）' },
    @{ b = 24; n = 'UNALIGNED  非对齐访问' },
    @{ b = 25; n = 'DIVBYZERO  除零' }
)

$faultNames = @(
    'SAI_INIT   HAL_SAI_InitProtocol() 失败',
    'SAI_CLK    SAI3 内核时钟偏离 24.576 MHz 超过 ±0.1%',
    'SAI_DMA    SAI3_B 没有链上 DMA（hdmatx == NULL）',
    'SYNTH_INIT synth_init() 失败',
    'USB_INIT   tusb_init() 失败',
    'AUDIO_START synth_port_audio_start() 失败',
    'SD_MOUNT   synth_port_sd_mount() 失败',
    'HAL        走到 Error_Handler()（后面全部没执行）',
    'QSPI_MM    QSPI 没能进内存映射（外部采样库不可用）'
)
$rstNames = @(
    @{ b = 17; n = 'CPURSTF' }, @{ b = 19; n = 'D1RSTF' }, @{ b = 20; n = 'D2RSTF' },
    @{ b = 21; n = 'BORRSTF(欠压)' }, @{ b = 22; n = 'PINRSTF(复位键)' }, @{ b = 23; n = 'PORRSTF(上电)' },
    @{ b = 24; n = 'SFTRSTF(软件复位)' }, @{ b = 26; n = 'IWDG1RSTF' }, @{ b = 28; n = 'WWDG1RSTF' },
    @{ b = 30; n = 'LPWRRSTF' }
)

Write-Host ''
Write-Host '地址与原始值' -ForegroundColor White
Write-Host ("  g_app_status          = 0x{0:X8}  ({1} 个 u32 = {2} B)" -f $st.addr, $words, $st.size)
Write-Host ("  g_synth_audio_xruns   = 0x{0:X8}  = {1}" -f $xrun.addr, $v[$words])
Write-Host ("  g_synth_midi_rx_bytes = 0x{0:X8}  = {1}" -f $rx.addr, $v[$words + 1])
Write-Host ("  原始: " + (($v[0..($words - 1)] | ForEach-Object { '0x{0:X8}' -f $_ }) -join ' '))

Write-Host ''
Write-Host 'g_app_status 解码' -ForegroundColor White
Write-Host ("  {0,-18} {1}" -f 'sysclk_hz', (Fmt-Hz $v[0]))
Write-Host ("  {0,-18} {1}" -f 'hclk_hz', (Fmt-Hz $v[1]))
Write-Host ("  {0,-18} {1}" -f 'usb_ck_hz', (Fmt-Hz $v[2]))
Write-Host ("  {0,-18} {1}" -f 'audio_ck_hz', (Fmt-Hz $v[3]))
Write-Host ("  {0,-18} {1}   (期望 2)" -f 'sai_mckdiv', $v[4])
Write-Host ("  {0,-18} 0x{1:X6}   (W25Q64 期望 0xEF4017)" -f 'qspi_id', $v[5])
Write-Host ("  {0,-18} 0x{1:X8}" -f 'fault', $v[6])
for ($i = 0; $i -lt $faultNames.Count; $i++) {
    if ($v[6] -band (1 -shl $i)) { Write-Host ("      ! bit{0}: {1}" -f $i, $faultNames[$i]) -ForegroundColor Yellow }
}
Write-Host ("  {0,-18} {1}" -f 'synth_init', (To-I32 $v[7]))
Write-Host ("  {0,-18} {1}" -f 'usb_init', (To-I32 $v[8]))
    $sdv = To-I32 $v[9]
    $sdText = switch ($sdv) {
        0  { '已挂载并跑过自检' }
        -1 { 'SD 功能没编进来（SYNTH_PORT_SD_ENABLE = 0）' }
        -3 { '卡初始化失败（没插卡 / 接触 / 卡座问题）' }
        1  { 'FR_DISK_ERR：底层读写错（接线/驱动）' }
        3  { 'FR_NOT_READY：卡没准备好' }
        11 { 'FR_INVALID_DRIVE：驱动没登记' }
        12 { 'FR_NOT_ENABLED：逻辑驱动器没挂载' }
        13 { 'FR_NO_FILESYSTEM：卡通了但文件系统不认（多半是 exFAT，本固件只支持 FAT12/16/32）' }
        15 { 'FR_TIMEOUT' }
        default { "FatFs FRESULT = $sdv" }
    }
    Write-Host ("  {0,-18} {1}   ({2})" -f 'sd_mount', $sdv, $sdText)
Write-Host ("  {0,-18} {1}" -f 'audio_start', (To-I32 $v[10]))
Write-Host ("  {0,-18} {1}   (1 = 主机已枚举完成)" -f 'usb_mounted', $v[11])
Write-Host ("  {0,-18} {1}   (弹键就涨)" -f 'midi_rx_bytes', $v[12])
Write-Host ("  {0,-18} {1}   (烧写/按复位键都会 +1；判断复位循环要连读两次看是否增长)" -f 'boot_count', $v[13])
$rst = $v[14]
$rstHit = @()
foreach ($r in $rstNames) { if ($rst -band (1 -shl $r.b)) { $rstHit += $r.n } }
Write-Host ("  {0,-18} 0x{1:X8}   {2}" -f 'reset_flags', $rst, (($rstHit -join ', ')))
Write-Host ("  {0,-18} {1}" -f 'usb_mount_count', $v[15])
Write-Host ("  {0,-18} {1}" -f 'usb_umount_count', $v[16])

# ---- 第 2 轮 USB 诊断字段（索引 17..27）----
$usbIrq   = $v[17]
$usbSel   = $v[18]
$usbClkRc = $v[19]
$gotgctl  = $v[20]
$gintsts  = $v[21]
$dctl     = $v[22]
$dsts     = $v[23]
$gccfg    = $v[24]
$gusbcfg  = $v[25]
$dcfg     = $v[26]
$loops    = $v[27]

Write-Host ''
Write-Host 'USB 寄存器判读' -ForegroundColor White
Write-Host ("  {0,-18} {1}   (1 = PLL1Q，3 = HSI48，0 = 时钟源被关掉)" -f 'usb_sel', $usbSel)
Write-Host ("  {0,-18} 0x{1:X8}   (0 = HAL_OK；0xFFFFFFFF = 还没跑到)" -f 'usb_clk_rc', $usbClkRc)
Write-Host ("  {0,-18} {1}   (插着线恒为 0 → 核没产生中断)" -f 'usb_irq_count', $usbIrq)
Write-Host ("  {0,-18} 0x{1:X8}   BSESVLD(bit19)={2}" -f 'GOTGCTL', $gotgctl, (($gotgctl -shr 19) -band 1))
Write-Host ("  {0,-18} 0x{1:X8}   CURMOD(bit0)={2}  SOF(bit3)={3}  ERLYSUSP(bit10)={4}" -f `
    'GINTSTS', $gintsts, ($gintsts -band 1), (($gintsts -shr 3) -band 1), (($gintsts -shr 10) -band 1))
Write-Host ("  {0,-18}              USBRST(bit12)={0}  ENUMDNE(bit13)={1}  WKUPINT(bit30)={2}" -f `
    (($gintsts -shr 12) -band 1), (($gintsts -shr 13) -band 1), (($gintsts -shr 30) -band 1))
Write-Host ("  {0,-18} 0x{1:X8}   RWUSIG(bit0 软断开)={2}" -f 'DCTL', $dctl, ($dctl -band 1))
Write-Host ("  {0,-18} 0x{1:X8}   SUSPSTS(bit0)={2}  ENUMSPD(bit1-2)={3}  ERLYSUSP(bit3)={4}  SOFFN(bit21-8)={5}" -f `
    'DSTS', $dsts, ($dsts -band 1), (($dsts -shr 1) -band 3), (($dsts -shr 3) -band 1), (($dsts -shr 8) -band 0x3FFF))
Write-Host ("  {0,-18} 0x{1:X8}   PWRDWN(bit16)={2}  VBDEN(bit21)={3}" -f `
    'GCCFG', $gccfg, (($gccfg -shr 16) -band 1), (($gccfg -shr 21) -band 1))
Write-Host ("  {0,-18} 0x{1:X8}   PHYSEL(bit6)={2}" -f 'GUSBCFG', $gusbcfg, (($gusbcfg -shr 6) -band 1))
Write-Host ("  {0,-18} 0x{1:X8}   DSPD(bit0-1)={2}" -f 'DCFG', $dcfg, ($dcfg -band 3))
Write-Host ("  {0,-18} {1}   (不涨 = 主循环卡死)" -f 'loop_count', $loops)

# ---- 存活探针（索引 28..31）----
$systicks = $v[28]
$stage    = $v[29]
$usbDepth = $v[30]
$audDepth = $v[31]
$stageName = switch ($stage) { 1 { 'usb_task' } 2 { 'patch_service' } 3 { 'synth_service' } 0 { '循环其它位置' } default { "未知($stage)" } }

Write-Host ''
Write-Host '存活探针（间隔几秒连读两次才有意义）' -ForegroundColor White
Write-Host ("  {0,-18} {1}   (1 kHz；不涨 = CPU 卡在优先级 0 的中断里，或进了 Error_Handler)" -f 'systick_ticks', $systicks)
Write-Host ("  {0,-18} {1}   ({2})" -f 'main_stage', $stage, $stageName)
Write-Host ("  {0,-18} {1}   (>0 = halt 那一刻正在 USB 中断里)" -f 'usb_isr_depth', $usbDepth)
Write-Host ("  {0,-18} {1}   (>0 = 正在音频 DMA 中断里渲染)" -f 'audio_isr_depth', $audDepth)

# ---- 负载指标（索引 32..34）：判断"复音一多就滋滋/卡死"还剩多少余量 ----
if ($words -ge 35) {
    $cpuPermille = $v[32]
    $peakCycles  = $v[33]
    $underruns   = $v[34]
    $budgetCyc   = 1280000   # 128 帧 @48 kHz = 2.667 ms；480 MHz 下约 1.28M 周期
    $peakPct     = if ($budgetCyc -gt 0) { [math]::Round(100.0 * $peakCycles / $budgetCyc, 1) } else { 0 }

    Write-Host ''
    Write-Host '负载（音频渲染余量）' -ForegroundColor White
    Write-Host ("  {0,-18} {1} ‰   ({2}%)   (1000 = 用满实时预算；>600 就没余量了)" -f `
        'cpu_permille', $cpuPermille, [math]::Round($cpuPermille / 10.0, 1))
    Write-Host ("  {0,-18} {1}   ({2}% 的 {3} 周期预算；接近 100% 必定欠载)" -f `
        'render_peak_cycles', $peakCycles, $peakPct, $budgetCyc)
    Write-Host ("  {0,-18} {1}   (>0 = 主循环没喂上，音频中断重复了上一块)" -f 'audio_underruns', $underruns)
}

# ---- 多音色：16 个通道各自的音色号（索引 35..38，每通道 4 bit；15 = 未知）----
if ($words -ge 39) {
    $patchNames = @('Init Saw','Warm Pad','Bright Lead','Sub Bass','Pluck','Square Keys','Super Saw',
                    'Sine Flute','PWM Strings','Res Sweep','Noise Sweep','Organ',
                    'FM E.Piano','FM Bass','FM Bell')
    $cps = @()
    for ($ch = 0; $ch -lt 16; $ch++) {
        $w = $v[35 + [int][math]::Floor($ch / 4)]
        $p = ($w -shr (4 * ($ch % 4))) -band 0xF
        if ($p -eq 0 -or $p -gt 14) { continue }   # 0 = Init Saw（默认，不列）
        $nm = if ($p -lt $patchNames.Count) { $patchNames[$p] } else { '?' }
        $cps += ("ch{0}=PC{1}({2})" -f $ch, $p, $nm)
    }
    Write-Host ''
    Write-Host '多音色（每通道音色；只列非默认的通道）' -ForegroundColor White
    if ($cps.Count -eq 0) {
        Write-Host '  · 16 个通道都是 0（Init Saw）—— 发 Program Change 后这里会显示各通道音色号'
    } else {
        Write-Host ('  ' + ($cps -join '   '))
    }
}

# ---- SD 卡自检（索引 39 = sd_selftest 位图，40 = sd_exported 本次新写出的文件数）----
if ($words -ge 41) {
    $st = [uint32]$v[39]
    $exp = To-I32 $v[40]
    $bits = @()
    if ($st -band 0x01) { $bits += '目录OK' }
    if ($st -band 0x02) { $bits += '写入OK' }
    if ($st -band 0x04) { $bits += '读回OK' }
    if ($st -band 0x08) { $bits += '解析OK' }
    if ($st -band 0x10) { $bits += '卡上已有文件(未覆盖)' }
    if ($st -band 0x20) { $bits += '加载路径OK' }
    Write-Host ''
    Write-Host 'SD 卡自检（挂载成功后跑一次；新卡首启应为 目录OK/写入OK/读回OK/解析OK，导出 15 个）' -ForegroundColor White
    Write-Host ("  sd_selftest       0x{0:X2}   {1}" -f $st, $(if ($bits.Count) { $bits -join ' / ' } else { '未挂载或全部失败' }))
    Write-Host ("  sd_exported       {0}   (本次新写出 /synth/NNN.syn 的个数；新卡首启 15，之后 0)" -f $exp)
    if ($words -ge 42) {
        $d3 = [uint32]$v[41]
        $d3Text = if ($d3 -eq 1) { '1 = 历史值：曾把 DAT3 当 GPIO 顶高（会和 4-bit 传输抢线，已废弃）' }
                  else { '0 = DAT3 已交回 SDMMC 外设（4-bit 正常，当前设计）' }
        Write-Host ("  sd_dat3           {0}   ({1})" -f $d3, $d3Text)
    }
    if ($words -ge 43) {
        $se = [uint32]$v[42]
        $seBits = @()
        if ($se -band 1)   { $seBits += 'CMD_CRC_FAIL' }
        if ($se -band 2)   { $seBits += 'DATA_CRC_FAIL' }
        if ($se -band 4)   { $seBits += 'CMD_RSP_TIMEOUT(卡不应答)' }
        if ($se -band 8)   { $seBits += 'DATA_TIMEOUT' }
        if ($se -band 16)  { $seBits += 'TX_UNDERRUN' }
        if ($se -band 32)  { $seBits += 'RX_OVERRUN(读数据跟不上/时钟太快)' }
        if ($se -band 64)  { $seBits += 'ADDR_OUT_OF_RANGE' }
        if ($se -band 128) { $seBits += 'REQUEST_NOT_APPLICABLE' }
        if ($se -band 256) { $seBits += 'PARAM' }
        if ($se -band 512) { $seBits += 'UNSUPPORTED_FEATURE' }
        if ($se -band 2048){ $seBits += 'IDMA_ERROR(缓冲区地址/对齐)' }
        $seText = if ($seBits.Count) { $seBits -join ' | ' } else { '无' }
        Write-Host ("  sd_err            0x{0:X}   (hsd1.ErrorCode: {1})" -f $se, $seText)
    }
}

# ---- HardFault 现场（g_fault，由 Core/Src/main.c 的 App_HardFaultRecord() 写入）----
if ($fv -and $fv.Count -ge 20) {
    $fMagic  = $fv[0];  $fCount = $fv[1]; $cfsr = $fv[2]; $hfsr = $fv[3]
    $mmfar   = $fv[4];  $bfar   = $fv[5]; $icsr = $fv[6]; $excRet = $fv[7]
    $fsp     = $fv[8];  $fvalid = $fv[9]
    $fr      = @($fv[10], $fv[11], $fv[12], $fv[13])
    $f12     = $fv[14]; $flr = $fv[15]; $fpc = $fv[16]; $fpsr = $fv[17]
    $fmsp    = $fv[18]; $fpsp = $fv[19]

    Write-Host ''
    Write-Host 'HardFault 现场（g_fault）' -ForegroundColor White
    Write-Host ("  地址 0x{0:X8}（{1} 个 u32）；magic 有效值 = 0x48465A31" -f $flt.addr, $fltWords)

    if ($fMagic -ne 0x48465A31) {
        if ($fMagic -eq 0) {
            Write-Host '  · 没有记录（magic = 0，开机时被清零）→ 本次上电确实没进过 HardFault' -ForegroundColor DarkGray
        }
        else {
            Write-Host ("  · 没有记录（magic = 0x{0:X8}）→ 怪值：板子上跑的固件比这个 ELF 旧（或刚断电）" -f $fMagic) -ForegroundColor DarkGray
            Write-Host '    用 tools\build.ps1 -Flash 烧写后再读，保证「烧进去的固件 = 查符号用的 ELF」。' -ForegroundColor DarkGray
        }
    }
    else {
        Write-Host ("  ! 发生过 HardFault：count = {0}" -f $fCount) -ForegroundColor Red
        Write-Host ("  CFSR   = 0x{0:X8}" -f $cfsr)
        foreach ($b in $cfsrBits) {
            if ($cfsr -band (1 -shl $b.b)) { Write-Host ("      · {0}" -f $b.n) -ForegroundColor Yellow }
        }
        $hhit = @()
        if ($hfsr -band (1 -shl 1))  { $hhit += 'VECTTBL（取向量表时总线错误 → 向量表/中断号有问题）' }
        if ($hfsr -band (1 -shl 30)) { $hhit += 'FORCED（由 MemManage/BusFault/UsageFault 升级而来，看 CFSR）' }
        if ($hfsr -band (1 -shl 31)) { $hhit += 'DEBUGEVT' }
        Write-Host ("  HFSR   = 0x{0:X8}   {1}" -f $hfsr, ($hhit -join '; '))
        if ($cfsr -band (1 -shl 7))  { Write-Host ("  MMFAR  = 0x{0:X8}   ← MemManage 犯规地址" -f $mmfar) -ForegroundColor Yellow }
        if ($cfsr -band (1 -shl 15)) { Write-Host ("  BFAR   = 0x{0:X8}   ← BusFault 犯规地址" -f $bfar) -ForegroundColor Yellow }

        if ($fvalid -ne 0) {
            Write-Host ("  出事时在执行：{0}" -f (Get-ExcName ([int]($fpsr -band 0x1FF))))
            if (($excRet -band 0x4) -ne 0) {
                Write-Host '  栈：PSP → 线程模式（主循环/普通代码里出的事）'
            }
            else {
                Write-Host '  栈：MSP → 中断里出的事（硬件异常栈帧压在 MSP 上）'
            }
            Write-Host ("  PC     = 0x{0:X8}   {1}" -f $fpc, (Resolve-Addr $fpc)) -ForegroundColor Yellow
            Write-Host ("  LR     = 0x{0:X8}   {1}" -f $flr, (Resolve-Addr $flr))
            Write-Host ("  r0-r3  = 0x{0:X8} 0x{1:X8} 0x{2:X8} 0x{3:X8}" -f $fr[0], $fr[1], $fr[2], $fr[3])
            Write-Host ("  r12    = 0x{0:X8}   xPSR = 0x{1:X8}" -f $f12, $fpsr)
            Write-Host ("  栈帧   = 0x{0:X8}   MSP = 0x{1:X8}   PSP = 0x{2:X8}" -f $fsp, $fmsp, $fpsp)
        }
        else {
            Write-Host '  · 没找到硬件栈帧（frame_valid = 0）—— 只能靠 CFSR/HFSR/MMFAR/BFAR 判'
            Write-Host ("  MSP = 0x{0:X8}   PSP = 0x{1:X8}   EXC_RETURN = 0x{2:X8}" -f $fmsp, $fpsp, $excRet)
        }
        $pend = ($icsr -shr 12) -band 0x1FF
        if ($pend -ne 0) { Write-Host ("  出错时挂起中的异常：{0}" -f (Get-ExcName ([int]$pend))) }
        Write-Host '  · 现场不会被软复位清掉（g_fault 在 .ram_section）：复位后再读还是这一份；要清空就断电或重新烧写' -ForegroundColor DarkGray
    }
}

Write-Host ''
Write-Host '结论' -ForegroundColor White
$problems = @()
if ($rst -band (1 -shl 21)) { $problems += '  出现过 BORRSTF（欠压）→ 供电/线材问题（换口、换线、别用 hub）' }
if ($rst -band (1 -shl 26) -or $rst -band (1 -shl 28)) { $problems += '  出现过看门狗复位（IWDG/WWDG）' }
$notes = @()
$notes += '死活判定：间隔 5 秒连读两次 —— systick_ticks 不涨 = CPU 卡在优先级 0 的中断里（或 Error_Handler）；systick 涨但 loop_count 不涨 = 主循环卡在 main_stage 那一步；两者都涨 = 固件活着'
if ($rst -band (1 -shl 24)) { $notes += '上次复位是 SFTRSTF（软件复位）：NVIC_SystemReset()（HardFault 策略 SYNTH_PORT_FAULT_RESET=1 时）和 openocd 的 reset 都会留下这一位，单独出现不算故障' }
# ⚠ boot_count 不能只看数字：烧写（openocd 的 reset）和按复位键都会让它 +1。
#    判断复位循环的正确做法：不烧写、不按复位，连读两次，只有两次之间增长才是循环。
Write-Host ("  · boot_count = {0}：烧写/按复位键都会 +1；判断复位循环要「不烧写不复位连读两次看是否增长」" -f $v[13])
if ($v[6] -ne 0)          { $problems += ("fault = 0x{0:X8}（见上面的位）" -f $v[6]) }
if ((To-I32 $v[8]) -ne 0)   { $problems += 'usb_init != 0：tusb_init() 失败' }
if ((To-I32 $v[10]) -ne 0)  { $problems += 'audio_start != 0：音频 DMA 没起来' }
if ((To-I32 $v[7]) -ne 0)   { $problems += 'synth_init != 0：引擎初始化失败' }
if ($loops -eq 0)         { $problems += 'loop_count = 0：主循环没跑起来' }

# ---- USB 专项判读 ----
if ($v[2] -ne 48000000)   { $problems += ("usb_ck_hz = {0}（应为 48000000）" -f $v[2]) }
if ($usbSel -ne 1)        { $problems += "usb_sel = $usbSel（应为 1 = PLL1Q）→ USB 内核时钟源没选上" }
if ($usbClkRc -ne 0 -and $usbClkRc -ne 4294967295) { $problems += ("HAL_RCCEx_PeriphCLKConfig 返回 0x{0:X8}（非 0 = 失败）" -f $usbClkRc) }
if ((($gccfg -shr 16) -band 1) -eq 0) { $problems += 'GCCFG.PWRDWN = 0：内部 FS 收发器被断电（TinyUSB 应置 1）' }
if ($gintsts -band 1)     { $problems += 'GINTSTS.CURMOD = 1：核跑在 host 模式（应为 device）' }
if ($dctl -band 1)        { $problems += 'DCTL.RWUSIG = 1：软断开中 → DP 上拉没接上，主机看不到设备' }
if ((($gotgctl -shr 19) -band 1) -eq 0) { $problems += 'GOTGCTL.BSESVLD = 0：核没识别到 VBUS（换 USB 口/线，或主机口没供电）' }
if ($dsts -band 1)        { $problems += 'DSTS.SUSPSTS = 1：总线挂起（主机还没开始枚举 / 线只供电不通讯）' }
if ($usbIrq -eq 0 -and $v[11] -eq 0) {
    $problems += 'usb_irq_count = 0：DWC2 核从未产生中断（内核时钟 / PHY 供电 / DP 上拉 / 中断使能）'
}
if ($v[16] -gt 0)         { $problems += "usb_umount_count = $($v[16])：USB 掉过线（插拔、固件复位、脚本 halt 都会计）" }
if ($v[15] -gt 1)         { $problems += "usb_mount_count = $($v[15])：主机枚举过多次（反复重枚举）" }
if ($v[15] -eq 0 -and $usbIrq -gt 0) { $problems += 'usb_mount_count = 0 但中断有：核在跑却没谈成（描述符/端点/主机侧）' }
if ($audDepth -gt 0)      { $problems += 'audio_isr_depth > 0：正卡在音频 DMA 中断的渲染里 —— 该中断优先级 0，会屏蔽 SysTick，现象就是「板子像死机」；连读两次都是 1 = 卡死在渲染里' }
if ($words -ge 35) {
    if ($v[34] -gt 0)  { $problems += ("audio_underruns = {0}：主循环喂不上预渲染环（CPU 忙不过来，或被 SD/阻塞调用卡住）" -f $v[34]) }
    if ($v[32] -gt 700) { $problems += ("cpu_permille = {0}‰：渲染已吃掉 7 成以上预算，复音再多就会断续" -f $v[32]) }
}
if ($usbDepth -gt 0)      { $problems += 'usb_isr_depth > 0：正卡在 USB 中断里（连读两次都是 1 = 卡死在 TinyUSB 中断处理里）' }
if ($fv -and $fv.Count -ge 20 -and $fv[0] -eq 0x48465A31) {
    $problems += ("进过 HardFault（count = {0}）：CFSR = 0x{1:X8}，出事时 PC = 0x{2:X8} —— 详见上面「HardFault 现场」" -f $fv[1], $fv[2], $fv[16])
}

if ($problems.Count -eq 0) {
    Write-Host '  ✓ 固件侧一切正常（无故障位、没有复位循环、USB 已枚举）。' -ForegroundColor Green
    Write-Host '    若 Windows 里仍报错 → 问题在主机侧驱动/端口，不在固件。' -ForegroundColor Green
} else {
    foreach ($p in $problems) { Write-Host "  ! $p" -ForegroundColor Yellow }
}
foreach ($n in $notes) { Write-Host "  · $n" -ForegroundColor DarkGray }
