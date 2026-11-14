# 硬件设计文件（原理图 / PCB）

`CH32H417_Mini_LogicAnalyzer_2026-11-14.epro2` 是 CH32H417 Mini 逻辑分析仪的
**EasyEDA Pro（立创EDA 专业版）** 工程包，包含原理图、PCB 与封装。

## 打开方式

立创EDA 专业版 → `文件` → `打开` → 选择 `.epro2` 文件；也可以直接双击该文件
（需要已安装立创EDA 专业版）。

## 克隆说明

`.epro2` 是 deflate 压缩包，Git 无法做 delta，每改一版就是完整新增 1 MB 左右，
所以用 **Git LFS** 管理（规则见仓库根目录 `.gitattributes`）。没装 Git LFS 时，
克隆下来的是一个 132 字节的指针文件，打开会失败：

```bash
brew install git-lfs && git lfs install   # 一次性
git lfs pull                              # 已有仓库里补拉
```

## 与固件的对应关系

板型通过固件编译宏区分，差异全部收在
[`firmware/CH32H417_Logic_Analyzer_APP/Common/board_config.h`](../firmware/CH32H417_Logic_Analyzer_APP/Common/board_config.h)：

| 板型 | 编译宏 | `HARDWARE_VERSION` | 模拟输入 |
|---|---|---|---|
| WEU6 Mini（默认） | 无 | `0x01` | 与逻辑共用可调 IO 电源，满量程 3.3 V |
| QEU6 | `-DBOARD_QEU6` | `0x02` | 独立 3.3 V IO 电源，带 1.53:1 分压，满量程约 5 V |

上位机在 `dev_open` 时读取固件版本，按这个字节自动选择 `adc_ratio` 等默认值，
见 `libsigrok/src/hardware/wch-ch32h417/protocol.c` 的
`ch32h417_apply_board_defaults()`。

## 说明

- 原始文件名是 `ProPrj_CH32H417_Mini逻辑分析仪_2026-11-14.epro2`，为了在命令行、
  CI 和 URL 里不出现空格与中文，入库时改成了 ASCII 名字，内容未做任何修改。
- 这是二进制工程包，diff 无法阅读；更新时整体替换并同时改文件名里的日期。
- 本目录未单独声明许可，分发前请与仓库其余部分一并确认。
