# Builds the examples from the SDK archive, the two ways a plugin repository can
# consume it: FetchContent of the archive's URL (hello-cpp) and find_package on
# the unpacked archive (hello-c). Checks the archive's contents and what the
# package lays out and adds (<target>-check, <target>-deploy).
#   tests/sdk/verify-sdk.ps1 -Archive dist/anvil-sdk-windows-x64.zip [-Work work/sdk-verify]
param(
    [Parameter(Mandatory)][string]$Archive,
    [string]$Work = 'sdk-verify'
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
$archive = (Resolve-Path $Archive).Path
Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Work | Out-Null
$work = (Resolve-Path $Work).Path
$examples = Join-Path $root 'examples'

function Run([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}
function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

# 1. The archive's contents.
$sdk = Join-Path $work 'sdk'
Expand-Archive $archive -DestinationPath $sdk
foreach ($file in 'include/anvil/anvil_plugin.h', 'include/anvil/anvil.hpp', 'include/anvil/anvil_symbols.hpp',
                  'include/anvil/anvil_game.h', 'include/anvil/anvil_benchmark.h',
                  'include/anvil/anvil_benchmark.hpp',
                  'bin/anvil.exe', 'cmake/AnvilPluginConfig.cmake', 'cmake/AnvilPluginConfigVersion.cmake',
                  'CMakeLists.txt', 'README.md', 'LICENSE', 'version.txt') {
    Assert (Test-Path (Join-Path $sdk $file)) "the SDK archive lacks $file"
}
Run 'anvil.exe --version' { & (Join-Path $sdk 'bin/anvil.exe') --version }

$hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
$game = Join-Path $work 'game'
$configure = @('-G', 'Visual Studio 17 2022', '-A', 'x64', '-DCMAKE_COMPILE_WARNING_AS_ERROR=ON', "-DEU4_DIR=$game")

# 2. FetchContent of the archive .
$fetch = Join-Path $work 'fetch'
$url = $archive.Replace('\', '/')   # a local path stands for the release URL
Run 'configure (FetchContent)' { cmake -S (Join-Path $root 'tests/sdk') -B $fetch @configure "-DANVIL_SDK_URL=$url" "-DANVIL_SDK_SHA256=$hash" "-DANVIL_EXAMPLES=$examples" }
Run 'build (FetchContent)' { cmake --build $fetch --config Release --parallel }
Run 'hello_cpp-check' { cmake --build $fetch --config Release --target hello_cpp-check }
Run 'hello_cpp-deploy' { cmake --build $fetch --config Release --target hello_cpp-deploy }

# 3. find_package on the unpacked archive.
$pkg = Join-Path $work 'package'
Run 'configure (find_package)' { cmake -S (Join-Path $examples 'hello-c') -B $pkg @configure "-DAnvilPlugin_DIR=$(Join-Path $sdk 'cmake')" }
Run 'build (find_package)' { cmake --build $pkg --config Release --parallel }
Run 'hello_c-check' { cmake --build $pkg --config Release --target hello_c-check }
Run 'hello_c-deploy' { cmake --build $pkg --config Release --target hello_c-deploy }

# 4. The layout: build/plugin/<id>/, and the deployed copy.
foreach ($case in @{ build = $fetch; id = 'hello-cpp'; files = 'hello_cpp.dll', 'settings.toml' },
                  @{ build = $pkg; id = 'hello-c'; files = @('hello_c.dll') }) {
    $folder = Join-Path $case.build "plugin/$($case.id)"
    $names = (Get-ChildItem $folder -File).Name | Sort-Object
    $expected = @($case.files) + @('plugin.toml', 'symbols.toml') | Sort-Object
    Assert (($names -join ',') -eq ($expected -join ',')) "$folder holds $($names -join ', '), expected $($expected -join ', ')"
    $deployed = Join-Path $game "anvil/plugins/$($case.id)"
    foreach ($name in $expected) { Assert (Test-Path (Join-Path $deployed $name)) "$deployed lacks $name" }
}

# 5. The check target without the game reads symbols.lock; a stale lock must fail.
$stale = Join-Path $work 'stale'
Copy-Item (Join-Path $examples 'hello-c') $stale -Recurse
Add-Content (Join-Path $stale 'symbols.toml') "`n# changed"
& (Join-Path $sdk 'bin/anvil.exe') check $stale | Out-Null
Assert ($LASTEXITCODE -eq 1) "anvil check accepted a stale symbols.lock (exit $LASTEXITCODE)"

Write-Host "SDK archive OK: $archive"
exit 0
