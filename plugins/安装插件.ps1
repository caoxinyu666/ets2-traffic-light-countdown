param([string]$GameDirectory)

$ErrorActionPreference = 'Stop'

if (-not $GameDirectory) {
    $libraries = @()
    $steam = Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
    if ($steam.SteamPath) {
        $libraries += $steam.SteamPath
        $vdf = Join-Path $steam.SteamPath 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $text = [System.IO.File]::ReadAllText($vdf)
            foreach ($match in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\', '\')
            }
        }
    }
    $libraries += @(
        (Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Steam'),
        (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Steam')
    )
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $candidate = Join-Path $library 'steamapps\common\Euro Truck Simulator 2'
        if (Test-Path -LiteralPath (Join-Path $candidate 'bin\win_x64\eurotrucks2.exe')) {
            $GameDirectory = $candidate
            break
        }
    }
}

if (-not $GameDirectory) {
    throw '未找到欧卡 2。请用 -GameDirectory 参数指定游戏安装目录。'
}
$GameDirectory = (Resolve-Path -LiteralPath $GameDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $GameDirectory 'bin\win_x64\eurotrucks2.exe'))) {
    throw "不是有效的欧卡 2 游戏目录：$GameDirectory"
}
$pluginDir = Join-Path $GameDirectory 'bin\win_x64\plugins'
New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null

foreach ($name in 'TrafficLightTimer.dll', 'TrafficLightTimer.ini') {
    $source = Join-Path $PSScriptRoot $name
    if (-not (Test-Path -LiteralPath $source)) { throw "缺少文件：$source" }
    $target = Join-Path $pluginDir $name
    if ($name -like '*.ini' -and (Test-Path -LiteralPath $target)) {
        Write-Output "保留现有配置：$target"
        continue
    }
    if ($name -like '*.dll' -and (Test-Path -LiteralPath $target)) {
        Copy-Item -LiteralPath $target -Destination "$target.bak" -Force
        Write-Output "已备份旧 DLL 到 $target.bak"
    }
    Copy-Item -LiteralPath $source -Destination $target -Force
    Write-Output "已安装 $name"
}

Write-Output '完成。此插件独立读取游戏信号灯，无需 ETS2LA；没有前方信号灯时 HUD 不显示。'
