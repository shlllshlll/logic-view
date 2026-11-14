/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on libsigrok.
 *
 * Copyright (C) 2026 Q2H2
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROK_HARDWARE_WCH_CH32H417_PROTOCOL_H
#define LIBSIGROK_HARDWARE_WCH_CH32H417_PROTOCOL_H

#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"
#include "ch375_wrapper.h"

#define LOG_PREFIX "wch-ch32h417"

#define USB_VENDOR_ID       0x1a86
#define USB_PRODUCT_ID      0x5537

/* USB配置 */
#define USB_INTERFACE       0
#define USB_CONFIGURATION   1
#define USB_TIMEOUT         500


#define PIPE_CMD            1       /* 命令 */
#define PIPE_LOGIC_DATA     2       /* 逻辑分析仪数据 */
#define PIPE_ADC_DATA       3       /* ADC数据 */

#define PACKET_SIZE_OUT         64      /* 下传命令包固定64字节 */
#define PACKET_SIZE_IN          32      /* 上传命令包固定32字节 */

/* 传输参数 */
#define NUM_SIMUL_TRANSFERS     64
#define MAX_EMPTY_TRANSFERS     (NUM_SIMUL_TRANSFERS * 2)

#define BUFFER_UPLOAD_SIZE      (4 * 1024 * 1024)

#define LOGIC_BATCH_TARGET_MS   10

/* 逻辑模式缓冲上传批量下限 (64KB)。 */
#define LOGIC_BUFFER_MIN_SIZE   (64 * 1024)

#define ADC_TRANSFER_SIZE       (1024 * 1024)

/* ADC满速档字节率 (20MHz 10bit = 40MB/s)：该档直接取满上传上限。 */
#define ADC_FULLRATE_BYTE_RATE  (SR_MHZ(20) * 2)

/* ADC模式缓冲上传目标每批时长 (10ms)，与逻辑模式同一套口径。 */
#define ADC_BATCH_TARGET_MS     10

/* ADC模式缓冲上传批量下限 (64KB)。 */
#define ADC_BUFFER_MIN_SIZE     (64 * 1024)

/* ADC上传积压告警起始阈值，之后逐倍提高。 */
#define ADC_BACKLOG_WARN_START  (16UL * 1024 * 1024)

/* 逻辑分析仪命令 */
#define CMD_SET_LOGIC_PARAMS    0xA0
#define CMD_SET_LOGIC_LEVEL     0xA1
#define CMD_START_LOGIC         0xA2
#define CMD_STOP_LOGIC          0xA3
#define CMD_GET_LOGIC_PARAMS    0xA4
#define CMD_GET_LOGIC_LEVEL     0xA5

/* ADC命令 */
#define CMD_SET_ADC_PARAMS      0xA6
#define CMD_SET_ADC_CHANNEL     0xA7
#define CMD_START_ADC           0xA8
#define CMD_STOP_ADC            0xA9
#define CMD_GET_ADC_PARAMS      0xAA
#define CMD_GET_ADC_CHANNEL     0xAB

/* 其他命令 */
#define CMD_GET_VERSION         0xAC
#define CMD_ENTER_IAP           0xAE

/* 响应命令 */
#define RESP_SET_LOGIC_PARAMS   0xB0
#define RESP_SET_LOGIC_LEVEL    0xB1
#define RESP_GET_LOGIC_PARAMS   0xB4
#define RESP_GET_LOGIC_LEVEL    0xB5
#define RESP_SET_ADC_PARAMS     0xB6
#define RESP_SET_ADC_CHANNEL    0xB7
#define RESP_GET_ADC_PARAMS     0xBA
#define RESP_GET_ADC_CHANNEL    0xBB
#define RESP_GET_VERSION        0xBC

/* 设备主动上传命令 (端点1) */
#define CMD_UPLOAD_OVERFLOW     0xBD    /* 上传缓冲区溢出 */
#define OVERFLOW_STATUS_LOGIC   0x01    /* 逻辑RAM溢出 */
#define OVERFLOW_STATUS_ADC     0x02    /* ADC RAM溢出 */

/* 采样位宽 */
#define SAMPLE_WIDTH_8BIT       0x08
#define SAMPLE_WIDTH_16BIT      0x10

/* ADC位宽 */
#define ADC_WIDTH_8BIT          0x08
#define ADC_WIDTH_10BIT         0x0A

/* 设备能力标志 */
#define DEV_CAPS_16BIT_LOGIC    (1 << 0)
#define DEV_CAPS_ADC            (1 << 1)
#define DEV_CAPS_ADC_10BIT      (1 << 2)

/*
 * 板型 —— 对应固件 CMD_GET_VERSION 应答里的 HARDWARE_VERSION 字节。
 * 固件侧见 board_config.h：WEU6 报 0x01，QEU6 报 0x02。
 */
#define BOARD_HW_WEU6_MINI      0x01    /* 模拟直采无分压，逻辑与模拟共用 IO 电源 */
#define BOARD_HW_QEU6           0x02    /* 带 1.53:1 分压，模拟供电独立 */

/* 固件 SOFTWARE_VERSION >= 该值才支持 25 档采样率 */
#define FW_SW_SAMPLERATE_25     0x03

/* 工作模式 */
enum ch32h417_mode {
	MODE_LOGIC_ANALYZER,
	MODE_ADC,
};

/* 电压电平 */
enum voltage_level {
	VOLTAGE_3_3V = 0,
	VOLTAGE_2_5V = 1,
	VOLTAGE_2_0V = 2,
	VOLTAGE_1_8V = 3,
	VOLTAGE_1_5V = 4,
	VOLTAGE_1_2V = 5,
};

/* ADC通道 */
enum adc_channel {
	ADC_CHANNEL_1 = 0,
	ADC_CHANNEL_2 = 1,
};

/*
 * ADC软件电平触发上下文。
 * 仿 soft_trigger_logic 的结构，但作用于模拟电压：逐样本将原始ADC字节换算为电压，
 * 与触发门限(match->value)按触发方向(OVER/UNDER/RISING/FALLING/EDGE)比较。
 * 预触发数据以原始字节形式存入环形缓冲，命中时经 send_data_proc 转 float 发送。
 */
struct ch32h417_analog_trigger {
	const struct sr_trigger *trigger;
	int unitsize;            /* ADC每样本字节数: 8bit=1, 10bit=2 */
	float prev_value;        /* 上一样本电压(用于边沿检测) */
	gboolean have_prev;      /* prev_value 是否有效 */
	uint8_t *pre_trigger_buffer;  /* 预触发环形缓冲 */
	uint8_t *pre_trigger_head;    /* 环形缓冲写指针 */
	int pre_trigger_size;         /* 环形缓冲总容量(字节) */
	int pre_trigger_fill;         /* 已填充字节数 */
	int cur_stage;           /* 当前触发stage */
	int count;               /* 已检查样本计数 */
};

/* 设备配置文件 */
struct ch32h417_profile {
	uint16_t vid;
	uint16_t pid;
	const char *vendor;
	const char *model;
	uint32_t dev_caps;
};

/* 设备上下文 */
struct dev_context {
	const struct ch32h417_profile *profile;

	/* 工作模式 */
	enum ch32h417_mode mode;

	/* 采样率相关 */
	const uint64_t *samplerates;
	int num_samplerates;
	uint64_t cur_samplerate;

	/* 逻辑分析仪配置 */
	uint64_t limit_samples;
	uint64_t limit_frames;
	uint64_t capture_ratio;
	uint64_t num_frames;
	uint64_t sent_samples;
	uint8_t sample_width;      /* 8bit or 16bit */
	uint8_t num_channels;      /* 8 or 16 channels */
	enum voltage_level voltage_level;
	uint16_t threshold_value_1;  /* DAC阈值原始值 (0-1024) */

	/* ADC配置 */
	uint8_t adc_width;         /* 8bit or 10bit */
	enum adc_channel adc_channel;
	float adc_ratio;           /* ADC衰减比 (输入电压 / ADC引脚电压) */
	float adc_bias;            /* ADC偏置 (V) */
	GSList *enabled_analog_channels;

	/* 触发相关 */
	gboolean trigger_fired;
	gboolean acq_aborted;
	struct soft_trigger_logic *stl;
	struct ch32h417_analog_trigger *atrig;  /* ADC软件电平触发上下文 */

	/* 固件版本*/
	uint16_t fw_version;

	/* USB速度 */
	gboolean is_usb2;

	/* CH375设备相关 */
	int device_index;          /* 设备索引 */
	void *device_handle;       /* 设备句柄 */
	
	/* 数据读取线程 */
	GThread *read_thread;      /* 读取线程 */
	GMutex mutex;              /* 互斥锁 */
	GCond cond;                /* 条件变量 */

	/* 数据缓冲 */
	uint8_t *logic_buffer;
	float *adc_float_buffer;
	size_t buffer_size;        /* 读取缓冲区大小 */

	/* 采集统计 */
	gint64 stats_start_time_us;       /* 采集开始时间 */
	uint64_t stats_bytes_read;        /* 累计从设备读取的字节数 */
	unsigned long stats_max_pending_bytes;  /* 最大待读字节数 */
	unsigned long stats_max_packet_count;   /* 最大当前数据包数 */

	/* 硬件溢出状态 (端点1 0xBD上传命令) */
	gboolean overflow_occurred;  /* 采集期间是否发生硬件溢出 */
	uint8_t overflow_status;     /* 溢出状态: OVERFLOW_STATUS_LOGIC/OVERFLOW_STATUS_ADC */

	void (*send_data_proc)(struct sr_dev_inst *sdi,
		uint8_t *data, size_t length, size_t sample_width);
};

/* 协议函数声明 */
SR_PRIV int ch32h417_send_command(struct sr_dev_inst *sdi,
	uint8_t cmd, uint8_t len, const uint8_t *data);
SR_PRIV int ch32h417_receive_response(struct sr_dev_inst *sdi,
	uint8_t expected_cmd, uint8_t *data, int *len);

SR_PRIV int ch32h417_set_logic_params(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_set_threshold_value(const struct sr_dev_inst *sdi, uint16_t value);
SR_PRIV int ch32h417_get_logic_params(const struct sr_dev_inst *sdi,
	uint8_t *width, uint8_t *div);
SR_PRIV int ch32h417_get_logic_level(const struct sr_dev_inst *sdi,
	uint8_t *level);

SR_PRIV int ch32h417_set_adc_params(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_set_adc_channel(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_get_adc_params(const struct sr_dev_inst *sdi,
	uint8_t *width, uint8_t *div);
SR_PRIV int ch32h417_get_adc_channel(const struct sr_dev_inst *sdi,
	uint8_t *channel);

SR_PRIV int ch32h417_start_logic(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_stop_logic(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_start_adc(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_stop_adc(const struct sr_dev_inst *sdi);

SR_PRIV struct dev_context *ch32h417_dev_new(void);
SR_PRIV gboolean ch32h417_detect_usb_speed(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_get_fw_version(const struct sr_dev_inst *sdi);
SR_PRIV void ch32h417_apply_board_defaults(struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_acquisition_start(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_acquisition_stop(const struct sr_dev_inst *sdi);
SR_PRIV int ch32h417_enter_iap(const struct sr_dev_inst *sdi);

/* ADC软件电平触发 */
SR_PRIV struct ch32h417_analog_trigger *ch32h417_analog_trigger_new(
	const struct sr_dev_inst *sdi, struct sr_trigger *trigger,
	int pre_trigger_samples);
SR_PRIV void ch32h417_analog_trigger_free(struct ch32h417_analog_trigger *at);
SR_PRIV int ch32h417_analog_trigger_check(struct sr_dev_inst *sdi,
	uint8_t *buf, int len, int *pre_trigger_samples);
SR_PRIV gboolean ch32h417_analog_trigger_has_match(struct dev_context *devc,
	struct sr_trigger *trigger);

#endif /* LIBSIGROK_HARDWARE_WCH_CH32H417_PROTOCOL_H */
