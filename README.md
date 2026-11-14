# CH32H417 Logic Analyzer（上位机）

基于 **PulseView 0.5.0 + libsigrok 0.6.0** 二次开发的 CH32H417 逻辑分析仪上位机。

- 16 通道逻辑分析，USB3 下最高 200 MS/s
- 2 通道 ADC（8/10 bit）
- 132 个 Python 协议解码器
- 阈值、触发、固件 IAP 升级、中文界面、热插拔
- 原生支持 Windows / macOS / Linux；USB 传输层在 macOS/Linux 上使用 libusb
- 设备端固件源码（CH32H417 APP + IAP + WCH 外设库）也在本仓库中

原始工程仅提供 Windows 版上位机。本仓库已补齐 macOS 原生构建、应用图标、
Linux 构建路径和 GitHub Actions 三平台 CI，并把清理后的设备端固件源码纳入 `firmware/`。

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
├── firmware/                   # 设备端固件（APP + IAP + WCH 外设库）
│   ├── CH32H417_Logic_Analyzer_APP/
│   ├── CH32H417_Logic_Analyzer_IAP/
│   ├── SRC/                    # WCH 官方外设库 / 启动文件 / 链接脚本
│   └── tools/make_app_bin.py   # V3F + V5F 合并为 IAP 升级镜像
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

## 设备端固件（APP + IAP）

固件源码在 [firmware/](firmware/)，保留原工程的目录层级：

- `CH32H417_Logic_Analyzer_APP/`：V3F（上电/唤醒）+ V5F（采集、USB、IAP 命令）双核主程序；
- `CH32H417_Logic_Analyzer_IAP/`：V3F/V5F 双核 Bootloader；
- `SRC/`：WCH 官方 CH32H417 外设库、启动文件和链接脚本；
- `tools/make_app_bin.py`：把 V3F/V5F APP 合并成 IAP 升级镜像。

构建环境是 **MounRiver Studio 2 / riscv-wch-elf-gcc 12.2**，APP 需要先编 V3F 再编 V5F；
完整步骤、Flash 布局和 UHSIF 闭源库说明见 [firmware/README.md](firmware/README.md)。

> `APP/`、`IAP/`、`SRC/` 必须保持在 `firmware/` 下的兄弟层级，不能单独挪动：
> `.project` 使用 `PARENT-2-PROJECT_LOC/SRC/...` 这类相对路径引用外设库。

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
./macos/install_deps.sh            # 首次：Homebrew 依赖（qt@5 / glibmm@2.66 / libusb / ...）
./macos/build.sh --install         # 构建 + 打包依赖 + 安装到 /Applications
```

日常改完代码只需要增量重建（改动 `libsigrok/` 时驱动会自动重编、PulseView 自动重链）：

```bash
./macos/build.sh --bundle          # 只构建，产物在 build/pulseview/LogicAnalyzer.app
./macos/build.sh --clean --bundle  # 全量重建，怀疑缓存损坏时用
```

`--install` 隐含 `--bundle`，它会先退出正在运行的上位机，把旧版本备份到 `/tmp`
（只保留最新一份），再复制到 `/Applications` 并校验签名。想装到别处用
`LOGICVIEW_INSTALL_DIR`：

```bash
LOGICVIEW_INSTALL_DIR="$HOME/Applications" ./macos/build.sh --install
```

手动安装（等价于 `--install` 的最后一步）：

```bash
ditto build/pulseview/LogicAnalyzer.app /Applications/LogicAnalyzer.app
open /Applications/LogicAnalyzer.app
```

`--bundle` 会把 Qt5、Python.framework、glib/boost/libusb 等依赖复制进
`Contents/Frameworks` 并改写路径；产物约 120MB，运行时不依赖 Homebrew。
构建阶段仍然需要 Homebrew 的 `qt@5`、`python@3.14` 等。

只想调试、不做打包时，去掉 `--bundle` 可以直接跑（运行时用 Homebrew 的 Qt，
需要先 `source macos/env.sh`）：

```bash
source macos/env.sh && open build/pulseview/LogicAnalyzer.app
```

验证产物是否自包含（应无输出）：

```bash
otool -L build/pulseview/LogicAnalyzer.app/Contents/MacOS/LogicAnalyzer | grep homebrew
```

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

本项目是**混合许可**仓库，不是所有文件都用同一个许可证：

- **上位机**（`libsigrok/`、`pulseview/`、`libsigrokdecode/`、`macos/`、`linux/`、`tools/`）：
  **GNU GPL v3.0 or later**，完整文本见 [LICENSE](LICENSE)。因为链接了 GPL-3.0-or-later 的
  libsigrok / libsigrokdecode；PulseView 部分原为 GPL-2.0-or-later。
- **设备端固件**（`firmware/`）：**不属于**上述 GPL。作者 Q2H2 与 WCH 的原始条款仍然适用，
  `libUHSIF.a` 是 WCH 的闭源二进制库。详见 [firmware/NOTICE](firmware/NOTICE) 和
  [firmware/README.md 第 7 节](firmware/README.md#7-许可提醒)。

分发或商用前请分别确认两部分的授权；本仓库不重新许可固件代码。
