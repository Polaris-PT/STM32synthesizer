<#
.SYNOPSIS
    构建 STM32synthesizer（CubeMX 生成代码 + Synth 引擎 + TinyUSB），并打印各区内存占用。

.DESCRIPTION
    等价于手写的三步 CMake 流程，但把最容易踩的坑固化了下来：
      * 一定带上 -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
        （不带就会用主机编译器，链接阶段才报一堆诡异错误）
      * 每种构建类型一个构建目录：build/<BuildType>（与 CMakePresets.json 一致）
      * 校验 cmake / arm-none-eabi-gcc / ninja 是否在 PATH 上
      * 构建完成后用 arm-none-eabi-size 按内存区域（FLASH / DTCMRAM / AXI SRAM / D2 / D3）
        汇总，直接给出 x KB / y KB (z%)

.PARAMETER BuildType
    Debug | RelWithDebInfo（默认）| Release | MinSizeRel
    RelWithDebInfo = -O2 -g，是日常上板用的；Debug 是 -O0，128 KB flash 会很紧张。

.PARAMETER Generator
    auto（默认）| Ninja | "MinGW Makefiles" | "Unix Makefiles"
    auto：有 ninja 用 Ninja，否则用 MinGW Makefiles。

.PARAMETER BuildDir
    构建目录，默认 <仓库根>/build/<BuildType>。

.PARAMETER Jobs
    并行编译任务数，默认 0 = 交给生成器自己决定。

.PARAMETER Clean
    先删掉构建目录再重新配置（换了生成器 / 换了工具链时用）。

.PARAMETER Flash
    构建成功后直接调用 tools/flash.ps1 下载（需要接上 DAPLink）。

.PARAMETER NoSize
    跳过内存占用汇总。

.PARAMETER Trim
    发布裁剪：关掉调试串口日志（SYNTH_PORT_DEBUG_UART=0），诊断字符串不再进 flash，
    同时 Core/Src 与"冷"端口层文件用 -Os 编译。实测省 ~4.7 KB。
    （LTO 试过能再省 ~4 KB，但会把 HAL 的 __weak MspInit 覆盖顶掉，已放弃 —— 见 CMakeLists.txt 里的记录。）

.EXAMPLE
    pwsh tools/build.ps1
    pwsh tools/build.ps1 -BuildType Debug -Clean
    pwsh tools/build.ps1 -Trim -Flash           # 发布版：flash 最省，但没有串口日志
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$BuildType = 'RelWithDebInfo',

    [string]$Generator = 'auto',
    [string]$BuildDir,
    [int]$Jobs = 0,
    [switch]$Clean,
    [switch]$Flash,
    [switch]$NoSize,
    [switch]$Trim
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- 路径与前置检查
$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Root 'CMakeLists.txt'))) {
    throw "找不到 $Root\CMakeLists.txt —— tools\build.ps1 必须放在仓库的 tools\ 目录下。"
}
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build\$BuildType" }
elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Root $BuildDir }
$ToolchainFile = Join-Path $Root 'cmake\gcc-arm-none-eabi.cmake'
if (-not (Test-Path $ToolchainFile)) { throw "找不到工具链文件：$ToolchainFile" }

function Require-Tool([string]$Name, [string]$Hint) {
    $c = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $c) { throw "PATH 里找不到 $Name。$Hint" }
    return $c.Source
}

$cmake = Require-Tool 'cmake' '安装 CMake 并勾选 "Add to PATH"。'
$null = Require-Tool 'arm-none-eabi-gcc' '安装 Arm GNU Toolchain (arm-none-eabi)，并把 bin 目录加入 PATH。'
$sizeTool = Get-Command 'arm-none-eabi-size' -ErrorAction SilentlyContinue

if ($Generator -eq 'auto') {
    if (Get-Command 'ninja' -ErrorAction SilentlyContinue) { $Generator = 'Ninja' }
    elseif (Get-Command 'mingw32-make' -ErrorAction SilentlyContinue) { $Generator = 'MinGW Makefiles' }
    else { $Generator = 'Unix Makefiles' }
}
switch -Wildcard ($Generator) {
    'Ninja' { $null = Require-Tool 'ninja' '安装 Ninja 或改用 -Generator "MinGW Makefiles"。' }
    'MinGW*' { $null = Require-Tool 'mingw32-make' '安装 MinGW-w64（含 mingw32-make）或改用 -Generator Ninja。' }
}

# ---------------------------------------------------------------- 目录准备
# 只在确属本仓库 build\ 下时才允许删除，避免手误传错路径
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $Root 'build'))
$buildFull = [System.IO.Path]::GetFullPath($BuildDir)
if ($Clean -and (Test-Path $buildFull)) {
    if (-not $buildFull.StartsWith($buildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "-Clean 只允许删除 $buildRoot 下的目录，但 BuildDir = $buildFull"
    }
    Write-Host "[clean] 删除 $buildFull" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildFull
}
if (Test-Path (Join-Path $buildFull 'CMakeCache.txt')) {
    # 生成器/工具链变了必须重配，否则 CMake 会直接报错退出
    $cached = Select-String -Path (Join-Path $buildFull 'CMakeCache.txt') -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' |
              Select-Object -First 1
    $cachedGen = if ($cached) { $cached.Matches[0].Groups[1].Value.Trim() } else { '' }
    if ($cachedGen -and $cachedGen -ne $Generator) {
        Write-Host "[clean] 构建目录里的生成器是 '$cachedGen'，本次要用 '$Generator'，重新配置。" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildFull
    }
}

# ---------------------------------------------------------------- 配置 + 构建
$sw = [System.Diagnostics.Stopwatch]::StartNew()
Write-Host "[configure] $Generator / $BuildType -> $buildFull" -ForegroundColor Cyan

# CMAKE_TOOLCHAIN_FILE 只在【第一次配置】时被读取，之后以缓存里的值为准。已经配好的目录再传一次，
# CMake 会打印 "Manually-specified variables were not used by the project: CMAKE_TOOLCHAIN_FILE"
# 这种看起来很吓人的警告 —— 缓存里已经是我们的工具链时就不再传（若缓存里是别的工具链则重配）。
$cacheFile = Join-Path $buildFull 'CMakeCache.txt'
$useToolchainArg = $true
if (Test-Path $cacheFile) {
    $cachedToolchain = Select-String -Path $cacheFile -Pattern '^CMAKE_TOOLCHAIN_FILE:[^=]*=(.+)$' |
                       Select-Object -First 1
    if ($cachedToolchain) {
        $cachedToolchainPath = $cachedToolchain.Matches[0].Groups[1].Value.Trim() -replace '/', '\'
        if ($cachedToolchainPath -ieq ($ToolchainFile -replace '/', '\')) {
            $useToolchainArg = $false
        } else {
            Write-Host "[clean] 构建目录里记的是另一个工具链（$cachedToolchainPath），重新配置。" -ForegroundColor Yellow
            Remove-Item -Recurse -Force $buildFull
        }
    }
}
$configureArgs = @(
    '-S', $Root, '-B', $buildFull, '-G', $Generator,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE'
)
if ($useToolchainArg) { $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" }
# 两个选项每次都显式传（ON/OFF 都传）：只传 ON 的话，上一次 -Trim 的结果会留在缓存里，
# 下次不带 -Trim 构建时仍然是裁剪版，很容易把人搞糊涂。
$configureArgs += "-DSYNTH_RELEASE:BOOL=" + $Trim.IsPresent.ToString().ToUpper()
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败（exit $LASTEXITCODE）" }

Write-Host "[build] cmake --build" -ForegroundColor Cyan
if ($Jobs -gt 0) { & $cmake --build $buildFull --parallel $Jobs }
else { & $cmake --build $buildFull --parallel }
if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }
$sw.Stop()

$elf = Join-Path $buildFull 'STM32synthesizer.elf'
if (-not (Test-Path $elf)) { throw "构建成功但没找到 $elf" }
Write-Host ("[ok] {0}  （{1:N1} s）" -f $elf, $sw.Elapsed.TotalSeconds) -ForegroundColor Green

# ---------------------------------------------------------------- 内存占用
if (-not $NoSize -and $sizeTool) {
    # arm-none-eabi-size --format=sysv 给出每个段的字节数与地址，按地址归到内存区域
    $regions = @(
        [pscustomobject]@{ Name = 'FLASH';     Base = 0x08000000L; Limit = 128KB; File = $true  },
        [pscustomobject]@{ Name = 'DTCMRAM';   Base = 0x20000000L; Limit = 128KB; File = $false },
        [pscustomobject]@{ Name = 'AXI_SRAM';  Base = 0x24000000L; Limit = 512KB; File = $false },
        [pscustomobject]@{ Name = 'SRAM_D2';   Base = 0x30000000L; Limit = 288KB; File = $false },
        [pscustomobject]@{ Name = 'SRAM_D3';   Base = 0x38000000L; Limit = 64KB;  File = $false }
    )
    $used = @{}
    foreach ($r in $regions) { $used[$r.Name] = 0L }

    foreach ($line in (& $sizeTool.Source --format=sysv $elf)) {
        # 形如 "  .text                 79084   134218392"
        if ($line -match '^\s*([.\w]+)\s+(\d+)\s+(\d+)\s*$') {
            $addr = [int64]$Matches[3]
            if ($addr -eq 0) { continue }              # .debug_* / .comment 等不占内存
            $size = [int64]$Matches[2]
            foreach ($r in $regions) {
                if ($addr -ge $r.Base -and $addr -lt ($r.Base + $r.Limit)) {
                    $used[$r.Name] += $size
                    break
                }
            }
        }
    }
    Write-Host ''
    Write-Host ('{0,-10} {1,12} {2,12} {3,8}' -f '区域', '已用', '容量', '占比')
    foreach ($r in $regions) {
        $u = $used[$r.Name]
        if ($u -eq 0 -and -not $r.File) { continue }   # 没用的 RAM 区不打印
        Write-Host ('{0,-10} {1,10:N0} B {2,10:N0} B {3,7:N1}%' -f $r.Name, $u, $r.Limit, (100.0 * $u / $r.Limit))
    }
    Write-Host ''
}

# ---------------------------------------------------------------- 可选：直接下载
if ($Flash) {
    & (Join-Path $PSScriptRoot 'flash.ps1') -Elf $elf
    if ($LASTEXITCODE -ne 0) { throw "下载失败（exit $LASTEXITCODE）" }
}
