# CH32H417 Logic Analyzer（上位机）

基于 **PulseView 0.5.0 + libsigrok 0.6.0** 二次开发的 CH32H417 逻辑分析仪上位机。

- 16 通道逻辑分析，USB3 下最高 200 MS/s
- 2 通道 ADC（8/10 bit）
- 132 个 Python 协议解码器
- 阈值、触发、固件 IAP 升级、中文界面、热插拔

原始工程仅提供 Windows 版（MSYS2 / MinGW64）。本仓库正在补充 **macOS 原生构建**。

---

## 目录结构

```
.
├── libsigrok/                  # 驱动后端（裁剪为 ch32h417 + demo 两个驱动）
│   └── src/hardware/wch-ch32h417/
│       ├── api.c               # 设备/配置/扫描
│       ├── protocol.c          # 命令、采集、触发
│       └── ch375_wrapper.*     # USB 访问层（Windows: WCH DLL）
├── libsigrokdecode/            # Python 协议解码（decoders/ 下 132 个）
├── pulseview/                  # Qt5 上位机（PulseView 0.5.0 深度改造）
├── macos/                      # macOS 构建脚本与依赖记录
├── tools/                      # 辅助工具（usb_probe 等）
├── attic/                      # 移植前的 .bak 历史备份，不参与构建
├── PORT_MACOS.md               # macOS 移植方案（两阶段路线图）
├── README_CH32H417.md          # 原 Windows 构建说明
├── build.sh / setup_env.sh     # 原 Windows 构建脚本
└── package_installer.sh        # 原 Windows 安装包脚本
```

---

## Windows 构建

见 [README_CH32H417.md](README_CH32H417.md)。简要：

```bash
./setup_env.sh     # 首次：MSYS2 MINGW64 环境
./build.sh         # 编译
cd build_cmake/logicanalyzer_build && ./LogicAnalyzer.exe
```

## macOS 构建

方案与进度见 [PORT_MACOS.md](PORT_MACOS.md)。

```bash
./macos/install_deps.sh   # 首次：Homebrew 依赖（qt@5 / glibmm@2.66 / libusb / ...）
./macos/build.sh          # 构建四个组件 + LogicAnalyzer.app
open build/pulseview/LogicAnalyzer.app
```

当前状态：原生构建、libusb 后端、设备枚举/版本/200MHz 采集、`.app` 打包均已打通；
热插拔代码已接入，IAP 与信号正确性待硬件验证。

验证工具：

```bash
clang -fobjc-arc -framework Foundation -framework IOKit -framework IOUSBHost \
      tools/usb_probe.m -o tools/usb_probe
./tools/usb_probe --version
```

预期输出包含 `1a86:5537`、`bcdUSB=0x0300`、6 个 bulk 端点，以及
`firmware=2 hardware=1`。

---

## 开发约定

- `attic/` 只放历史备份，不参与编译。
- `macos/` 下的依赖脚本会记录安装前后状态，`cleanup_deps.sh` 可撤销。
- 构建产物统一放在 `build/`、`install/`、`dist/`，已被 `.gitignore` 忽略。
- 不要提交 `pulseview/config.h`、`qrc_*.cpp`、`*.qm` 等生成文件。

## License

PulseView: GPL-2.0-or-later；libsigrok / libsigrokdecode: GPL-3.0-or-later。
分发时请遵守相应许可证。
