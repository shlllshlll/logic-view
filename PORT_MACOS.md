# CH32H417 逻辑分析仪上位机 macOS 移植方案

> 实测环境：macOS 27.2 (arm64) / Apple clang 21 / Homebrew 6.0.21
> 实测日期：2026-11-12
> 设备：CH32H417 Logic Analyzer，`1a86:5537`，固件 2 / 硬件 1

---

## 0. 结论摘要

| 项目 | 结论 |
|---|---|
| 可行性 | **可行**。UI、解码器、文件格式本身跨平台，唯一硬伤是 USB 访问层绑死 WCH 的 Windows DLL |
| 设备侧 | 已在 macOS 上**实测打通**：读到完整描述符、配置设备、claim 接口、发 `0xAC` 收 `0xBC`（纯 IOKit，无 WCH DLL、无 libusb、无 root） |
| 实现方案 | 用 **libusb** 重写 `ch375_wrapper`（保持对外 API 不变，上层几乎不改） |
| 构建 | Homebrew 依赖 + 原生 CMake/Ninja + `LogicAnalyzer.app` |
| 主要风险 | 16ch@200MHz = **400 MB/s** 的持续吞吐；其余均为常规适配 |
| 预估工作量 | 第一阶段 **4–7 个工程日**（不含公证/分发） |

---

## 0.1 路线图（两阶段）

### 第一阶段：最小代价对齐（当前）

**目标**：不改架构，把现有 PulseView 上位机原样搬到 macOS，功能对齐 Windows 版。

- 范围：libusb USB 后端、构建系统适配、`.app` 打包；UI、解码器、文件格式保持不动。
- 验收：能枚举设备、读版本、16ch 逐档采集到 200MHz、触发/ADC/热插拔/IAP 可用、能存开 `.sr`。
- 状态：依赖已安装、目录已整理、git 基线已建立；接下来进入 P0（编译打通）。

### 第二阶段：Web 技术重构（概要，第一阶段完成后再细化）

**目标**：前后端分离，用 Web 技术重写 UI，后端用 C++/Rust 做高性能采集与解码。

- **后端**：C++（复用 libsigrok）或 Rust；负责 USB 采集、触发、环形缓冲、解码调度；对外提供本地 API（WebSocket / IPC）。
- **前端**：TypeScript + Canvas/WebGL 波形渲染；壳层可用 Tauri（Rust，体积小）或 Wails，也可纯浏览器访问。
- **数据通道**：二进制帧 + 背压 + 环形缓冲，避免 JSON/Base64 中转。
- **解码器**：短期保留 Python sidecar（复用现有 132 个解码器）；长期评估把常用协议迁到 Rust/TS。
- **风险**：400MB/s 数据穿过 WebView 的传输与渲染、解码器生态、整体范围较大。
- **建议路径**：先做「打开 `.sr` 的只读波形浏览器」→ 再接实时采集 → 再补解码/触发 → 最后替换旧 UI。

> 第二阶段暂不展开，等第一阶段验收通过后再做架构评审和里程碑拆分。

---

## 1. 实测基线（2026-11-12）

### 1.1 USB 链路

换 USB3 线后，设备从 USB2.1 Hub 移到 **USB3.2 Hub** 下：

| 属性 | USB2 线（旧） | USB3 线（现在） |
|---|---|---|
| 链路速率 | 480 Mbps | **5 Gbps** |
| `bcdUSB` | `0x0200` | **`0x0300`** |
| `bMaxPacketSize0` | 64 | **512** |
| 端点包大小 | 512 | **1024** |
| 配置描述符长度 | 60 | **96**（多了 6 个 SS 端点伴侣描述符） |
| 端点类型 | 6 × bulk | 6 × bulk |
| `maxburst` | — | **15（16 × 1024 = 16KB/突发）** |

设备描述符：`bDeviceClass=0xFF / SubClass=0x80 / Protocol=0x55`（WCH 私有标记），单配置、单接口（vendor class `0xFF`）、alt 0、6 个端点。

端点映射与驱动里的 `PIPE_*` 完全一致：

| 端点 | 方向 | 类型 | 包大小 | 对应 |
|---|---|---|---|---|
| `0x01` / `0x81` | OUT / IN | bulk | 1024 | `PIPE_CMD`（命令/响应） |
| `0x02` / `0x82` | OUT / IN | bulk | 1024 | `PIPE_LOGIC_DATA`（逻辑数据） |
| `0x03` / `0x83` | OUT / IN | bulk | 1024 | `PIPE_ADC_DATA`（ADC 数据） |

### 1.2 协议实测

用 `tools/usb_probe.m`（纯 IOKit/IOUSBHost，281 行）执行：

```
device 1a86:5537 bcdUSB=0x0300 class=ff/80/55 bMaxPacketSize0=9 bNumConfigurations=1
  config: wTotalLength=96 bNumInterfaces=1 bConfigurationValue=1
  interface 0 alt=0 class=0xff sub=0xff proto=0xff nEP=6
    ep 0x81 bulk maxpacket=1024  ss-companion maxburst=15
    ep 0x01 bulk maxpacket=1024  ss-companion maxburst=15
    ...（2/0x82、3/0x83 同）
configuring device (bConfigurationValue=1)...
sent 0xAC, 64 bytes
got 32 bytes:
bc 03 00 02 01 00 00 ...
  => firmware=2 hardware=1 (status 0x00)
```

`0xAC` = `CMD_GET_VERSION`，`0xBC` = `RESP_GET_VERSION`，回包布局 `[cmd, len, status, data...]`，与
`protocol.c` 完全一致。

**结论：**

1. 设备在 macOS 上可被用户态直接访问，**不需要 kernel driver、不需要 `com.apple.vm.device-access` entitlement、不需要 root**。
2. 端点全部是 **bulk**，没有 isochronous，libusb 的 bulk 通路足够。
3. `bcdUSB=0x0300` ⇒ 驱动的 `ch32h417_detect_usb_speed()` 会走 **USB3 采样率表（最高 200MHz）**。
4. 命令层不需要任何 WCH 私有初始化握手。

---

## 2. 背景

### 2.1 工程构成

| 目录 | 内容 | 规模 |
|---|---|---|
| `pulseview/` | PulseView **0.5.0**（Qt5）深度改造 | `mainbar.cpp` 2849 行、`session.cpp` 2366 行 |
| `libsigrok/` | 驱动后端 0.6.0 裁剪版 | 仅 `wch-ch32h417` + `demo` 两个驱动 |
| `libsigrok/bindings/cxx` | libsigrokcxx | 仅 `classes.cpp` |
| `libsigrokdecode/` | Python 协议解码 | 132 个解码器目录、288 个 `.py` |

定制功能：16ch 逻辑 + 2ch ADC、200MS/s、阈值、触发、固件 IAP 升级、中文 UI、热插拔。

### 2.2 数据通路

```
PulseView UI
  └ libsigrok 驱动 (api.c / protocol.c)   ← 开源：命令字、包格式、参数
      └ ch375_wrapper.c (539 行)          ← 唯一 Windows 相关的层
          ├ CH375DLL64.DLL + WDM          ← Windows 闭源，必须替换
          └ libusb                        ← macOS 实现
```

上层只调用 wrapper 的十几个函数：

`ch375_init / open_device / close_device / get_usb_id / get_device_descr /
set_timeout_ex / set_io_mode / write_endpoint / read_endpoint /
set_buf_upload_ex / query_buf_upload_ex / clear_buf_upload /
set_buf_download_ex / set_device_notify / cleanup`

### 2.3 可移植性核对

| 位置 | 状态 |
|---|---|
| `libsigrok` 驱动注册 section | 已有 `__APPLE__` 分支（`libsigrok-internal.h:993`） |
| `storesession.cpp` 文件写入 | 已有 `_WIN32` 分支 |
| `main.cpp` DWM 深色标题栏 | 已有 `_WIN32` 保护 |
| `mainwindow.cpp` 内存检查 | ❌ `MEMORYSTATUSEX` 无保护，mac 编译失败 |
| `application.cpp` | ❌ 运行 `UpdateFwVersion.exe`（CH569 遗留） |
| `pulseview/CMakeLists.txt` | ❌ `/mingw64/lib`、`-Wl,-Bdynamic`、hidapi/libusb 仅 WIN32 |
| `libsigrokdecode/CMakeLists.txt` | ⚠️ `DECODERS_DIR` 仅 WIN32 定义 |
| `pulseview/pv/session.cpp:60` | ⚠️ `#include <libusb-1.0/libusb.h>` 路径不兼容且未使用 |

---

## 3. 方案选型

| 方案 | 依赖 | 代码量 | 跨平台 | 结论 |
|---|---|---|---|---|
| **libusb** | 1 个 dylib（项目已有） | ~350 行 C | Win/Linux/mac 同一份 | ✅ **采用** |
| IOKit `IOUSBHost` | 系统框架 | ~600 行 ObjC++ | 仅 mac | 备选（性能兜底） |
| IOKit `IOUSBLib` | 系统框架 | ~700 行 C | 仅 mac | 不推荐（legacy） |
| DriverKit | Apple 授权 + dext | 很大 | 仅 mac | ❌ 过度 |
| hidapi | 项目已有 | 很少 | 全平台 | 仅 IAP/HID 用 |
| Windows 虚拟机 | — | 0 | — | 临时方案，非原生 |

**为什么选 libusb：**

1. 已经是项目依赖（`libsigrok/src/usb.c`、PulseView 热插拔线程）。
2. 一份实现覆盖三平台，Windows 后端可保留。
3. 直接调用 `ReadPipeAsync`，无额外大块拷贝；400MB/s 的瓶颈在 USB 链路，不在这层。
4. 异步多 URB、热插拔、错误恢复都是现成的。

**兜底策略：** wrapper 的对外 API 是抽象层。如果实测 400MB/s 受 libusb 限制，只需把 darwin 数据通路换成 `IOUSBHost::enqueueIORequest`，上层不动。

---

## 4. 编译依赖安装

### 4.1 Homebrew 依赖

```bash
brew install \
  cmake ninja pkgconf \
  qt@5 glibmm@2.66 libsigc++@2 \
  glib boost libusb hidapi libzip \
  python@3.14
```

| 包 | 版本（当前） | 用途 / 注意 |
|---|---|---|
| `qt@5` | 5.15.19 | **keg-only**；已 deprecated（2026-05），2027-05 会被 disable |
| `glibmm@2.66` | 2.66.10 | **必须用这个**；`glibmm`（2.90）是新的 `glibmm-2.68` ABI，不兼容 |
| `libsigc++@2` | 2.12.2 | glibmm@2.66 的依赖 |
| `glib` | 2.90.1 | glib-2.0 |
| `boost` | 1.92.0 | filesystem / serialization / system |
| `libusb` | 1.0.30 | 新后端 |
| `hidapi` | 0.15.0 | IAP 固件升级 |
| `libzip` | 1.12.1 | `.sr` 压缩格式 |
| `python@3.14` | 3.14.10 | 协议解码器 |
| `cmake` / `ninja` / `pkgconf` | 4.4.3 / 1.13.2 / 3.0.6 | 构建 |

### 4.2 环境变量

实际脚本为 `macos/env.sh`（用法：`source macos/env.sh`）：

```bash
export HOMEBREW_PREFIX="$(brew --prefix)"
export QT5="$(brew --prefix qt@5)"
export GLIBMM="$(brew --prefix glibmm@2.66)"
export PYTHON_PREFIX="$(brew --prefix python@3.14)"

export PATH="$QT5/bin:$HOMEBREW_PREFIX/bin:$PYTHON_PREFIX/libexec/bin:$PATH"
export PKG_CONFIG_PATH="$QT5/lib/pkgconfig:$GLIBMM/lib/pkgconfig:$PYTHON_PREFIX/lib/pkgconfig:$HOMEBREW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$QT5:$GLIBMM:$HOMEBREW_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
```

验证（2026-11-12 实测）：

```bash
pkg-config --modversion glibmm-2.4          # 2.66.10
pkg-config --modversion libusb-1.0          # 1.0.30
pkg-config --modversion hidapi              # 0.15.0
pkg-config --modversion python-3.14-embed   # 3.14
lrelease -version                           # Qt 5.15.19
```

### 4.3 已知坑

| 坑 | 处理 |
|---|---|
| `qt@5` keg-only | `CMAKE_PREFIX_PATH` / `PKG_CONFIG_PATH` 指向 `brew --prefix qt@5` |
| 误装 `glibmm` 2.90 | 只用 `glibmm@2.66`；`pkg-config glibmm-2.4` 必须存在 |
| CMake 4 对 `cmake_minimum_required(3.5)` / Qt5 旧 config | 加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`；必要时改用 CMake 3.31 |
| Boost 1.92 与 `FindBoost` | 失败时用 `-DBoost_DIR=$(brew --prefix boost)/lib/cmake/Boost-1.92.0` |
| **`pkg-config python3` 不存在** | Homebrew 只提供 `python-3.14.pc` / `python-3.14-embed.pc`。`libsigrokdecode` 的 CMake 需改成候选探测（`python3-embed` → `python-3.14-embed` → `python3`），且必须用 embed 版的 `-lpython3.14` |
| **`hidapi` 只有 `hidapi.pc`** | PulseView 里查询 `hidapi-hidraw`/`hidapi-libusb` 会失败；mac 分支直接用 `hidapi` |
| **`openssl@4` 副作用** | 安装 `python@3.14` 时 Homebrew 把 `openssl` 链接从 3.6.4 切到 4.1.0；`openssl@3` 仍在但未链接。恢复方法见 `macos/deps.txt` |
| 源码树残留 | 已清理（见 `.gitignore`） |

### 4.4 依赖记录与清理

- `macos/brew-formulae-before.txt` / `brew-formulae-after.txt`：安装前后快照
- `macos/deps.txt`：精确版本、新增包、被顺带升级的共享依赖、openssl 副作用
- `macos/install_deps.sh`：可重复执行，默认不升级无关的 dependents
- `macos/cleanup_deps.sh`：卸载本次新增的顶层包；`--autoremove` 清理孤儿依赖，`--build` 删除构建产物

---

## 5. 构建系统改造

### 5.1 `libsigrok/CMakeLists.txt`

```cmake
# 1) 按平台选择 wrapper 实现
if(WIN32 OR MINGW)
    list(APPEND LIBSIGROK_SOURCES
        src/hardware/wch-ch32h417/ch375_wrapper.c)
else()
    list(APPEND LIBSIGROK_SOURCES
        src/hardware/wch-ch32h417/ch375_wrapper_libusb.c)
endif()

# 2) ch32 驱动强依赖 libusb
pkg_check_modules(LIBUSB REQUIRED libusb-1.0>=1.0.16)

# 3) 非 Windows 平台库
if(NOT WIN32 AND NOT MINGW)
    set(PLATFORM_LIBS m pthread)
endif()
```

> 现状：`src/.../ch375_wrapper.c` 被无条件编译，且里面直接 `#include <windows.h>`，mac 上必定失败，必须拆开。

### 5.2 `pulseview/CMakeLists.txt`

| 位置 | 现状 | 修改 |
|---|---|---|
| L536 | `include_directories(D:/msys64/...)` | 包进 `if(WIN32)` |
| L632 | `link_directories(/mingw64/lib)` | 包进 `if(WIN32)` |
| L646 | `-Wl,-Bdynamic` | macOS `ld` 不支持，改为 `if(NOT APPLE)` 或删除 |
| L662 | hidapi/libusb 只在 WIN32 链接 | 增加 `if(APPLE)` 分支，用 pkg-config 链接 |
| 目标属性 | 无 bundle | `MACOSX_BUNDLE TRUE` + `Info.plist` |
| rpath | 无 | `INSTALL_RPATH "@executable_path/../Frameworks"` |

关键片段：

```cmake
if(APPLE)
    pkg_check_modules(LIBUSB libusb-1.0 REQUIRED)
    pkg_check_modules(HIDAPI hidapi REQUIRED)
    include_directories(${LIBUSB_INCLUDE_DIRS} ${HIDAPI_INCLUDE_DIRS})
    list(APPEND PULSEVIEW_LINK_LIBS ${LIBUSB_LIBRARIES} ${HIDAPI_LIBRARIES})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME LogicAnalyzer
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist"
    INSTALL_RPATH "@executable_path/../Frameworks")
```

### 5.3 `libsigrokdecode/CMakeLists.txt`

```cmake
# DECODERS_DIR 对所有平台都定义（原代码只在 WIN32 分支里定义）
add_definitions(-DDECODERS_DIR="${CMAKE_INSTALL_PREFIX}/share/libsigrokdecode/decoders")

# 非 Windows 平台库
if(NOT WIN32 AND NOT MINGW)
    set(PLATFORM_LIBS m pthread)
endif()
```

> `.app` 内解码器路径不依赖编译期常量，运行时用 `srd_decoder_searchpath_add()` 指向 `Contents/Resources/decoders`。

### 5.4 构建顺序

```bash
./env_macos.sh
PREFIX="$PWD/install"

cmake -S libsigrok -B build/libsigrok -G Ninja \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release -DDISABLE_WERROR=ON
cmake --build build/libsigrok && cmake --install build/libsigrok

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

cmake -S libsigrok/bindings/cxx -B build/libsigrokcxx -G Ninja \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release
cmake --build build/libsigrokcxx && cmake --install build/libsigrokcxx

cmake -S libsigrokdecode -B build/libsigrokdecode -G Ninja \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release -DDISABLE_WERROR=ON
cmake --build build/libsigrokdecode && cmake --install build/libsigrokdecode

cmake -S pulseview -B build/pulseview -G Ninja \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$QT5;$GLIBMM;$PREFIX" \
      -DENABLE_DECODE=ON -DENABLE_FLOW=OFF -DENABLE_TESTS=OFF \
      -DENABLE_SIGNALS=OFF -DDISABLE_WERROR=ON -DSTATIC_PKGDEPS_LIBS=OFF
cmake --build build/pulseview
```

运行（开发态）：

```bash
export SIGROKDECODE_DIR="$PREFIX/share/libsigrokdecode/decoders"
export DYLD_LIBRARY_PATH="$PREFIX/lib"
./build/pulseview/LogicAnalyzer.app/Contents/MacOS/LogicAnalyzer
```

---

## 6. 代码改造计划

### 6.1 新增 `ch375_wrapper_libusb.c`

保持 `ch375_wrapper.h` 的 API 完全不变，仅替换实现。

#### 6.1.1 函数映射

| API | libusb 实现 |
|---|---|
| `ch375_init` / `cleanup` | `libusb_init(NULL)` 一次（`g_once`），cleanup 只停线程、关句柄，**不调用 `libusb_exit`**（PulseView 热插拔线程还在用默认 context） |
| `open_device(index)` | 枚举 `1a86:5537` 过滤列表 → `libusb_open` → 若未配置则 `libusb_set_configuration(h, 1)` → `libusb_claim_interface(h, 0)` |
| `close_device` | 停该设备所有 pipe 线程 → `release_interface` → `close` |
| `get_usb_id` | `libusb_get_device_descriptor` 拼 `(pid << 16) \| vid` |
| `get_device_descr` | 同上，拷 18 字节 |
| `set_timeout_ex` | 记录 read/write 超时（默认 500ms） |
| `set_io_mode` / `set_buf_download_ex` | 空实现返回成功 |
| `write_endpoint` | pipe 1/2/3 → `0x01/0x02/0x03`，`libusb_bulk_transfer`（同步） |
| `read_endpoint` | pipe 1 → `0x81` 同步读；pipe 2/3 → 从队列取（见下） |
| `set_buf_upload_ex` | 记录 `enable/endpoint/transferSize`；首次 query/read 时惰性启动 reader |
| `query_buf_upload_ex` | 返回队列里的包数、字节数 |
| `clear_buf_upload` | 清空队列、重置计数（不停止 reader） |
| `set_device_notify` | 300ms 轮询 VID/PID 出现/消失，回调 `CH375_DEVICE_ARRIVAL(3)` / `CH375_DEVICE_REMOVE(0)` |
| `abort_*` / `read_data` / `write_data` | 空实现或返回失败 |

#### 6.1.2 数据结构

```c
struct ch375_pipe {
    gboolean enabled;
    int ep_in, ep_out;
    size_t transfer_size;

    GThread *thread;          /* 每 pipe 一个事件线程（或用设备级单线程） */
    GMutex lock;
    GCond cond;
    GQueue *queue;            /* 已完成的 buffer: (data, len) */
    size_t queued_bytes;
    unsigned long queued_packets;

    /* 当前正在读出的队首偏移 */
    size_t head_offset;

    gboolean stop;
    struct libusb_transfer *xfers[8];
    int nxfers;
};

struct ch375_dev {
    int index;
    libusb_device_handle *handle;
    int refcount;

    unsigned long wtimeout, rtimeout;
    guint8 bus, addr;

    struct ch375_pipe pipes[4];   /* 下标 = PIPE_CMD/LOGIC/ADC */
    GMutex lock;
};
```

#### 6.1.3 数据通路（关键）

驱动侧读循环：

```c
query_buf_upload_ex(pipe)          // 有积压吗？
read_endpoint(pipe, buf, len)      // 取数据
```

实现：

1. `set_buf_upload_ex(enable=1, pipe, size)` 只记录参数。
2. 第一次 `query`/`read` 时启动 reader：
   - 分配 `N=8` 个 transfer，每个 `chunk = clamp(size, 256KB, 1MB)`；
   - `libusb_fill_bulk_transfer(...)` + `libusb_submit_transfer`；
   - 事件线程 `libusb_handle_events_timeout_completed()`；
   - 完成回调：`status == COMPLETED && length > 0` → 入队、计数、重新提交；`CANCELLED`/`NO_DEVICE` → 停止。
3. `query` 返回 `queued_packets` / `queued_bytes`（队列为空返回 0，驱动会走 1ms 重试/500ms 超时逻辑）。
4. `read_endpoint` 从队首拷贝，支持跨多个 buffer 拼满调用方长度；不足部分留在 `head_offset`。
5. `clear` 只清队列；`disable`/`close` 取消 transfer、join 线程。

参数依据：SS 端点 `maxburst=15`（16KB/突发）、包 1024B；1MB × 8 在飞 ≈ 20ms 缓冲，兼顾延迟与吞吐，后续按实测调整。

> 注意：`ch375_init_comm_params()` 在 scan 阶段会对 `PIPE_LOGIC_DATA` 和 `PIPE_ADC_DATA` 都调用
> `set_buf_upload_ex(1, ...)`。惰性启动可以避免为未使用的 pipe 白白挂 8 个 1MB transfer。

#### 6.1.4 生命周期与线程安全

- `ch375_init` 可重入（scan / 热插拔都会调），用引用计数。
- `open_device` 对同一 index 可重复调用（scan 打开后 `dev_open` 会再开），引用计数。
- `close_device` 必须能处理“采集线程已 join、但 pipe reader 还活着”的情况：先停 pipe，再关句柄。
- 回调 `set_device_notify` 在轮询线程触发；PulseView 内部已经用 `QMetaObject::invokeMethod` 切回主线程，无需改 UI 代码。
- 不要调用 `libusb_exit(NULL)`，否则 PulseView 自己的 `libusb_get_device_list(NULL)` 线程会崩。

### 6.2 PulseView 适配

| 文件 | 修改 |
|---|---|
| `pv/mainwindow.cpp:102` | `MEMORYSTATUSEX` → macOS 用 `sysctl(HW_MEMSIZE)` 或 `host_statistics64`；整段加 `_WIN32` / `__APPLE__` 分支 |
| `pv/application.cpp:269` | `UpdateFwVersion.exe` 启动整段加 `#ifdef _WIN32` |
| `pv/session.cpp:60` | 删除未使用的 `#include <libusb-1.0/libusb.h>`（或改 `<libusb.h>`） |
| `pv/toolbars/mainbar.hpp:48` | `<libusb.h>` 由 pkg-config include 路径提供，无需改 |
| `pv/dialogs/iapdialog.cpp:35` | `<hidapi.h>` 在 mac 上由 `-I.../include/hidapi` 提供；若用 pkg-config 则保持 |
| `main.cpp` | 在 `srd_decoder_load_all()` 前加 `srd_decoder_searchpath_add()` 指向 bundle 内解码器 |
| `pulseview/CMakeLists.txt` | 见 5.2 |

### 6.3 协议解码器

- 开发态：`SIGROKDECODE_DIR=$PREFIX/share/libsigrokdecode/decoders`。
- `.app`：解码器拷到 `Contents/Resources/decoders`，启动时：

```cpp
#ifdef __APPLE__
const QString pd = QCoreApplication::applicationDirPath() + "/../Resources/decoders";
srd_decoder_searchpath_add(pd.toUtf8().constData());
#endif
```

- Python：开发态链接 Homebrew `libpython3.14.dylib` 即可；分发时需把 `Python.framework`（或
  python-build-standalone）与 stdlib 一起放进 bundle，并设置 `PYTHONHOME`。

### 6.4 打包（第二阶段）

1. `macdeployqt LogicAnalyzer.app -always-overwrite` 搬 Qt 框架。
2. 把 `libsigrok*.dylib` 拷进 `Contents/Frameworks`，`install_name_tool -change` 改路径。
3. 解码器进 `Contents/Resources/decoders`。
4. `codesign --force --deep -s - LogicAnalyzer.app`（本机 ad-hoc；分发需 Developer ID + 公证）。
5. `Info.plist`：`CFBundleIdentifier`、`CFBundleName`、`CFBundleIconFile`、`NSHighResolutionCapable`。

---

## 7. 第一阶段：实施阶段与验收

| 阶段 | 内容 | 验收标准 | 预估 |
|---|---|---|---|
| **P0 构建打通** | 装依赖、清残留、改 3 个 CMake、修 2 处 Windows API | 用 `demo` 驱动启动；解码器列表 ~130 项；`.sr` 存/开正常；退出不崩 | 0.5–1 天 |
| **P1 驱动核心** | libusb wrapper：枚举 / open / claim / 描述符 / 超时 / 命令通道 / 轮询热插拔 | 应用能列出 `WCH CH32H417 Logic Analyzer`；读到 firmware=2 / hardware=1；反复开关设备不崩 | 1 天 |
| **P2 数据通路** | reader 线程 + 队列 + 异步多 URB；1MHz → 200MHz 逐档 | 1MHz 方波正确；50/100/200MHz 无硬件溢出（或记录稳定上限）；触发正常；ADC 模式正常 | 1.5–3 天 |
| **P3 热插拔 + IAP** | 插拔回调、`0xAE` 进 IAP、hidapi 升级 | 插拔 UI 自动更新；固件升级成功（用已知 `.bin` 验证） | 0.5–1 天 |
| **P4 打包** | `.app`、dylib、解码器、Python、签名 | 双击运行；换一台 Mac（无 brew）也能跑（可选） | 0.5–1 天 |

**独立冒烟测试**：`tools/capture_test.c` 已实现，直接走 libsigrok API：
`open → set rate/limit/threshold → session_start → 收数据 → stop`。

### 当前进展（2026-11-12）

| 阶段 | 状态 | 实测 |
|---|---|---|
| P0 构建打通 | ✅ 完成 | `./macos/build.sh --clean` 一键全量构建成功；`LogicAnalyzer.app` 生成；132 个解码器加载 |
| P1 驱动核心 | ✅ 完成 | 应用列出设备；`bcdUSB=0x0300`、USB3、firmware=2/hardware=1；反复启动稳定 |
| P2 数据通路 | ✅ 核心完成 | 逻辑：1MHz / 50MHz / 200MHz 均可采集；4MB × 8 在飞下 200MHz × 100M 样本 = 200MB，0.771s（≈251MB/s），无 overflow 标志。ADC：1MHz × 100k 样本输出 0.548–1.486V，证明数据通路真实 |
| P3 热插拔 + IAP | 🟡 部分 | 轮询热插拔代码已接入；IAP 尚未用固件实测 |
| P4 打包 | 🟡 本机可用 | `.app` 内置 libsigrok*/解码器，ad-hoc 签名；Qt/brew 依赖仍走系统路径 |

**待验证（需要硬件配合）**：

1. **逻辑电平**：当前逻辑输入悬空，数据全 0。已用 `tools/usb_probe --capture` 直接读 EP 0x82
   （512KB 全 0）复现，排除 libusb/驱动问题；ADC 悬空读到 0.5–1.5V，说明 USB 通路正常。
   用跳线把 D0 接到 3.3V/GND 或已知方波，确认电平与采样点正确。
2. **采样率校准**：同一 50M 样本数下，200/100/50/25MHz 的墙钟耗时分别约
   0.446 / 0.611 / 0.608 / 0.608s。低采样率没有按实时变慢，说明设备侧的数据产出节奏
   需要接已知频率信号后用周期/时间戳进一步确认（可能是固件缓冲/分频语义问题）。
3. **触发**：接信号后设置 rising/falling/edge，确认触发点位置。
4. **热插拔**：运行中拔插设备，确认 UI 自动更新。
5. **IAP**：用已知固件 `.bin` 走一次升级流程。

---

## 8. 测试计划

| 项 | 方法 | 通过标准 |
|---|---|---|
| 枚举 | 启动应用 | 设备出现在下拉框 |
| 版本 | 选中设备 / 查看硬件版本 | firmware=2, hardware=1 |
| 低速采集 | 1MHz 方波 | 波形周期/占空比正确 |
| 逐档压测 | 10/20/50/100/200MHz | 无 `Upload backlog` / `overflow` 告警，采样数正确 |
| 触发 | rising/falling/edge | 触发点位置正确 |
| 阈值 | 改阈值电压 | 波形翻转点变化 |
| ADC | A0/A1，8/10bit | 电压换算正确 |
| 热插拔 | 拔插设备 | UI 自动切换，无需重启 |
| IAP | 升级固件 | 进度到 100%，设备重启后版本变化 |
| 解码器 | I²C/SPI/UART 样例 | 解码结果正确 |
| 文件 | 保存/打开 `.sr` | 波形与解码完整 |
| 稳定性 | 连续采集 10 分钟 | 无崩溃、无句柄泄漏 |

性能指标：`Acquisition summary` 里的 `average MB/s`、`max_pending`、`overflow_occurred`。

---

## 9. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| 16ch@200MHz = 400MB/s 持续吞吐 | **高** | 异步多 URB（1MB × 8 起步）；按实测调 chunk/在飞数；退档 100MHz 或 8ch@200MHz 也可接受 |
| WCH buffer-upload 语义偏差 | 中 | 惰性 reader + 队列模拟；用冒烟工具对比“读到的字节率 vs 采样率”；必要时改读循环 |
| macOS 设备捕获/权限 | 低 | 已实测 vendor class 直接 open，无需 entitlement；HID（IAP）走 hidapi |
| Qt5 2027 停更 | 低 | 现阶段可用；后续可迁 Qt6（工程量大，另立项） |
| Python 分发 | 中 | 开发态用 brew python；分发时 bundle framework + stdlib |
| IAP HID 权限/报文 | 中 | 先在本机验证；macOS 一般不需要额外授权 |
| CMake 4 + Qt5 旧 config | 中 | `CMAKE_POLICY_VERSION_MINIMUM=3.5`；或固定 CMake 3.31 |
| 源码树残留（`pulseview/config.h` 等） | 低 | 建 git 基线后清理 |

---

## 10. 文件变更清单

**新增**

```
README.md                                 # 项目总览
.gitignore
PORT_MACOS.md                             # 本文档
macos/env.sh                              # 环境变量
macos/install_deps.sh                     # 依赖安装 + 记录
macos/cleanup_deps.sh                     # 依赖清理
macos/deps.txt                            # 本次安装记录
macos/build.sh                            # 构建脚本（P0 阶段补）
tools/usb_probe.m                         # 已完成：设备描述符/版本探针
attic/                                    # 历史 .bak 归档
libsigrok/src/hardware/wch-ch32h417/ch375_wrapper_libusb.c
packaging/Info.plist
```

**修改**

```
libsigrok/CMakeLists.txt                   # 平台选择 wrapper、libusb REQUIRED
pulseview/CMakeLists.txt                   # 去 MinGW 硬编码、加 mac 链接/bundle
libsigrokdecode/CMakeLists.txt             # DECODERS_DIR、Python 探测、平台库
pulseview/pv/mainwindow.cpp                # MEMORYSTATUSEX
pulseview/pv/application.cpp               # UpdateFwVersion.exe
pulseview/pv/session.cpp                   # libusb include
pulseview/main.cpp                          # 解码器搜索路径（可选）
```

**已删除（构建残留）**

```
pulseview/config.h
pulseview/Makefile
pulseview/cmake_install.cmake
pulseview/CTestTestfile.cmake
pulseview/CPackConfig.cmake
pulseview/CPackSourceConfig.cmake
pulseview/install_manifest.txt
pulseview/qrc_pulseview.cpp
pulseview/qrc_translations.cpp
pulseview/*.depends
pulseview/.qm
pulseview/l10n/*.qm
libsigrokdecode/version.h
libsigrok/conf241733.file
```

---

## 11. 当前进度与准备

已完成：

1. ✅ 设备在 USB3（5Gbps）下的描述符/版本命令验证（`tools/usb_probe.m`）。
2. ✅ 构建依赖安装并记录（`macos/deps.txt`、`macos/install_deps.sh`、`macos/cleanup_deps.sh`）。
3. ✅ 源码目录整理：构建残留清理、`.bak` 归档到 `attic/`、新增 `README.md` / `.gitignore`。
4. ✅ `git init` 并提交基线（commit `1ac1b0e`）。

配置检查（2026-11-12）：

- `libsigrok` configure **通过**（glib / libusb / libzip 均被识别）。
- `libsigrokdecode` configure **失败**，原因正是 `pkg_check_modules(PYTHON3 REQUIRED python3>=3.2)`：
  Homebrew 只提供 `python-3.14-embed.pc`。这是 P0 要修的第一项。

开始 P0 前确认：

1. 设备保持插在 **USB3 口 / USB3 线**（当前 5Gbps 已确认）。
2. 确认没有 Chrome/其他程序通过 WebUSB 占用设备。
3. `source macos/env.sh` 后 `pkg-config` 能查到 `glibmm-2.4`、`libusb-1.0`、`hidapi`、`python-3.14-embed`。

---

## 12. 备注：为什么不需要逆向 WCH DLL

- DLL 只是“端点搬运 + 内核侧缓冲”的封装；协议由开源的 `protocol.c` 定义。
- 端点类型/包大小由设备描述符给出（已实测：6 × bulk，1024B，maxburst 15）。
- 命令层已用原生 IOKit 打通（`0xAC` → `0xBC`），不需要 DLL 的任何初始化。
- 唯一需要“试”的是 buffer-upload 的排队策略，用独立采集工具即可校准。
