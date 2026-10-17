# 欧卡 2 前方红绿灯倒计时

独立的 ETS2 Windows x64 插件。驶近路口时读取游戏内前方信号灯的当前灯色与剩余时间，在 HUD 显示倒计时；没有符合条件的信号灯时自动隐藏。无需安装 ETS2LA。

目前适配 **Euro Truck Simulator 2 1.61.1.9 / DirectX 11**。游戏更新后若内存特征失效，自动倒计时会关闭，避免继续显示错误时间。复杂路口可能选到相邻车道的灯，请以游戏画面为准。

## 安装

1. 退出游戏。
2. 下载本仓库 `plugins` 目录中的 `TrafficLightTimer.dll`、`TrafficLightTimer.ini`、`安装插件.ps1`，放在同一文件夹。
3. 在该文件夹运行 PowerShell：`powershell -ExecutionPolicy Bypass -File .\安装插件.ps1`。脚本会查找 Steam 游戏目录、备份原有 DLL，并保留已有配置。如未找到游戏，传入 `-GameDirectory "游戏安装目录"`。
4. 启动游戏，允许 SDK 插件加载。

默认检测距离为 250 米，可在设置面板调整到 100–400 米。**Ctrl+Shift+Backspace** 打开设置，**F11** 显示或隐藏 HUD。详细说明见 [使用说明.md](使用说明.md)。

## 从源码编译

需要 Zig 0.13、Dear ImGui 1.91.9b、MinHook 1.3.4 和 SCS Telemetry SDK 头文件。把依赖分别放入 `third_party/deps/imgui-1.91.9b`、`third_party/deps/minhook-1.3.4`、`third_party/deps/scs-include`，在 PowerShell 中运行 `tools/build_plugin.ps1`；也可通过 `ZIG`、`THIRD` 环境变量指定 Zig 程序和依赖目录。`tools/build_plugin.sh` 可在其他系统交叉编译 Windows DLL。

本仓库包含源码、已编译 DLL、配置和安装脚本。游戏提取数据及旧版固定周期 `.scs` 未上传。第三方许可见 `third_party` 目录；信号灯读取实现参考 [ETS2LA/plugin](https://github.com/ETS2LA/plugin)。
