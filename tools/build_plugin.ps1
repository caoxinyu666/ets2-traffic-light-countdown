# Build the Windows x64 HUD DLL with Zig 0.13.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$third = if ($env:THIRD) { $env:THIRD } else { Join-Path $root 'third_party\deps' }
$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$imgui = Join-Path $third 'imgui-1.91.9b'
$minhook = Join-Path $third 'minhook-1.3.4'
$scs = Join-Path $third 'scs-include'
$build = Join-Path $root 'plugins\build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$objects = @()
foreach ($unit in @('buffer', 'hook', 'trampoline', 'hde/hde64')) {
    $object = Join-Path $build (($unit -replace '/', '_') + '.o')
    & $zig cc --target=x86_64-windows-gnu -O2 -c (Join-Path $minhook "src/$unit.c") "-I$(Join-Path $minhook 'include')" -o $object
    if ($LASTEXITCODE -ne 0) { throw "MinHook 编译失败：$unit" }
    $objects += $object
}
$output = Join-Path $build 'TrafficLightTimer.dll'
$arguments = @(
    'c++', '--target=x86_64-windows-gnu', '-std=c++17', '-O2', '-Wall', '-Wextra', '-static', '-shared',
    (Join-Path $root 'src\plugin.cpp'), (Join-Path $root 'src\plugin.def'),
    (Join-Path $imgui 'imgui.cpp'), (Join-Path $imgui 'imgui_draw.cpp'),
    (Join-Path $imgui 'imgui_tables.cpp'), (Join-Path $imgui 'imgui_widgets.cpp'),
    (Join-Path $imgui 'backends\imgui_impl_dx11.cpp')
)
$arguments += $objects
$arguments += @(
    "-I$imgui", "-I$(Join-Path $imgui 'backends')", "-I$(Join-Path $minhook 'include')",
    "-I$scs", "-I$(Join-Path $root 'src')", '-ld3d11', '-ldxgi', '-ld3dcompiler_47',
    '-luser32', '-lgdi32', '-limm32', '-o', $output
)
& $zig @arguments
if ($LASTEXITCODE -ne 0) { throw 'HUD DLL 编译失败' }
Copy-Item -LiteralPath $output -Destination (Join-Path $root 'plugins\TrafficLightTimer.dll') -Force
Get-Item (Join-Path $root 'plugins\TrafficLightTimer.dll') | Select-Object FullName, Length
