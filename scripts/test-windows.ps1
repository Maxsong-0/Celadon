# Windows 上编并跑 celadon_tests（CMake + Visual Studio 2022 生成器，x64）。与 macOS 的 scripts/test-local.sh 对应。
#
# 用法（PowerShell，任意目录）：
#   scripts\test-windows.ps1                                  # 全套，Release
#   scripts\test-windows.ps1 -Only MusicAnalyzer,LegacyFixture
#   scripts\test-windows.ps1 -Config Debug                     # 调试构建：性能检查只打印、不断言
# 需要：Visual Studio 2022（「使用 C++ 的桌面开发」，带 MSVC v143 与 Windows SDK）、CMake 3.22+；
#       JUCE 9.0.2 放在仓库旁边的 ..\JUCE（与 Celadon.jucer 的 ../JUCE/modules 同一份），或用 -JuceDir 指定。
# 构建目录默认 %TEMP%\celadon-build-vs2022（不进代码目录），可用 -BuildDir 覆盖。
# 如果 PowerShell 不让跑脚本：powershell -ExecutionPolicy Bypass -File scripts\test-windows.ps1
param(
    [string[]] $Only = @(),
    [ValidateSet("Release", "Debug")] [string] $Config = "Release",
    [string] $JuceDir = "",
    [string] $BuildDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if ($JuceDir -eq "") { $JuceDir = Join-Path (Split-Path -Parent $root) "JUCE" }
if ($BuildDir -eq "") { $BuildDir = Join-Path $env:TEMP "celadon-build-vs2022" }

if (-not (Test-Path (Join-Path $JuceDir "modules\juce_core"))) {
    Write-Error "找不到 JUCE：$JuceDir（需要 JUCE 9.0.2，或用 -JuceDir 指定）"
}

cmake -S $root -B $BuildDir -G "Visual Studio 17 2022" -A x64 "-DCELADON_JUCE_DIR=$JuceDir"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config $Config --target celadon_tests --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$testArgs = @()
if ($Only.Count -gt 0) { $testArgs = @("--only", ($Only -join ",")) }

& (Join-Path $BuildDir "$Config\celadon_tests.exe") @testArgs
exit $LASTEXITCODE
