# CH32H417 Logic Analyzer（上位机）

基于 **PulseView 0.5.0 + libsigrok 0.6.0** 二次开发的 CH32H417 逻辑分析仪上位机。

- 16 通道逻辑分析，USB3 下最高 200 MS/s
- 2 通道 ADC（8/10 bit）
- 132 个 Python 协议解码器
- 阈值、触发、固件 IAP 升级、中文界面、热插拔
- 原生支持 Windows / macOS / Linux；USB 传输层在 macOS/Linux 上使用 libusb

原始工程仅提供 Windows 版（MSYS2 / MinGW64）。本仓库已补齐 macOS 原生构建、应用图标、
Linux 构建路径和 GitHub Actions 三平台 CI。

---

## 目录结构

```
.
├── libsigrok/                  # 驱动后端（裁剪为 ch32h417 + demo 两个驱动）
│   └── src/hardware/wch-ch32h417/
│       ├── api.c               # 设备/配置/扫描
│       ├── protocol.c          # 命令、采集、触发
│       ├── ch375_wrapper.c     # Windows: WCH DLL 传输层
│       └── ch375_wrapper_libusb.c  # macOS/Linux: libusb 传输层
├── libsigrokdecode/            # Python 协议解码（decoders/ 下 132 个）
├── pulseview/                  # Qt5 上位机（PulseView 0.5.0 深度改造）
├── macos/                      # macOS 构建脚本与依赖记录
├── linux/                      # Linux 构建脚本、udev 规则、桌面文件
├── tools/                      # 辅助工具（usb_probe、capture_test 等）
├── .github/workflows/build.yml # 三平台 CI + tag 自动发布
├── attic/                      # 移植前的 .bak 历史备份，不参与构建
├── PORT_MACOS.md               # macOS 移植记录 / 两阶段路线图
├── README_CH32H417.md          # 原 Windows 构建说明
├── build.sh / setup_env.sh     # Windows（MSYS2）构建脚本
└── package_installer.sh        # 原 Windows 安装包脚本
```

---

## 平台状态

| 平台 | 构建命令 | 产物 | 状态 |
|---|---|---|---|
| Windows | `./build.sh --package`（MSYS2 MINGW64） | `dist/LogicAnalyzer/`（含 DLL/Python/解码器） | 保留原路径，CI 构建 |
| macOS | `./macos/build.sh --bundle` | `build/pulseview/LogicAnalyzer.app`（自包含） | 已实测：枚举/版本/200MHz/ADC/8·16 通道/IAP/热插拔 |
| Linux | `./linux/build.sh` | `dist/LogicAnalyzer-linux-<arch>.tar.gz` | 复用 libusb 后端，CI 构建 |

> Linux 与 macOS 共用 `ch375_wrapper_libusb.c`；Linux 侧额外提供 udev 规则与桌面文件。

---

## Windows 构建

见 [README_CH32H417.md](README_CH32H417.md)。简要：

```bash
./setup_env.sh     # 首次：MSYS2 MINGW64 环境
./build.sh --package
```

## macOS 构建

方案与进度见 [PORT_MACOS.md](PORT_MACOS.md)。

```bash
./macos/install_deps.sh   # 首次：Homebrew 依赖（qt@5 / glibmm@2.66 / libusb / ...）
./macos/build.sh --clean --bundle  # 构建 + 把 Qt/Python/glib 等全部打包进 .app
open build/pulseview/LogicAnalyzer.app
```

也可以直接安装到 `/Applications`：

```bash
ditto build/pulseview/LogicAnalyzer.app /Applications/LogicAnalyzer.app
```

`--bundle` 会把 Qt5、Python.framework、glib/boost/libusb 等依赖复制进
`Contents/Frameworks` 并改写路径；产物约 120MB，运行时不依赖 Homebrew。
构建阶段仍然需要 Homebrew 的 `qt@5`、`python@3.14` 等。

## Linux 构建

依赖、udev 规则和运行方式见 [linux/README.md](linux/README.md)。简要：

```bash
./linux/build.sh --clean
tar -xzf dist/LogicAnalyzer-linux-$(uname -m).tar.gz
./LogicAnalyzer-linux-$(uname -m)/bin/LogicAnalyzer
```

---

## 图标

应用图标由 `pulseview/icons/pulseview.svg` 渲染生成：

- macOS：`pulseview/icons/logicanalyzer.icns`
- Windows：`pulseview/icons/pulseview.ico`（`pulseviewico.rc` 引用）
- Linux：`pulseview/icons/logicanalyzer-256.png`（安装为 hicolor 图标）

---

## CI / 发布

[.github/workflows/build.yml](.github/workflows/build.yml) 支持：

- `workflow_dispatch`：手动触发三平台构建，产物上传为 Actions artifacts；
- `push` tag `v*`：三平台构建完成后自动创建 GitHub Release 并上传压缩包。

macOS 产物是 ad-hoc 签名的自包含 `.app`，未公证；首次打开可能需要右键“打开”。
Windows 产物自包含；Linux 产物需要系统 Qt5/glib 运行库（见 `linux/README.md`）。

---

## 开发约定

- `attic/` 只放历史备份，不参与编译。
- `macos/` 下的依赖脚本会记录安装前后状态，`cleanup_deps.sh` 可撤销。
- 构建产物统一放在 `build/`、`install/`、`dist/`，已被 `.gitignore` 忽略。
- 不要提交 `pulseview/config.h`、`qrc_*.cpp`、`*.qm` 等生成文件。

## License

PulseView: GPL-2.0-or-later；libsigrok / libsigrokdecode: GPL-3.0-or-later。
分发时请遵守相应许可证。
