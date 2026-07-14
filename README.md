# Virtual Microphone

一个 Windows 原生的 WASAPI 音频桥接程序：从指定实体麦克风采集音频，进行固定增益和限幅，再写入已有虚拟音频线路的播放端。

它不使用 Equalizer APO，不修改 `USBAUDIO.sys`，也不创建或安装音频驱动。虚拟音频驱动由用户自行安装；本项目以 VB-CABLE 为例，但任何提供播放端和录制端的虚拟音频线路都可使用。

## 音频路径

```text
实体麦克风 (capture endpoint)
    -> virtual-microphone.exe (可选 RNNoise VST2 -> +gain, hard limiter)
    -> 虚拟线路播放端 (render endpoint，例如 CABLE Input)
    -> 虚拟线路录制端 (例如 CABLE Output)
    -> Discord / OBS / 会议软件
```

## 构建

在 Linux 交叉编译 Windows x64 可执行文件：

```sh
cmake -S . -B build-windows -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build-windows
```

生成文件为 `build-windows/virtual-microphone.exe`。

GitHub Release 提供 `Virtual-Microphone-Latest.zip` 和同名 `.sha256` 校验文件。ZIP 包含程序、同源源码构建的 RNNoise VST2 DLL、示例配置和手动重启脚本。

## 配置与运行

1. 在 Windows 安装一个虚拟音频线路驱动。例如 VB-CABLE 中，`CABLE Input` 是播放设备，`CABLE Output` 是录制设备。

2. 运行以下命令列出 Windows 当前活动端点及其稳定 ID：

```bat
virtual-microphone.exe --list-devices
```

3. 将 `virtual-microphone.ini.example` 复制为 `virtual-microphone.ini`，填写：

```ini
input = {实体麦克风的完整 endpoint ID}
output = CABLE Input
gain_db = 20
```

`input` 应优先使用 `--list-devices` 输出的完整 ID。`output` 可填唯一的设备名称片段；对 VB-CABLE 通常就是 `CABLE Input`。

如需使用 Release 中随包提供的 `rnnoise_mono.dll`，在配置中加入：

```ini
vst_plugin = rnnoise_mono.dll
vad_threshold = 0.6
vad_grace_period = 20
vad_retroactive_grace_period = 0
```

这是 `werman/noise-suppression-for-voice` 的 64 位单声道 VST2 构建产物。程序会在增益前将输入送入 RNNoise，并复制处理后的单声道信号到虚拟线路所需的所有输出声道。使用 VST 时，请将 `CABLE Input` 的默认格式设为 48 kHz；RNNoise 的模型以 48 kHz、480 帧块工作。

4. 将配置文件与 `virtual-microphone.exe` 放在同一目录，运行：

```bat
virtual-microphone.exe
```

5. 在应用中将麦克风选为虚拟线路的录制端：VB-CABLE 中即 `CABLE Output`。如应用遵循 Windows 默认输入设备，也可以将它设为默认录制设备。

## 手动重启

Release 包含 `virtual-microphone-restart.ps1`。将 `virtual-microphone.ini` 按实际设备填写后，在该目录运行：

```powershell
.\virtual-microphone-restart.ps1 -Restart
```

它会等待已有的 `virtual-microphone.exe` 退出，随后以同目录为工作目录启动新实例。脚本会在启动前检查 exe、配置文件和 RNNoise DLL 是否齐全。

## 第三方组件

- [werman/noise-suppression-for-voice](https://github.com/werman/noise-suppression-for-voice)：提供基于 RNNoise 的 VST2 降噪插件。本项目将其源码放在 `vendor/noise-suppression-for-voice`，GitHub Actions 会从该源码构建并打包 `rnnoise_mono.dll`。

`noise-suppression-for-voice` 使用 GPLv3 许可。发布包含其构建产物的包时，应同时遵守 GPLv3 的源码提供与许可声明要求。

## 限制

- 本程序不包含守护、自动重连或异常自动恢复逻辑。
- 输入与输出端点可以使用不同的共享模式格式；程序会完成单声道/多声道映射、16/24/32 位 PCM 或 32 位浮点转换，并以最近邻方式完成采样率转换。正增益会提高底噪；超过满刻度的部分会被硬限幅，避免整数溢出。
- 该程序不能替代虚拟音频驱动；普通 Windows 用户态程序无法自行注册可供所有应用选择的麦克风端点。
