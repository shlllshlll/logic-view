# CH32H417 逻辑分析仪 v2.0 — 干净源码树

这是从作者发布的 `CH32H417_Logic_Analyzer_2v0` 固件包整理出来的**纯源码工程**：
删掉了所有编译产物，补回了缺失的依赖，工程可以直接用 MounRiver Studio 2 打开编译。

原始包里的 `obj/`、`*.o`、`*.d`、`*.map`、`*.lst`、`*.elf`、`*.hex`、`*.bin`、`.DS_Store`
全部删除，**没有保留任何预编译文件**（唯一例外见下面「关于 UHSIF」一节）。

---

## 1. 目录结构

```
.
├── CH32H417_Logic_Analyzer_IAP/          IAP（Bootloader）
│   ├── CH32H417_IAP.wvsln                工作空间
│   ├── Common/  Debug/ Ld/ USB/          flash.c iap.c hardware.c + USB 驱动
│   ├── V3F/                              V3F 核工程（.cproject/.wvproj/User）
│   └── V5F/                              V5F 核工程
├── CH32H417_Logic_Analyzer_APP/          APP（逻辑分析仪主程序）
│   ├── CH32H417_Logic_Analyzer.wvsln
│   ├── Common/                           ★ 作者原创代码都在这里
│   │   ├── board_config.h                ★ 板型配置（WEU6 / QEU6），见 4.1
│   │   ├── ch32h417_logic.c / .h         采集 + USB 上传 + IAP 命令（897 行）
│   │   ├── ch32h417_uhsif.c / .h         UHSIF 应用层（WCH 例程改）
│   │   ├── hardware.c / .h               上电初始化（69 行）
│   │   ├── libUHSIF.a                    ★ 本目录新增，见第 3 节
│   │   └── ch32h417_usb*.c usb_desc.c    USB 描述符与中断（WCH 例程）
│   ├── V3F/                              V3F 核工程（上电唤醒 V5F 后睡下去）
│   └── V5F/                              V5F 核工程（真正干活的那个）
├── SRC/                                  WCH 官方外设库（Core / Debug / Ld / Peripheral / Startup）
└── tools/
    └── make_app_bin.py                   合并 V3F+V5F 成 IAP 升级用的 APP.bin
```

**目录层级不能动**：`.project` 里用的是 `PARENT-1-PROJECT_LOC/Common`、
`PARENT-2-PROJECT_LOC/SRC/...` 这类相对软链接，挪位置会导致工程找不到文件。

---

## 2. 和原始包相比改了什么

| 操作 | 内容 |
|---|---|
| 删除 | 4 个 `obj/` 目录（198 个 `.o`、197 个 `.d`、33 个 `subdir.mk`、makefile、`*.map`、`*.lst`、`*.elf`、`*.hex`、`*.bin`） |
| 删除 | 7 个 `.DS_Store` |
| 删除 | `Common/ch32h417_uhsif_it.o`（129 KB 预编译目标文件） |
| 新增 | `Common/libUHSIF.a`（WCH 官方 UHSIF 静态库，19 KB） |
| 新增 | `tools/make_app_bin.py` |
| 修改 | `APP/V5F/.cproject`：链接器 `Libraries` 加 `UHSIF`，`Other objects` 移除 `ch32h417_uhsif_it.o` |
| 修改 | `APP/V5F/*.wvproj`：同上，两处（C / C++ linker）+ 移除 `.d` 残留条目 |
| 修改 | `APP/V3F/*.wvproj`：移除对不存在的 `ch32h417_uhsif_it.c` 的引用 |
| 修改 | `APP/V3F/.cproject`：移除 `excluding` 里对 `ch32h417_uhsif_it.c` 的排除项 |
| 新增 | `Common/board_config.h`：WEU6 / QEU6 两套板型宏 |
| 修改 | `Common/ch32h417_logic.c`：DAC / ADC / LED / 共用电源改为读板型宏 |
| 修改 | `Common/ch32h417_uhsif.c`：数据总线 remap、水线改为读板型宏 |
| 修改 | `Common/ch32h417_logic.h`：版本宏改为 `SW=0x03` + `HW=BOARD_HW_VERSION` |
| 修改 | `Common/hardware.c`：ADC 端口时钟改用板型宏 |
| 未动 | 其余 `.c` / `.h` / `.S` / `.ld` 源码，一个字节都没改 |

IAP 工程的 4 个文件**完全没有改动**——它本来就完整。

---

## 3. 关于 UHSIF：这一段是闭源的，绕不过去

原始包里 `Common/ch32h417_uhsif_it.c` 缺失，只有一个 `ch32h417_uhsif_it.o`。
我核对了 WCH 官方仓库 `openwch/ch32h417`，结论是：

> **WCH 官方例程 `EVT/EXAM/UHSIF/UHSIF_SLAVE/` 里也没有这个 `.c`，只提供 `Common/libUHSIF.a`。**

也就是说，UHSIF（通用高速接口）驱动是 WCH 的**闭源二进制库**，源码从未公开。
作者手上应该有源码（那个 `.o` 带完整调试信息，`DW_AT_name` 指向 `e:/沁恒代码/.../ch32h417_uhsif_it.c`，
`UHSIF_Cfg` 在源文件第 1032 行），但发布时只给了编译产物。

所以这里用**官方库**替代，和 WCH 自己例程的做法完全一致。

### 等价性验证

| 检查项 | 结果 |
|---|---|
| 导出符号 | 完全一致：`UHSIF_GPIO_Init` `UHSIF_Cfg` `UHSIF_IRQHandler` `UHSIF_Line_Cfg` `UHSIF_Start` `UHSIF_Trans_Cfg` `UHSIF_Get_Ver` `UHSIF_Reg_Cfg` `SOCK_CFG_INIT` `UHSIF_Para` `SOCKET_DSCR_RD/WR` |
| 外部依赖符号 | 除 `__riscv_save_0/restore_0` 与 `save_1/restore_1` 的差别外完全一致（都由 libgcc 提供，无影响） |
| 各函数体积 | 略有差异（`UHSIF_Cfg` 0x308 vs 0x29e 等），属于不同编译版本，功能等价 |
| 链接配置 | 与 WCH 官方 `UHSIF_DEVICE_V5F` 工程一致：`libs = UHSIF`、库路径 `${ProjName}/Common`、`other objects` 为空 |

> 副作用：**这一份驱动无法修改、无法审计**。想改 UHSIF 行为（改采样位宽、加 line、
> 改中断处理）只能反汇编，或者自己按官方手册重写。
> 官方手册在 `openwch/ch32h417` 仓库里：`EVT/EXAM/UHSIF/CH32H417通用高速接口（UHSIF）开发参考手册.pdf`。

---

## 4. 编译（MounRiver Studio 2）

工具链 `riscv-wch-elf-gcc 12.2`，编译选项与原工程一致（`-Os -march=rv32imac_zba_zbb_zbc_zbs_xw -mabi=ilp32`）。

### IAP

1. 打开 `CH32H417_Logic_Analyzer_IAP/CH32H417_IAP.wvsln`
2. 编译 **V3F** 子工程 → `V3F/obj/CH32H417_IAP_V3F.bin`
3. 如报 `No rule to make target`：右键工程 → `Clean Project` → 重新 Build

### APP

1. 打开 `CH32H417_Logic_Analyzer_APP/CH32H417_Logic_Analyzer.wvsln`
2. **先编 V3F，再编 V5F**
   - `V3F/obj/CH32H417_Logic_Analyzer_V3F.bin`（约 10.8 KB）
   - `V5F/obj/CH32H417_Logic_Analyzer_V5F.bin`（约 26.1 KB）
3. **合并是 MRS 自动做的**，不需要额外脚本：
   `V5F/CH32H417_Logic_Analyzer_V5F.wvproj` 里配了双核合并（`kernel.mate = "../V3F"`，
   `mergedOptions.enabled = true`，`filledWith = "0xFF"`），编 V5F 时会读取
   `../V3F/obj/CH32H417_Logic_Analyzer_V3F.hex` 并输出唯一的
   `V5F/obj/CH32H417_Logic_Analyzer_APP.bin`。
   这也是**必须先编 V3F** 的原因——否则合并进去的是旧的 hex。

   `tools/make_app_bin.py` 只是这个合并过程的手工复现（输出与 MRS 的产物字节完全一致），
   用于 CI 或没有 MRS 的环境，正常用 MRS 时不需要它。

> ⚠️ 不要用 MRS 的 Download 按钮直接烧这个合并 bin：`flashConfig.address` 写的是
> `0x08000000`，而合并 bin 的内容其实从 Flash `0x6000` 开始。日常升级走 IAP 上位机流程。

---

## 4.1 板型变体：一次源码，两个 bin

板型差异全部集中在 `CH32H417_Logic_Analyzer_APP/Common/board_config.h`，通过编译宏切换：

| | `BOARD_WEU6`（默认） | `-DBOARD_QEU6` |
|---|---|---|
| 适用硬件 | CH32H417_Mini（WEU6，已验证） | QEU6 版（作者未验证） |
| UHSIF 数据总线 | `DEF_UHSIF_PINREMAP0` | `DEF_UHSIF_PINREMAP2` |
| UHSIF 水线 | 0 | 1 |
| DAC（VIOH / 阈值） | PA5 / DAC2 | PA4 / DAC1 |
| 模拟输入 | PC2 / PC3（HSADC2/3） | PF8 / PF9（HSADC4/5） |
| LED | PB0 / PB1 | PB3 / PB4 |
| 逻辑与模拟共用 IO 电源 | 是（开 ADC 要把 VIOH 顶到 3.3V） | 否 |
| 上报 `HARDWARE_VERSION` | `0x01` | `0x02` |

`SOFTWARE_VERSION` 统一为 `0x03`（25 档采样 + 10-bit ADC 修复 + 溢出上报）。
上位机按 `HARDWARE_VERSION` 选默认 `adc_ratio`、按 `SOFTWARE_VERSION` 决定可用采样率档数。

### 编两个 bin 的步骤

1. 直接编（不带任何宏）→ WEU6 版 `APP.bin`，改名为 `..._APP_WEU6.bin`；
2. 工程属性 → C/C++ Build → Settings → Preprocessor → Defined symbols 加 `BOARD_QEU6`，
   `Clean` 后重新编 V5F（V3F 不用重编，它没有板型相关代码）→ 得到 `..._APP_QEU6.bin`；
3. 验证时把宏去掉，恢复默认。

V3F 那份只负责唤醒 V5F，与板型无关，两个板型共用同一个 V3F bin（合并时可以直接复用）。

### 共用电源的行为（只对 WEU6 生效）

`HSADC_Function_Start()` 会先把 VIOH 顶到 3.3V，`HSADC_Function_Stop()` 再把用户设的
逻辑阈值恢复回去。因此 **WEU6 打开模拟采集时，16 路逻辑输入的阈值会一起变成 3.3V**，
不能同时使用「1.8V 逻辑 + 3.3V 模拟」。QEU6 版 `BOARD_SHARED_RAIL = 0`，跳过这两步。

---

## 5. Flash 布局

| 镜像 | 核 | 地址 | 实际大小 |
|---|---|---|---|
| IAP | V3F | `0x00000000`–`0x00003DCC` | 15.8 KB（24 KB 区） |
| APP | V3F | `0x00006000`–`0x00008A44` | 10.8 KB（16 KB 区） |
| IAP | V5F | `0x00010000`–`0x00013430` | 13.4 KB（128 KB 区） |
| APP | V5F | `0x00020000`–`0x0002661C` | 26.1 KB（128 KB 区） |

合并镜像 `CH32H417_Logic_Analyzer_APP.bin` 相对 `0x00006000` 的布局：

```
偏移 0x00000   V3F APP      10820 字节
偏移 0x1A000   V5F APP      26140 字节   (0x20000 - 0x6000)
              空隙 0xFF 填充，尾部补齐到 16 字节对齐，共 132640 字节
```

IAP 的 `FLASH_Base = 0x08006000`、`User_APP_Addr_offset = 0x6000`，
正好对应这个合并镜像的起始地址。

---

## 6. 验证记录

已做：

- ✅ 4 个 `.wvproj` 引用的 21 个源文件全部存在（原包缺 1 个）
- ✅ 4 个 `.cproject` / `.project` XML 格式合法
- ✅ 4 个 `.wvproj` JSON 格式合法
- ✅ 4 个工程共 185 个 `.c` 的 `#include "..."` 全部可解析（项目内头文件 0 缺失）
- ✅ `libUHSIF.a` 与缺失 `.o` 的导出符号、外部依赖符号逐一比对一致
- ✅ `tools/make_app_bin.py` 输出与作者发布产物字节一致
- ✅ 目录内无任何遗留 `.o/.d/.map/.lst/.elf/.hex/.bin`
- ✅ 两套板型宏经 `clang -E` 预处理核对，展开值分别落在各自的引脚上：
      WEU6 → PA5/DAC2、PC2/PC3、HSADC2/3、PB0/PB1、remap0、水线0；
      QEU6 → PA4/DAC1、PF8/PF9、HSADC4/5、PB3/PB4、remap2、水线1
- ✅ `ch32h417_logic.c` / `ch32h417_uhsif.c` / `hardware.c` 在两种宏下
      `clang -fsyntax-only` 均 0 error（用 stub 头文件替代 newlib）
- ✅ 上位机侧 `libsigrok` 改动已实际编译并链接通过（macOS，glib 2.90）

**没做**：真实的完整编译。这台 Mac 上没有 WCH 的 `riscv-wch-elf-gcc`
（MRS 只有 Windows/Linux 版），且在 macOS 上装通用 RISC-V GCC 会因为
`__attribute__((interrupt("WCH-Interrupt-fast")))` 这个沁恒私有属性而失败。
所以「能编过」这一条，需要你在 MRS 里实际验证一次。

---

## 7. 许可提醒

这个包里**没有任何 LICENSE 文件**：

- `Q2H2` 署名的文件（`ch32h417_logic.c/.h`、`hardware.c/.h`）：默认保留所有权利
- WCH 署名的文件：仅授权"用于南京沁恒单片机"，不是开源许可
- USB VID `0x1A86` / PID `0x5537` 是沁恒的厂商号，二次分发产品不应沿用

自用、学习、改造没问题；商用或再发布请先联系作者与沁恒。
