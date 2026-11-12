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
| 未动 | 所有 `.c` / `.h` / `.S` / `.ld` 源码，一个字节都没改 |

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
3. 合并出升级镜像：

   ```bash
   python3 tools/make_app_bin.py
   # -> CH32H417_Logic_Analyzer_APP/V5F/obj/CH32H417_Logic_Analyzer_APP.bin
   ```

   这个合并文件**工程里没有自动后处理步骤**（作者包里那份应该是手动做的），
   脚本已验证与作者发布的 `CH32H417_Logic_Analyzer_APP.bin` **字节完全一致**。

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
