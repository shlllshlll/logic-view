/********************************************************************************
 * File Name          : board_config.h
 * Description        : 板型配置（WEU6 Mini / QEU6）
 *
 * 同一份源码，靠编译宏切换板型：
 *   默认          -> BOARD_WEU6  (CH32H417_Mini / WEU6，已验证)
 *   -DBOARD_QEU6  -> QEU6 版
 *
 * 两版的差异只有下面这些，全部收在这一个文件里：
 *   - UHSIF 16 位数据总线的引脚映射（PINREMAP）
 *   - DAC 通道与引脚（决定 VIOH / 逻辑阈值）
 *   - HSADC 通道与模拟输入引脚
 *   - LED 引脚
 *   - 逻辑与模拟是否共用同一路可调 IO 电源（shared_rail）
 *
 * shared_rail = 1 的板子（WEU6）：VIO18 由外部 DAC 经 buck 反馈调节，
 * 16 路逻辑输入和 2 路模拟输入共用这一路。要让 ADC 有 0~3.3V 量程，
 * 必须把 VIOH 顶到 3.3V，逻辑输入阈值因此也一起变成 3.3V；
 * 所以 ADC 启动前要写 DAC，停止后要把用户阈值恢复回去。
 *******************************************************************************/
#ifndef __BOARD_CONFIG_H
#define __BOARD_CONFIG_H

#include "ch32h417.h"
#include "ch32h417_uhsif.h"

/* ---------------- 板型选择 ---------------- */
#if defined(BOARD_QEU6)

#define BOARD_NAME                  "QEU6"
#define BOARD_HW_VERSION            0x02

#define BOARD_UHSIF_PINREMAP        DEF_UHSIF_PINREMAP2
#define BOARD_UHSIF_WATERMARK       1

/* DAC 两个板型都在 GPIOA 上，只有通道和引脚号不同 */
#define BOARD_DAC_PORT              GPIOA
#define BOARD_DAC_PIN               GPIO_Pin_4
#define BOARD_DAC_CH                DAC_Channel_1
#define BOARD_DAC_CH_NUM            1

#define BOARD_ADC_PORT              GPIOF
#define BOARD_ADC_GPIO_CLK          RCC_HB2Periph_GPIOF
#define BOARD_ADC_PIN0              GPIO_Pin_8
#define BOARD_ADC_PIN1              GPIO_Pin_9
#define BOARD_HSADC_CH0             HSADC_Channel_4
#define BOARD_HSADC_CH1             HSADC_Channel_5

/* 两个板型的 LED 都在 GPIOB 上，只有引脚号不同 */
#define BOARD_LED_PINS              (GPIO_Pin_3 | GPIO_Pin_4)

/* QEU6：模拟采样有独立 3.3V IO 电源，与逻辑 IO 不共用 */
#define BOARD_SHARED_RAIL           0

#else  /* ---------------- 默认 WEU6 ---------------- */

#define BOARD_WEU6                  1

#define BOARD_NAME                  "WEU6"
#define BOARD_HW_VERSION            0x01

#define BOARD_UHSIF_PINREMAP        DEF_UHSIF_PINREMAP0
#define BOARD_UHSIF_WATERMARK       0

/* DAC 两个板型都在 GPIOA 上，只有通道和引脚号不同 */
#define BOARD_DAC_PORT              GPIOA
#define BOARD_DAC_PIN               GPIO_Pin_5
#define BOARD_DAC_CH                DAC_Channel_2
#define BOARD_DAC_CH_NUM            2

#define BOARD_ADC_PORT              GPIOC
#define BOARD_ADC_GPIO_CLK          RCC_HB2Periph_GPIOC
#define BOARD_ADC_PIN0              GPIO_Pin_2
#define BOARD_ADC_PIN1              GPIO_Pin_3
#define BOARD_HSADC_CH0             HSADC_Channel_2
#define BOARD_HSADC_CH1             HSADC_Channel_3

/* 两个板型的 LED 都在 GPIOB 上，只有引脚号不同 */
#define BOARD_LED_PINS              (GPIO_Pin_0 | GPIO_Pin_1)

/* WEU6：逻辑与模拟共用一路可调 IO 电源，开 ADC 必须顶到 3.3V */
#define BOARD_SHARED_RAIL           1

#endif /* BOARD_QEU6 */

#endif /* __BOARD_CONFIG_H */
