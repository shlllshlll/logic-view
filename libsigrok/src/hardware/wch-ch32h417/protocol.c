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

#include <config.h>
#include "protocol.h"
#include <math.h>

/* 逻辑分析仪采样率 - USB 3.0 (最高200MHz) */
static const uint64_t logic_samplerates_usb3[] = {
	SR_MHZ(200),
	SR_MHZ(187.5),
	SR_MHZ(175),
	SR_MHZ(162.5),
	SR_MHZ(156.25),
	SR_MHZ(150),
	SR_MHZ(137.5),
	SR_MHZ(125),
	SR_MHZ(112.5),
	SR_MHZ(100),
	SR_MHZ(93.75),
	SR_MHZ(87.5),
	SR_MHZ(81.5),
	SR_MHZ(75.125),
	SR_MHZ(75),
	SR_MHZ(68.75),
	SR_MHZ(50),
	SR_MHZ(40),
	SR_MHZ(25),
	SR_MHZ(20),
	SR_MHZ(16),
	SR_MHZ(12.5),
	SR_MHZ(10),
	SR_MHZ(8),
	SR_MHZ(6.25),
};

/* 逻辑分析仪USB 2.0 16通道 (最高20MHz) */
static const uint64_t logic_samplerates_usb2_16ch[] = {
	SR_MHZ(20),
};

/* 逻辑分析仪USB 2.0 8通道 (最高40MHz) */
static const uint64_t logic_samplerates_usb2_8ch[] = {
	SR_MHZ(40),
};

/* ADC采样率 最高20MHz */
static const uint64_t adc_samplerates[] = {
	SR_MHZ(20),
	SR_MHZ(16),
	SR_MHZ(13.33),
	SR_MHZ(11.43),
	SR_MHZ(10),
};

static uint8_t resp_buffer[PACKET_SIZE_IN];
static int resp_len = 0;

SR_PRIV int ch32h417_send_command(struct sr_dev_inst *sdi,
								  uint8_t cmd, uint8_t len, const uint8_t *data)
{
	struct dev_context *devc;
	uint8_t out_buffer[PACKET_SIZE_OUT] = {0};
	unsigned long io_length;

	devc = sdi->priv;

	if (len > (PACKET_SIZE_OUT - 2))
	{
		sr_err("Command data too long: %d > %d", len, PACKET_SIZE_OUT - 2);
		return SR_ERR_ARG;
	}

	out_buffer[0] = cmd;
	out_buffer[1] = len;
	if (len > 0 && data)
		memcpy(&out_buffer[2], data, len);

	io_length = PACKET_SIZE_OUT;

	if (!ch375_write_endpoint(devc->device_index, PIPE_CMD, out_buffer, &io_length))
	{
		sr_err("CH375WriteEndP failed for command 0x%02x", cmd);
		return SR_ERR;
	}

	sr_dbg("Sent command 0x%02x, len=%d, written=%lu bytes", cmd, len, io_length);

	return SR_OK;
}

SR_PRIV int ch32h417_receive_response(struct sr_dev_inst *sdi,
									  uint8_t expected_cmd, uint8_t *data, int *len)
{
	struct dev_context *devc;
	unsigned long io_length;

	devc = sdi->priv;

	/* 读取响应。采集结束后端点1中可能残留硬件主动上传的溢出命令
	 * (0xBD+len+status)，需跳过它继续读取真实响应，并记录溢出标志，
	 * 供GUI在采集结束后查询提示。 */
	for (int attempt = 0; attempt < 4; attempt++)
	{
		io_length = PACKET_SIZE_IN;

		if (!ch375_read_endpoint(devc->device_index, PIPE_CMD, resp_buffer, &io_length))
		{
			sr_err("CH375ReadEndP failed for response");
			return SR_ERR;
		}

		resp_len = (int)io_length;

		/* 硬件溢出命令: 0xBD + len + status */
		if (resp_len >= 3 && resp_buffer[0] == CMD_UPLOAD_OVERFLOW)
		{
			sr_warn("Hardware overflow detected on EP1, status=0x%02x", resp_buffer[2]);
			g_mutex_lock(&devc->mutex);
			devc->overflow_occurred = TRUE;
			devc->overflow_status = resp_buffer[2];
			g_mutex_unlock(&devc->mutex);
			continue; /* 跳过溢出命令，继续读取真实响应 */
		}
		break;
	}

	if (resp_len < 2)
	{
		sr_err("Response too short: %d bytes", resp_len);
		return SR_ERR;
	}

	if (resp_buffer[0] != expected_cmd)
	{
		sr_err("Unexpected response: 0x%02x (expected 0x%02x)",
			   resp_buffer[0], expected_cmd);
		return SR_ERR;
	}

	uint8_t resp_data_len = resp_buffer[1];
	if (resp_data_len > 0 && resp_buffer[2] != 0x00)
	{
		sr_err("Command failed with status 0x%02x", resp_buffer[2]);
		return SR_ERR;
	}

	if (len)
		*len = resp_data_len;
	if (data && resp_data_len > 1)
		memcpy(data, &resp_buffer[3], resp_data_len - 1);

	sr_dbg("Received response 0x%02x, len=%d", resp_buffer[0], resp_data_len);

	return SR_OK;
}

SR_PRIV int ch32h417_set_logic_params(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	uint8_t params[2];
	int ret;

	devc = sdi->priv;

	/* 采样位宽 */
	params[0] = devc->sample_width;

	/* 分频系数 */
	int div = 0;
	while (div < devc->num_samplerates &&
		   devc->samplerates[div] != devc->cur_samplerate)
	{
		div++;
	}
	if (div >= devc->num_samplerates)
	{
		sr_err("Invalid samplerate: %" PRIu64, devc->cur_samplerate);
		return SR_ERR_ARG;
	}
	params[1] = (uint8_t)div;

	sr_info("Setting logic params: width=0x%02x, div=%d", params[0], params[1]);

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_SET_LOGIC_PARAMS, 2, params);
	if (ret != SR_OK)
		return ret;

	return ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_SET_LOGIC_PARAMS, NULL, NULL);
}

/*
 * 设置DAC阈值
 * 发送两字节DAC值：buf[0] = 高位, buf[1] = 低位
 * 值范围：0-1024，0对应约3.3V，1024对应约1.2V
 */
SR_PRIV int ch32h417_set_threshold_value(const struct sr_dev_inst *sdi, uint16_t value)
{
	uint8_t params[2];
	int ret;

	params[0] = (value >> 8) & 0xFF; /* 高位 */
	params[1] = value & 0xFF;		 /* 低位 */

	sr_info("Setting DAC threshold: 0x%04x (%d)", value, value);

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_SET_LOGIC_LEVEL, 2, params);
	if (ret != SR_OK)
		return ret;

	return ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_SET_LOGIC_LEVEL, NULL, NULL);
}

/*
 * 获取逻辑分析仪参数
 */
SR_PRIV int ch32h417_get_logic_params(const struct sr_dev_inst *sdi,
									  uint8_t *width, uint8_t *div)
{
	uint8_t data[2];
	int len;
	int ret;
	gint64 t1, t2;

	t1 = g_get_monotonic_time();
	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_GET_LOGIC_PARAMS, 0, NULL);
	t2 = g_get_monotonic_time();
	sr_dbg("ch32h417_get_logic_params: send_command took %" G_GINT64_FORMAT " us", t2 - t1);
	if (ret != SR_OK)
		return ret;

	t1 = g_get_monotonic_time();
	ret = ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_GET_LOGIC_PARAMS, data, &len);
	t2 = g_get_monotonic_time();
	sr_dbg("ch32h417_get_logic_params: receive_response took %" G_GINT64_FORMAT " us", t2 - t1);
	if (ret != SR_OK)
		return ret;

	if (width)
		*width = data[0];
	if (div)
		*div = data[1];

	sr_dbg("Got logic params: width=0x%02x, div=%d", data[0], data[1]);

	return SR_OK;
}

/*
 * 获取逻辑分析仪电压电平
 */
SR_PRIV int ch32h417_get_logic_level(const struct sr_dev_inst *sdi,
									 uint8_t *level)
{
	uint8_t data;
	int len;
	int ret;

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_GET_LOGIC_LEVEL, 0, NULL);
	if (ret != SR_OK)
		return ret;

	ret = ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_GET_LOGIC_LEVEL, &data, &len);
	if (ret != SR_OK)
		return ret;

	if (level)
		*level = data;

	sr_dbg("Got logic level: %d", data);

	return SR_OK;
}

/*
 * 设置ADC参数
 * Buf[0]: 0x8=8位ADC, 0x0a=10位ADC
 * Buf[1]: 分频系数 (0=20M, 1=10M, ..., 最多64分频)
 */
SR_PRIV int ch32h417_set_adc_params(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	uint8_t params[2];
	int ret;

	devc = sdi->priv;

	params[0] = devc->adc_width;

	/* 计算分频系数 */
	int div = 0;
	while (div < ARRAY_SIZE(adc_samplerates) &&
		   adc_samplerates[div] != devc->cur_samplerate)
	{
		div++;
	}
	if (div >= ARRAY_SIZE(adc_samplerates))
	{
		sr_err("Invalid ADC samplerate: %" PRIu64, devc->cur_samplerate);
		return SR_ERR_ARG;
	}
	div += 3;
	params[1] = (uint8_t)div;

	sr_info("Setting ADC params: width=0x%02x, div=%d", params[0], params[1]);

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_SET_ADC_PARAMS, 2, params);
	if (ret != SR_OK)
		return ret;

	return ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_SET_ADC_PARAMS, NULL, NULL);
}

/*
 * 设置ADC通道
 * Buf[0]: 0=通道1, 1=通道2
 */
SR_PRIV int ch32h417_set_adc_channel(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	uint8_t param;
	int ret;

	devc = sdi->priv;

	param = (uint8_t)devc->adc_channel;

	sr_info("Setting ADC channel: %d", param);

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_SET_ADC_CHANNEL, 1, &param);
	if (ret != SR_OK)
		return ret;

	return ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_SET_ADC_CHANNEL, NULL, NULL);
}

/*
 * 获取ADC参数
 */
SR_PRIV int ch32h417_get_adc_params(const struct sr_dev_inst *sdi,
									uint8_t *width, uint8_t *div)
{
	uint8_t data[2];
	int len;
	int ret;

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_GET_ADC_PARAMS, 0, NULL);
	if (ret != SR_OK)
		return ret;

	ret = ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_GET_ADC_PARAMS, data, &len);
	if (ret != SR_OK)
		return ret;

	if (width)
		*width = data[0];
	if (div)
		*div = data[1];

	return SR_OK;
}

/*
 * 获取ADC通道
 */
SR_PRIV int ch32h417_get_adc_channel(const struct sr_dev_inst *sdi,
									 uint8_t *channel)
{
	uint8_t data;
	int len;
	int ret;

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_GET_ADC_CHANNEL, 0, NULL);
	if (ret != SR_OK)
		return ret;

	ret = ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_GET_ADC_CHANNEL, &data, &len);
	if (ret != SR_OK)
		return ret;

	if (channel)
		*channel = data;

	return SR_OK;
}

/*
 * 开始逻辑分析仪采集
 */
SR_PRIV int ch32h417_start_logic(const struct sr_dev_inst *sdi)
{
	sr_info("Starting logic acquisition");
	return ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_START_LOGIC, 0, NULL);
}

/*
 * 停止逻辑分析仪采集
 */
SR_PRIV int ch32h417_stop_logic(const struct sr_dev_inst *sdi)
{
	sr_info("Stopping logic acquisition");
	return ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_STOP_LOGIC, 0, NULL);
}

/*
 * 开始ADC采集
 */
SR_PRIV int ch32h417_start_adc(const struct sr_dev_inst *sdi)
{
	sr_info("Starting ADC acquisition");
	return ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_START_ADC, 0, NULL);
}

/*
 * 停止ADC采集
 */
SR_PRIV int ch32h417_stop_adc(const struct sr_dev_inst *sdi)
{
	sr_info("Stopping ADC acquisition");
	return ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_STOP_ADC, 0, NULL);
}

/*
 * 进入IAP模式（固件升级模式）
 * 命令: 0xAE + len(0)
 * 设备收到此命令后会进入IAP模式，可用于固件升级
 */
SR_PRIV int ch32h417_enter_iap(const struct sr_dev_inst *sdi)
{
	sr_info("Entering IAP mode for firmware upgrade");
	return ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_ENTER_IAP, 0, NULL);
}

/* 发送停止命令并发送SR_DF_END */
static void send_stop_command(const struct sr_dev_inst *sdi)
{
	int ret = -1;
	struct dev_context *devc = sdi->priv;

	/* 发送停止命令 */
	if (devc->mode == MODE_LOGIC_ANALYZER)
	{
		ret = ch32h417_stop_logic(sdi);
		if (ret != SR_OK)
		{
			sr_warn("Failed to send stop logic command on finish.");
		}
	}
	else
	{
		ret = ch32h417_stop_adc(sdi);
		if (ret != SR_OK)
		{
			sr_warn("Failed to send stop ADC command on finish.");
		}
	}

	/* 清除缓冲上传缓存 */
	unsigned long pipe_num = (devc->mode == MODE_LOGIC_ANALYZER) ? PIPE_LOGIC_DATA : PIPE_ADC_DATA;
	if (!ch375_clear_buf_upload(devc->device_index, pipe_num))
	{
		sr_warn("Failed to clear upload buffer pipe %lu for device at index %d", pipe_num, devc->device_index);
	}
}

/*
 * 创建设备上下文
 */
SR_PRIV struct dev_context *ch32h417_dev_new(void)
{
	struct dev_context *devc;

	devc = g_malloc0(sizeof(struct dev_context));
	devc->mode = MODE_LOGIC_ANALYZER;
	devc->cur_samplerate = logic_samplerates_usb3[0];
	devc->limit_samples = 0;
	devc->limit_frames = 1;
	devc->capture_ratio = 0;
	devc->sample_width = SAMPLE_WIDTH_16BIT;
	devc->num_channels = 16; /* 默认16通道 */
	devc->voltage_level = VOLTAGE_3_3V;
	devc->adc_width = ADC_WIDTH_10BIT;
	devc->adc_channel = ADC_CHANNEL_1;
	devc->adc_ratio = 0.6526f;
	devc->adc_bias = 0.0f;
	devc->fw_version = 0;
	devc->is_usb2 = FALSE;
	devc->samplerates = logic_samplerates_usb3;
	devc->num_samplerates = ARRAY_SIZE(logic_samplerates_usb3);

	/* CH375相关初始化 */
	devc->device_index = -1;
	devc->device_handle = NULL;
	devc->buffer_size = BUFFER_UPLOAD_SIZE;

	g_mutex_init(&devc->mutex);
	g_cond_init(&devc->cond);

	return devc;
}

/*
 * 检测USB速度（通过设备描述符中的bcdUSB字段）
 * USB设备描述符结构：
 *   偏移0: bLength (描述符长度)
 *   偏移1: bDescriptorType (描述符类型)
 *   偏移2-3: bcdUSB (USB版本，BCD编码)
 *   ...
 * 返回: TRUE表示USB 2.0，FALSE表示USB 3.0
 */
SR_PRIV gboolean ch32h417_detect_usb_speed(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	uint8_t descr[18]; /* USB设备描述符固定18字节 */
	unsigned long length;
	uint16_t bcd_usb;

	devc = sdi->priv;

	g_usleep(50000);

	length = sizeof(descr);
	if (!ch375_get_device_descr(devc->device_index, descr, &length))
	{
		sr_warn("Failed to get device descriptor, assuming USB 3.0");
		return FALSE; /* 默认USB 3.0 */
	}

	if (length < 4)
	{
		sr_warn("Device descriptor too short (%lu bytes), assuming USB 3.0", length);
		return FALSE;
	}

	/* bcdUSB字段位于偏移2-3，小端序 */
	bcd_usb = descr[2] | (descr[3] << 8);

	sr_info("Device descriptor: bcdUSB=0x%04x", bcd_usb);

	/* USB 2.0: bcdUSB = 0x0200
	 * USB 3.0: bcdUSB = 0x0300
	 */
	if (bcd_usb >= 0x0300)
	{
		sr_info("USB 3.0 device detected (bcdUSB=0x%04x)", bcd_usb);
		return FALSE;
	}
	else
	{
		sr_info("USB 2.0 device detected (bcdUSB=0x%04x)", bcd_usb);
		return TRUE;
	}
}

/*
 * 获取固件版本号
 * CH32H417无FPGA，只有固件版本
 * 命令: 0xAC + len(0)
 * 返回: 0xBC + len(2) + Status(0x00) + buf[0](固件版本) + buf[1](硬件版本)
 */
SR_PRIV int ch32h417_get_fw_version(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	uint8_t data[2] = {0};
	int len;
	int ret;

	devc = sdi->priv;

	ret = ch32h417_send_command((struct sr_dev_inst *)sdi, CMD_GET_VERSION, 0, NULL);
	if (ret != SR_OK)
	{
		sr_err("Failed to send get version command");
		return ret;
	}

	ret = ch32h417_receive_response((struct sr_dev_inst *)sdi, RESP_GET_VERSION, data, &len);
	if (ret != SR_OK)
	{
		sr_err("Failed to receive version response");
		return ret;
	}

	if (len >= 2)
	{
		devc->fw_version = (data[0] << 8) | data[1];
		sr_info("Firmware version: %d, Hardware version: %d", data[0], data[1]);
	}
	else
	{
		sr_warn("Version response too short: %d", len);
		devc->fw_version = 0;
	}

	sr_dbg("Firmware version: 0x%04x", devc->fw_version);

	return SR_OK;
}

static int acquisition_check_cb(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi = cb_data;
	struct dev_context *devc = sdi->priv;

	(void)fd;
	(void)revents;

	g_mutex_lock(&devc->mutex);
	gboolean should_stop = devc->acq_aborted;
	g_mutex_unlock(&devc->mutex);

	if (should_stop)
	{
		sr_dbg("acquisition_check_cb: thread requested stop");
		sr_dev_acquisition_stop(sdi);
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static gpointer stop_command_thread_func(gpointer data)
{
	struct sr_dev_inst *sdi = data;
	send_stop_command(sdi);
	return NULL;
}

/* 发送逻辑分析仪数据 */
static void la_send_data_proc(struct sr_dev_inst *sdi,
							  uint8_t *data, size_t length, size_t unitsize)
{
	const struct sr_datafeed_logic logic = {
		.length = length,
		.unitsize = unitsize,
		.data = data};

	const struct sr_datafeed_packet packet = {
		.type = SR_DF_LOGIC,
		.payload = &logic};

	sr_session_send(sdi, &packet);
}

/* 将单个ADC原始样本换算为电压值(V)。
 * 10bit: 读取16位值(取低10位); 8bit: 读取8位值。
 * 换算公式与硬件分压及参考电压一致。 */
static inline float adc_raw_to_voltage(struct dev_context *devc, const uint8_t *sample)
{
	if (devc->adc_width == ADC_WIDTH_10BIT)
	{
		uint16_t raw = *((const uint16_t *)sample);
		return (float)(raw & 0x3FF) * 3.3f / 1024.0f / devc->adc_ratio + devc->adc_bias;
	}
	else
	{
		return (float)(*sample) * 3.3f / 256.0f / devc->adc_ratio + devc->adc_bias;
	}
}

/* 发送ADC数据 */
static void adc_send_data_proc(struct sr_dev_inst *sdi,
							   uint8_t *data, size_t length, size_t sample_width)
{
	struct dev_context *devc;
	struct sr_datafeed_analog analog;
	struct sr_analog_encoding encoding;
	struct sr_analog_meaning meaning;
	struct sr_analog_spec spec;
	float *float_data;
	float scale, bias;
	size_t sample_bytes;
	size_t offset;

	devc = sdi->priv;
	(void)sample_width;

	/* 8位ADC每样本1字节，10位ADC每样本2字节。 */
	sample_bytes = (devc->adc_width == ADC_WIDTH_10BIT) ? 2 : 1;

	/* 转换缓冲区在采集启动时按读取块预分配并复用。 */
	if (!devc->adc_float_buffer)
	{
		sr_err("ADC conversion buffer is not allocated");
		devc->acq_aborted = TRUE;
		return;
	}
	float_data = devc->adc_float_buffer;

	sr_analog_init(&analog, &encoding, &meaning, &spec, 2);
	analog.meaning->channels = devc->enabled_analog_channels;
	analog.meaning->mq = SR_MQ_VOLTAGE;
	analog.meaning->unit = SR_UNIT_VOLT;
	analog.meaning->mqflags = 0;
	analog.data = float_data;

	const struct sr_datafeed_packet packet = {
		.type = SR_DF_ANALOG,
		.payload = &analog};

	/* 预计算比例和偏置，避免在样本循环中重复除法。 */
	bias = devc->adc_bias;
	scale = 3.3f / devc->adc_ratio;
	if (devc->adc_width == ADC_WIDTH_10BIT)
		scale /= 1024.0f;
	else
		scale /= 256.0f;

	for (offset = 0; offset < length;)
	{
		size_t chunk_length = length - offset;
		size_t num_samples;
		size_t i;

		if (chunk_length > devc->buffer_size)
			chunk_length = devc->buffer_size;
		chunk_length -= chunk_length % sample_bytes;
		if (chunk_length == 0)
			break;

		num_samples = chunk_length / sample_bytes;
		if (devc->adc_width == ADC_WIDTH_10BIT)
		{
			for (i = 0; i < num_samples; i++)
			{
				uint16_t raw = *((const uint16_t *)(data + offset + i * sample_bytes));
				float_data[i] = (float)(raw & 0x3FF) * scale + bias;
			}
		}
		else
		{
			for (i = 0; i < num_samples; i++)
				float_data[i] =
					(float)data[offset + i] * scale + bias;
		}

		analog.num_samples = num_samples;
		sr_session_send(sdi, &packet);

		offset += chunk_length;
	}
}

/* ---- ADC软件电平触发 ---- */

SR_PRIV struct ch32h417_analog_trigger *ch32h417_analog_trigger_new(
	const struct sr_dev_inst *sdi, struct sr_trigger *trigger,
	int pre_trigger_samples)
{
	struct dev_context *devc = sdi->priv;
	struct ch32h417_analog_trigger *at;

	at = g_malloc0(sizeof(*at));
	at->trigger = trigger;
	at->unitsize = (devc->adc_width == ADC_WIDTH_10BIT) ? 2 : 1;
	at->have_prev = FALSE;
	at->prev_value = 0.0f;
	at->cur_stage = 0;
	at->count = 0;
	at->pre_trigger_size = at->unitsize * pre_trigger_samples;
	at->pre_trigger_fill = 0;
	if (at->pre_trigger_size > 0)
	{
		at->pre_trigger_buffer = g_try_malloc(at->pre_trigger_size);
		if (!at->pre_trigger_buffer)
		{
			g_free(at);
			return NULL;
		}
	}
	else
	{
		at->pre_trigger_buffer = NULL;
	}
	at->pre_trigger_head = at->pre_trigger_buffer;

	return at;
}

SR_PRIV void ch32h417_analog_trigger_free(struct ch32h417_analog_trigger *at)
{
	if (!at)
		return;
	g_free(at->pre_trigger_buffer);
	g_free(at);
}

/* 检查session trigger是否含"已启用模拟通道上的模拟触发match"。
 * 用于ADC模式启动时判断是否真正需要软件触发，避免残留的逻辑通道触发导致空等/误触发。 */
SR_PRIV gboolean ch32h417_analog_trigger_has_match(struct dev_context *devc,
												   struct sr_trigger *trigger)
{
	GSList *l_stage, *l_match;

	(void)devc;

	if (!trigger)
		return FALSE;

	for (l_stage = trigger->stages; l_stage; l_stage = l_stage->next)
	{
		struct sr_trigger_stage *stage = l_stage->data;
		if (!stage->matches)
			continue;
		for (l_match = stage->matches; l_match; l_match = l_match->next)
		{
			struct sr_trigger_match *m = l_match->data;
			if (!m->channel || !m->channel->enabled)
				continue;
			if (m->channel->type != SR_CHANNEL_ANALOG)
				continue;
			switch (m->match)
			{
			case SR_TRIGGER_OVER:
			case SR_TRIGGER_UNDER:
			case SR_TRIGGER_RISING:
			case SR_TRIGGER_FALLING:
			case SR_TRIGGER_EDGE:
				return TRUE;
			default:
				break;
			}
		}
	}

	return FALSE;
}

/* 判断单样本电压是否满足触发match(按模拟方向比较) */
static gboolean analog_check_match(struct ch32h417_analog_trigger *at,
								   float cur, struct sr_trigger_match *match)
{
	float v = match->value;

	at->count++;
	switch (match->match)
	{
	case SR_TRIGGER_OVER:
		return cur > v;
	case SR_TRIGGER_UNDER:
		return cur < v;
	case SR_TRIGGER_RISING:
		if (!at->have_prev)
			return FALSE;
		return at->prev_value <= v && cur > v;
	case SR_TRIGGER_FALLING:
		if (!at->have_prev)
			return FALSE;
		return at->prev_value >= v && cur < v;
	case SR_TRIGGER_EDGE:
		if (!at->have_prev)
			return FALSE;
		return (at->prev_value <= v && cur > v) ||
			   (at->prev_value >= v && cur < v);
	default:
		return FALSE;
	}
}

/* 将预触发原始字节追加到环形缓冲(仿 soft-trigger.c 的 pre_trigger_append) */
static void analog_pre_trigger_append(struct ch32h417_analog_trigger *at,
									  uint8_t *buf, int len)
{
	if (len <= 0 || !at->pre_trigger_buffer || at->pre_trigger_size <= 0)
		return;

	/* 不必拷贝超过环形缓冲容量的数据 */
	if (len > at->pre_trigger_size)
	{
		buf += len - at->pre_trigger_size;
		len = at->pre_trigger_size;
	}

	at->pre_trigger_fill = MIN(at->pre_trigger_fill + len, at->pre_trigger_size);

	while (len > 0)
	{
		size_t size = MIN((size_t)(at->pre_trigger_buffer + at->pre_trigger_size - at->pre_trigger_head), (size_t)len);
		memcpy(at->pre_trigger_head, buf, size);
		at->pre_trigger_head += size;
		if (at->pre_trigger_head >= at->pre_trigger_buffer + at->pre_trigger_size)
			at->pre_trigger_head = at->pre_trigger_buffer;
		buf += size;
		len -= size;
	}
}

/* 将预触发环形缓冲中的原始字节经 send_data_proc 转 float 后发送(SR_DF_ANALOG) */
static void analog_pre_trigger_send(struct sr_dev_inst *sdi,
									struct ch32h417_analog_trigger *at, int *pre_trigger_samples)
{
	struct dev_context *devc = sdi->priv;

	if (pre_trigger_samples)
		*pre_trigger_samples = 0;

	if (!at->pre_trigger_buffer || at->pre_trigger_size <= 0)
		return;

	/* 若环形缓冲未满，回退头到首个有效样本 */
	if (at->pre_trigger_fill < at->pre_trigger_size)
		at->pre_trigger_head = at->pre_trigger_buffer;

	while (at->pre_trigger_fill > 0)
	{
		size_t size = MIN((size_t)(at->pre_trigger_buffer + at->pre_trigger_size - at->pre_trigger_head), (size_t)at->pre_trigger_fill);
		devc->send_data_proc(sdi, at->pre_trigger_head, size, at->unitsize);
		at->pre_trigger_head = at->pre_trigger_buffer;
		at->pre_trigger_fill -= size;
		if (pre_trigger_samples)
			*pre_trigger_samples += size / at->unitsize;
	}
}

/* 返回触发点在buf中的样本偏移(>=0)，未触发返回-1。
 * pre_trigger_samples 返回随预触发缓冲一并发出的样本数。 */
SR_PRIV int ch32h417_analog_trigger_check(struct sr_dev_inst *sdi,
										  uint8_t *buf, int len, int *pre_trigger_samples)
{
	struct dev_context *devc = sdi->priv;
	struct ch32h417_analog_trigger *at = devc->atrig;
	struct sr_trigger *trigger;
	struct sr_trigger_stage *stage;
	struct sr_trigger_match *match;
	GSList *l, *l_stage;
	int offset, i, unitsize;
	gboolean match_found;

	if (!at || !at->trigger)
		return -1;

	trigger = (struct sr_trigger *)at->trigger;
	unitsize = at->unitsize;
	offset = -1;

	if (pre_trigger_samples)
		*pre_trigger_samples = 0;

	for (i = 0; i < len; i += unitsize)
	{
		l_stage = g_slist_nth(trigger->stages, at->cur_stage);
		if (!l_stage || !l_stage->data)
			return SR_ERR_ARG;
		stage = l_stage->data;
		if (!stage->matches)
			return SR_ERR_ARG;

		float cur = adc_raw_to_voltage(devc, buf + i);

		match_found = TRUE;
		for (l = stage->matches; l; l = l->next)
		{
			match = l->data;
			if (!match->channel || !match->channel->enabled)
				/* 忽略禁用通道上的触发 */
				continue;
			if (!analog_check_match(at, cur, match))
			{
				match_found = FALSE;
				break;
			}
		}

		/* 更新上一样本电压，供边沿检测使用 */
		at->prev_value = cur;
		at->have_prev = TRUE;

		if (match_found)
		{
			if (l_stage->next)
			{
				/* 命中当前stage，推进到下一stage */
				at->cur_stage++;
			}
			else
			{
				/* 命中末级stage：累入本块前缀并发送预触发数据，触发 */
				analog_pre_trigger_append(at, buf, i);
				analog_pre_trigger_send(sdi, at,
										pre_trigger_samples);
				offset = i / unitsize;
				break;
			}
		}
		else if (at->cur_stage > 0)
		{
			/* 之前命中过更高stage，本次失败，回退到stage0从下一样本重试 */
			i -= at->cur_stage * unitsize;
			if (i < -1)
				i = -1;
			at->cur_stage = 0;
		}
	}

	if (offset == -1)
		analog_pre_trigger_append(at, buf, len);

	return offset;
}

/* 数据读取线程 */
static gpointer data_read_thread(gpointer arg)
{
	struct sr_dev_inst *sdi = arg;
	struct dev_context *devc = sdi->priv;
	unsigned long pipe_num;
	unsigned long transfer_count, total_data_len;
	unsigned long length;
	uint8_t *buffer;
	size_t unitsize;
	unsigned int num_samples;
	int trigger_offset, pre_trigger_samples;
	int no_data_count = 0;
	const int no_data_timeout_ms = 500; /* 500ms无数据超时 */
	unsigned long carry_len = 0;		/* 上一轮残留的部分采样字节 */
	unsigned long aligned_len;			/* 对齐到 unitsize 的整采样字节数 */
	unsigned long partial;				/* 本轮不足一个采样的尾部字节数 */
	unsigned long backlog_warn_threshold = ADC_BACKLOG_WARN_START;

	pipe_num = (devc->mode == MODE_LOGIC_ANALYZER) ? PIPE_LOGIC_DATA : PIPE_ADC_DATA;

	if (devc->mode == MODE_LOGIC_ANALYZER)
		unitsize = (devc->num_channels == 16) ? 2 : 1;
	else
		unitsize = (devc->adc_width == ADC_WIDTH_10BIT) ? 2 : 1;

	buffer = g_try_malloc(devc->buffer_size);
	if (!buffer)
	{
		sr_err("Failed to allocate read buffer");
		g_mutex_lock(&devc->mutex);
		devc->acq_aborted = TRUE;
		g_mutex_unlock(&devc->mutex);
		return NULL;
	}

	sr_dbg("Data read thread started, pipe=%lu, buffer_size=%zu",
		   pipe_num, devc->buffer_size);

	while (!devc->acq_aborted)
	{
		/* 查询缓冲数据 */
		if (!ch375_query_buf_upload_ex(devc->device_index, pipe_num,
									   &transfer_count, &total_data_len))
		{
			/* 查询失败，短暂等待后重试 */
			g_usleep(1000);
			continue;
		}

		/* 积压诊断：total_data_len 为缓冲上传中当前尚未读取的字节数。
		 * 持续攀升说明主机消费跟不上设备产出，最终会溢出丢采样。
		 * 从16MB开始按倍数提高告警阈值，避免每256KB刷一条日志。 */
		if (total_data_len > devc->stats_max_pending_bytes)
			devc->stats_max_pending_bytes = total_data_len;
		if (transfer_count > devc->stats_max_packet_count)
			devc->stats_max_packet_count = transfer_count;

		if (backlog_warn_threshold &&
			total_data_len >= backlog_warn_threshold)
		{
			sr_warn("Upload backlog reached %lu bytes, host may be "
					"dropping samples.",
					total_data_len);
			if (backlog_warn_threshold <= G_MAXULONG / 2)
				backlog_warn_threshold *= 2;
			else
				backlog_warn_threshold = 0;
		}

		if (total_data_len == 0)
		{
			/* 没有数据，检查是否设备已停止 */
			no_data_count++;
			if (no_data_count >= no_data_timeout_ms)
			{
				send_stop_command(sdi);
				/* 超时无数据，设备可能已停止采集 */
				sr_info("No data for %d ms, device may have stopped", no_data_timeout_ms);
				g_free(buffer);
				/* 设置标志表示线程正在退出，定时器回调会处理停止 */
				g_mutex_lock(&devc->mutex);
				devc->acq_aborted = TRUE;
				g_mutex_unlock(&devc->mutex);
				sr_dbg("data_read_thread EXIT (timeout)");
				return NULL;
			}
			g_usleep(1000);
			continue;
		}

		/* 有数据，重置计数器 */
		no_data_count = 0;

		/* 读取数据，预留空间承接上一轮残留的部分采样字节 (carry) */
		length = devc->buffer_size - carry_len;
		if (!ch375_read_endpoint(devc->device_index, pipe_num,
								 buffer + carry_len, &length))
		{
			sr_warn("Failed to read data from pipe %lu", pipe_num);
			g_usleep(1000);
			continue;
		}

		/* 合并 carry 前缀，恢复连续采样字节流 */
		length += carry_len;
		carry_len = 0;

		if (length == 0)
		{
			g_usleep(1000);
			continue;
		}

		devc->stats_bytes_read += length;
		sr_dbg("Read %lu bytes from pipe %lu", length, pipe_num);

		num_samples = length / unitsize;
		aligned_len = num_samples * unitsize;

		/* 不足一个采样的尾部字节留到下一轮拼接，避免丢字节导致帧错位。
		 * 16ch(unitsize=2) 下若一次读取返回奇数字节，直接截断会丢 1 字节，
		 * 此后所有采样的双字节帧随之错位--滚动模式 16ch 长时间采集后
		 * 波形错乱的可能成因之一。先保存 partial 并把 length 截断到整采样，
		 * 再进入触发/发送处理。 */
		partial = length - aligned_len;
		length = aligned_len;

		if (num_samples == 0)
		{
			/* 仅剩不足一个采样，挪到首部供下一轮拼接 */
			if (partial)
			{
				carry_len = partial;
				memmove(buffer, buffer + aligned_len, partial);
			}
			g_usleep(1000);
			continue;
		}

		/* 触发处理 */
		g_mutex_lock(&devc->mutex);
		if (devc->trigger_fired)
		{
			/* 触发后发送数据 */
			if (!devc->limit_samples || devc->sent_samples < devc->limit_samples)
			{
				if (devc->limit_samples &&
					devc->sent_samples + num_samples > devc->limit_samples)
					num_samples = devc->limit_samples - devc->sent_samples;
				devc->send_data_proc(sdi, buffer,
									 num_samples * unitsize, unitsize);
				devc->sent_samples += num_samples;
			}
		}
		else if (devc->atrig)
		{
			/* ADC软件电平触发检测 */
			devc->atrig->unitsize = unitsize;
			trigger_offset = ch32h417_analog_trigger_check(sdi,
														   buffer, length,
														   &pre_trigger_samples);

			if (trigger_offset > -1)
			{
				/* 发送预触发数据(本块前缀部分) */
				devc->send_data_proc(sdi, buffer,
									 trigger_offset * unitsize, unitsize);
				std_session_send_df_trigger(sdi);
				devc->trigger_fired = TRUE;
				devc->sent_samples = pre_trigger_samples;

				/* 发送触发后数据 */
				num_samples = (length / unitsize) - trigger_offset;
				if (devc->limit_samples &&
					devc->sent_samples + num_samples > devc->limit_samples)
					num_samples = devc->limit_samples - devc->sent_samples;

				if (num_samples > 0)
				{
					devc->send_data_proc(sdi,
										 buffer + trigger_offset * unitsize,
										 num_samples * unitsize, unitsize);
					devc->sent_samples += num_samples;
				}
			}
		}
		else if (devc->stl)
		{
			/* 检测触发 */
			devc->stl->unitsize = unitsize;
			trigger_offset = soft_trigger_logic_check(devc->stl,
													  buffer, length,
													  &pre_trigger_samples);

			if (trigger_offset > -1)
			{
				/* 发送预触发数据 */
				devc->send_data_proc(sdi, buffer,
									 trigger_offset * unitsize, unitsize);
				std_session_send_df_trigger(sdi);
				devc->trigger_fired = TRUE;
				devc->sent_samples = pre_trigger_samples;

				/* 发送触发后数据 */
				num_samples = (length / unitsize) - trigger_offset;
				if (devc->limit_samples &&
					devc->sent_samples + num_samples > devc->limit_samples)
					num_samples = devc->limit_samples - devc->sent_samples;

				if (num_samples > 0)
				{
					devc->send_data_proc(sdi,
										 buffer + trigger_offset * unitsize,
										 num_samples * unitsize, unitsize);
					devc->sent_samples += num_samples;
				}
			}
		}
		g_mutex_unlock(&devc->mutex);

		/* 把不足一个采样的尾部字节挪到缓冲区首部，供下一轮拼接 */
		if (partial)
		{
			carry_len = partial;
			memmove(buffer, buffer + aligned_len, partial);
		}

		/* 检查采样限制 */
		if (devc->limit_samples && devc->sent_samples >= devc->limit_samples)
		{
			devc->num_frames++;
			devc->sent_samples = 0;
			devc->trigger_fired = FALSE;

			if (devc->stl)
				devc->stl->cur_stage = 0;

			if (devc->atrig)
			{
				devc->atrig->cur_stage = 0;
				devc->atrig->have_prev = FALSE;
				devc->atrig->pre_trigger_fill = 0;
				devc->atrig->pre_trigger_head = devc->atrig->pre_trigger_buffer;
			}

			if (devc->limit_frames && devc->num_frames >= devc->limit_frames)
			{
				// g_thread_try_new("stop_cmd", stop_command_thread_func, (gpointer)sdi, NULL);
				send_stop_command(sdi);

				g_mutex_lock(&devc->mutex);
				devc->acq_aborted = TRUE;
				g_mutex_unlock(&devc->mutex);
				g_free(buffer);
				sr_dbg("data_read_thread EXIT (frame limit)");
				return NULL;
			}
		}
	}

	g_free(buffer);
	sr_dbg("Data read thread exiting");

	return NULL;
}

/* 结束采集 */
static void finish_acquisition(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	devc = sdi->priv;

	/* 发送停止命令和结束包。停止命令的响应读取会顺带检测端点1中
	 * 的溢出命令(0xBD)，见 ch32h417_receive_response。 */
	send_stop_command(sdi);

	/* 移除定时器回调 */
	sr_session_source_remove(sdi->session, -1);

	/* 停止读取线程 */
	g_mutex_lock(&devc->mutex);
	devc->acq_aborted = TRUE;
	g_mutex_unlock(&devc->mutex);

	/* 等待线程结束 */
	if (devc->read_thread)
	{
		g_thread_join(devc->read_thread);
		devc->read_thread = NULL;
	}

	{
		gint64 elapsed_us = g_get_monotonic_time() -
							devc->stats_start_time_us;
		double elapsed_s;
		double avg_mb_s;

		if (elapsed_us < 0)
			elapsed_us = 0;
		elapsed_s = elapsed_us / 1000000.0;
		avg_mb_s = elapsed_s > 0.0 ? (double)devc->stats_bytes_read / (1024.0 * 1024.0) /
										 elapsed_s
								   : 0.0;

		sr_info("Acquisition summary: read=%" G_GUINT64_FORMAT
				" bytes, max_pending=%lu bytes, max_packets=%lu, "
				"elapsed=%.3f s, average=%.2f MB/s",
				devc->stats_bytes_read, devc->stats_max_pending_bytes,
				devc->stats_max_packet_count, elapsed_s, avg_mb_s);
	}

	std_session_send_df_end(sdi);

	sr_dbg("devc->read_thread EXIT");

	if (devc->stl)
	{
		soft_trigger_logic_free(devc->stl);
		devc->stl = NULL;
	}

	if (devc->atrig)
	{
		ch32h417_analog_trigger_free(devc->atrig);
		devc->atrig = NULL;
	}

	if (devc->logic_buffer)
	{
		g_free(devc->logic_buffer);
		devc->logic_buffer = NULL;
	}

	if (devc->adc_float_buffer)
	{
		g_free(devc->adc_float_buffer);
		devc->adc_float_buffer = NULL;
	}
	sr_dbg("finish_acquisition function done");
}

/* 根据当前采样率与通道数计算逻辑模式的缓冲上传批量大小。
 * 目标: 每批约 LOGIC_BATCH_TARGET_MS 到达一次。字节速率越低批量越小，
 * 避免低采样率下4MB要数百ms才凑满一批，造成波形"大批量+长间隔"卡顿
 * (与ADC修复思路一致)。与 data_read_thread 的 unitsize 估算保持一致
 * (8ch=1B, 16ch=2B/采样)。 */
static size_t ch32h417_logic_buffer_size(const struct dev_context *devc)
{
	const uint64_t unitsize = (devc->num_channels == 16) ? 2 : 1;
	const uint64_t byte_rate = devc->cur_samplerate * unitsize;
	uint64_t size;

	if (byte_rate == 0)
		return BUFFER_UPLOAD_SIZE;

	size = byte_rate * LOGIC_BATCH_TARGET_MS / 1000;

	/* 向上对齐到64KB，并clamp到[下限, 上限] */
	size = ((size + 65535) / 65536) * 65536;
	if (size < LOGIC_BUFFER_MIN_SIZE)
		size = LOGIC_BUFFER_MIN_SIZE;
	if (size > BUFFER_UPLOAD_SIZE)
		size = BUFFER_UPLOAD_SIZE;

	return (size_t)size;
}

static size_t ch32h417_adc_buffer_size(const struct dev_context *devc)
{
	const uint64_t unitsize = (devc->adc_width == ADC_WIDTH_10BIT) ? 2 : 1;
	const uint64_t byte_rate = devc->cur_samplerate * unitsize;
	uint64_t size;

	if (byte_rate == 0)
		return ADC_TRANSFER_SIZE;

	if (byte_rate >= ADC_FULLRATE_BYTE_RATE)
		return ADC_TRANSFER_SIZE;

	size = byte_rate * ADC_BATCH_TARGET_MS / 1000;

	size = (size / 65536) * 65536;
	if (size < ADC_BUFFER_MIN_SIZE)
		size = ADC_BUFFER_MIN_SIZE;
	if (size > ADC_TRANSFER_SIZE)
		size = ADC_TRANSFER_SIZE;

	return (size_t)size;
}

/*
 * 开始采集
 */
SR_PRIV int ch32h417_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_trigger *trigger;
	unsigned long pipe_num;
	unsigned long upload_size;
	int ret;

	devc = sdi->priv;
	devc->sent_samples = 0;
	devc->acq_aborted = FALSE;
	devc->num_frames = 0;
	devc->overflow_occurred = FALSE;
	devc->overflow_status = 0;
	devc->stats_start_time_us = g_get_monotonic_time();
	devc->stats_bytes_read = 0;
	devc->stats_max_pending_bytes = 0;
	devc->stats_max_packet_count = 0;

	if (devc->mode == MODE_ADC)
	{
		devc->buffer_size = ch32h417_adc_buffer_size(devc);
		upload_size = devc->buffer_size;
	}
	else
	{
		devc->buffer_size = ch32h417_logic_buffer_size(devc);
		upload_size = devc->buffer_size;
	}
	sr_info("Mode %s: upload transfer size = %lu bytes, read size = %zu bytes",
			(devc->mode == MODE_ADC) ? "ADC" : "logic",
			upload_size, devc->buffer_size);

	/* 启动缓冲上传 */
	if (!ch375_set_buf_upload_ex(devc->device_index, 0, PIPE_LOGIC_DATA, upload_size))
	{
		sr_warn("Failed to set upload buffer pipe %d for device at index %d", PIPE_LOGIC_DATA, devc->device_index);
	}
	if (!ch375_set_buf_upload_ex(devc->device_index, 0, PIPE_ADC_DATA, upload_size))
	{
		sr_warn("Failed to set upload buffer pipe %d for device at index %d", PIPE_ADC_DATA, devc->device_index);
	}

	if (!ch375_set_buf_upload_ex(devc->device_index, 1, PIPE_LOGIC_DATA, upload_size))
	{
		sr_warn("Failed to set upload buffer pipe %d for device at index %d", PIPE_LOGIC_DATA, devc->device_index);
	}
	if (!ch375_set_buf_upload_ex(devc->device_index, 1, PIPE_ADC_DATA, upload_size))
	{
		sr_warn("Failed to set upload buffer pipe %d for device at index %d", PIPE_ADC_DATA, devc->device_index);
	}

	/* 配置通道 */
	if (devc->mode == MODE_LOGIC_ANALYZER)
	{
		pipe_num = PIPE_LOGIC_DATA;
		/* 先发送停止命令，确保设备处于空闲状态 */
		ret = ch32h417_stop_logic(sdi);
		if (ret != SR_OK)
		{
			sr_warn("Pre-stop logic command failed, continuing anyway...");
		}

		/* 设置逻辑分析仪参数 */
		ret = ch32h417_set_logic_params(sdi);
		if (ret != SR_OK)
		{
			sr_err("Failed to set logic params.");
			return ret;
		}

		/* 测试验证命令 */
		{
			uint8_t width, div, level;
			int test_failed = 0;

			/* 测试 A4: 获取逻辑分析仪参数 */
			ret = ch32h417_get_logic_params(sdi, &width, &div);
			if (ret == SR_OK)
			{
				sr_info("Verify A4: width=0x%02x, div=%d (expect width=0x%02x)",
						width, div, devc->sample_width);
				if (width != devc->sample_width)
				{
					sr_warn("A4 verification: width mismatch!");
					test_failed = 1;
				}
			}
			else
			{
				sr_err("A4 command failed!");
				test_failed = 1;
			}
		}

		/* 开始逻辑分析仪采集 */
		ret = ch32h417_start_logic(sdi);
		if (ret != SR_OK)
		{
			sr_err("Failed to start logic acquisition.");
			return ret;
		}
	}
	else
	{
		pipe_num = PIPE_ADC_DATA;

		sr_dbg("ADC mode: initializing enabled_analog_channels");
		sr_dbg("  sdi->channels = %p", sdi->channels);

		if (devc->enabled_analog_channels)
			g_slist_free(devc->enabled_analog_channels);
		devc->enabled_analog_channels = NULL;

		int analog_count = 0;
		for (GSList *l = sdi->channels; l; l = l->next)
		{
			struct sr_channel *ch = l->data;
			sr_dbg("  channel %p: type=%d, enabled=%d", ch, ch->type, ch->enabled);
			if (ch->type == SR_CHANNEL_ANALOG && ch->enabled)
			{
				devc->enabled_analog_channels = g_slist_append(
					devc->enabled_analog_channels, ch);
				analog_count++;
			}
		}
		sr_dbg("  enabled_analog_channels count = %d", analog_count);

		ret = ch32h417_stop_adc(sdi);
		if (ret != SR_OK)
		{
			sr_warn("Pre-stop ADC command failed, continuing anyway...");
		}

		ret = ch32h417_set_adc_params(sdi);
		if (ret != SR_OK)
		{
			sr_err("Failed to set ADC params.");
			return ret;
		}

		ret = ch32h417_set_adc_channel(sdi);
		if (ret != SR_OK)
		{
			sr_err("Failed to set ADC channel.");
			return ret;
		}

		/* 开始ADC采集 */
		ret = ch32h417_start_adc(sdi);
		if (ret != SR_OK)
		{
			sr_err("Failed to start ADC acquisition.");
			return ret;
		}
	}

	/* 设置触发 */
	if ((trigger = sr_session_trigger_get(sdi->session)))
	{
		int pre_trigger_samples = 0;
		if (devc->limit_samples > 0)
			pre_trigger_samples = (devc->capture_ratio * devc->limit_samples) / 100;

		if (devc->mode == MODE_ADC)
		{
			/* ADC软件电平触发：仅当trigger含已启用模拟通道的模拟match时才启用，
			 * 避免残留的逻辑通道触发导致空等/误触发。 */
			if (ch32h417_analog_trigger_has_match(devc, trigger))
			{
				devc->atrig = ch32h417_analog_trigger_new(sdi, trigger,
														  pre_trigger_samples);
				if (!devc->atrig)
					return SR_ERR_MALLOC;
				devc->trigger_fired = FALSE;
			}
			else
			{
				/* 无有效模拟触发match，按无触发处理 */
				devc->trigger_fired = TRUE;
			}
		}
		else
		{
			devc->stl = soft_trigger_logic_new(sdi, trigger, pre_trigger_samples);
			if (!devc->stl)
				return SR_ERR_MALLOC;
			devc->trigger_fired = FALSE;
		}
	}
	else
	{
		devc->trigger_fired = TRUE;
	}

	if (devc->mode == MODE_LOGIC_ANALYZER)
	{
		devc->send_data_proc = la_send_data_proc;
	}
	else
	{
		devc->send_data_proc = adc_send_data_proc;
	}

	g_mutex_lock(&devc->mutex);
	devc->acq_aborted = TRUE;
	g_mutex_unlock(&devc->mutex);

	if (devc->read_thread)
	{
		g_thread_join(devc->read_thread);
		devc->read_thread = NULL;
	}

	g_mutex_lock(&devc->mutex);
	devc->acq_aborted = FALSE;
	g_mutex_unlock(&devc->mutex);

	if (devc->mode == MODE_ADC)
	{
		const size_t sample_bytes =
			(devc->adc_width == ADC_WIDTH_10BIT) ? 2 : 1;
		const size_t capacity = devc->buffer_size / sample_bytes;

		if (devc->adc_float_buffer)
			g_free(devc->adc_float_buffer);
		devc->adc_float_buffer = g_try_malloc(
			sizeof(float) * capacity);
		if (!devc->adc_float_buffer)
		{
			sr_err("Failed to allocate ADC conversion buffer");
			send_stop_command(sdi);
			ch375_set_buf_upload_ex(devc->device_index, 0, PIPE_LOGIC_DATA, 0);
			ch375_set_buf_upload_ex(devc->device_index, 0, PIPE_ADC_DATA, 0);
			if (devc->atrig)
			{
				ch32h417_analog_trigger_free(devc->atrig);
				devc->atrig = NULL;
			}
			if (devc->stl)
			{
				soft_trigger_logic_free(devc->stl);
				devc->stl = NULL;
			}
			return SR_ERR_MALLOC;
		}
	}

	if (!ch375_clear_buf_upload(devc->device_index, pipe_num))
	{
		sr_warn("Failed to clear upload buffer pipe %lu for device at index %d", pipe_num, devc->device_index);
	}

	devc->read_thread = g_thread_new("ch32h417-read", data_read_thread, (void *)sdi);
	if (!devc->read_thread)
	{
		sr_err("Failed to create read thread");
		if (!ch375_clear_buf_upload(devc->device_index, pipe_num))
		{
			sr_warn("Failed to clear upload buffer pipe %lu for device at index %d", pipe_num, devc->device_index);
		}
		send_stop_command(sdi);
		if (devc->adc_float_buffer)
		{
			g_free(devc->adc_float_buffer);
			devc->adc_float_buffer = NULL;
		}
		return SR_ERR;
	}

	std_session_send_df_header(sdi);

	/* 添加定时器回调，用于检查采集状态并在主线程中停止采集 */
	sr_session_source_add(sdi->session, -1, 0, 5,
						  acquisition_check_cb, (void *)sdi);

	return SR_OK;
}

/*
 * 停止采集
 */
SR_PRIV int ch32h417_acquisition_stop(const struct sr_dev_inst *sdi)
{
	finish_acquisition((struct sr_dev_inst *)sdi);
	return SR_OK;
}
