/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85C: ESP32-S3R8, 16 MB flash, round 360 px
 * ST77916 LCD on QSPI with a PWM backlight (GPIO5), CST816 touch, TCA9554 IO
 * expander holding the LCD and touch resets. Audio differs by revision, told
 * apart by whether the ES8311 answers on I2C:
 *  - V1: PCM5101 DAC on its own I2S bus and an I2S MEMS mic on another.
 *  - V2: ES8311 speaker (NS4150B amp on GPIO15) + ES7210 with two mics, on
 *    one duplex bus. GPIO2 and GPIO15 are the V1 mic's WS and SCK.
 * No PMU: the battery is read
 * through a 200K/100K divider on GPIO8 and the slide switch cuts it. BOOT
 * (GPIO0) is the only button besides RESET, so it talks; sleep and power off
 * are in the touch settings.
 *
 * Pins and the panel init sequence follow Waveshare's
 * waveshareteam/ESP32-S3-Touch-LCD-1.85C (ESP-IDF example and V2 schematic)
 * and xiaozhi-esp32's waveshare/esp32-s3-touch-lcd-1.85c board (both
 * revisions' audio pins).
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_CS GPIO_NUM_21
#define LCD_BL GPIO_NUM_5
#define DRAW_BUF_LINES 90                   /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define TP_INT GPIO_NUM_4

/* Speaker bus, the same on both revisions. */
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47
#define I2S_DIN GPIO_NUM_39     /* V2 ES7210 data, V1 mic data */
/* V2 */
#define I2S_MCLK GPIO_NUM_2
#define PA_EN GPIO_NUM_15
/* V1 mic, on its own bus */
#define MIC_WS GPIO_NUM_2
#define MIC_SCK GPIO_NUM_15
/* The V1 mic sends 24 bits in 32-bit slots. Its top 16 bits are quiet for
 * speech, so the settings' mic gain, made for the V2's PGA, applies less this. */
#define MIC_GAIN_OFFSET_DB 18

#define BOOT_GPIO GPIO_NUM_0
#define BATT_ADC ADC_CHANNEL_7              /* GPIO8, behind a 1/3 divider */

/* TCA9554 at 0x20: EXIO1 (P0) resets touch, EXIO2 (P1) the LCD. */
#define EXIO_ADDR 0x20
#define EXIO_OUT_REG 0x01
#define EXIO_CFG_REG 0x03
#define EXIO_TP_RST BIT(0)
#define EXIO_LCD_RST BIT(1)

/* ST77916 QSPI opcodes in the top byte of a 32-bit command. */
#define LCD_OPCODE_WRITE_CMD 0x02
#define LCD_OPCODE_READ_CMD 0x0B

/* For panels that read 00 02 7F 7F from register 0x04; the driver's default
 * sequence suits the other (00 7F 7F 7F). Waveshare's, unchanged. */
static const st77916_lcd_init_cmd_t s_init_new[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},
};

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exio;
static uint8_t s_exio_out = 0xFF;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static muse_board_t s_board;
static bool s_v2;                       /* ES8311 + ES7210 rather than PCM5101 + I2S mic */
static i2s_chan_handle_t s_mic_rx;      /* V1 */
static int s_mic_gain_q8 = 256;

static esp_err_t exio_set(uint8_t mask, bool high)
{
    s_exio_out = high ? s_exio_out | mask : s_exio_out & ~mask;
    return i2c_master_transmit(s_exio, (uint8_t[]){ EXIO_OUT_REG, s_exio_out }, 2, 100);
}

/* Pulses a reset line on the expander, as Waveshare's drivers do. */
static void exio_reset(uint8_t mask)
{
    exio_set(mask, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    exio_set(mask, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t exio_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXIO_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exio_cfg, &s_exio), TAG, "expander");
    /* Both resets released, then made outputs; the rest stay inputs. */
    ESP_RETURN_ON_ERROR(exio_set(EXIO_TP_RST | EXIO_LCD_RST, true), TAG, "expander out");
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_exio, (uint8_t[]){ EXIO_CFG_REG, (uint8_t)~(EXIO_TP_RST | EXIO_LCD_RST) }, 2, 100),
                        TAG, "expander dir");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    s_v2 = i2c_master_probe(s_i2c, ES8311_CODEC_DEFAULT_ADDR >> 1, 50) == ESP_OK;
    ESP_LOGI(TAG, "board V%d: %s", s_v2 ? 2 : 1, s_v2 ? "ES8311 + ES7210" : "PCM5101 + I2S mic");
    if (!s_v2) {
        s_board.mic_slot = 1;   /* the mic answers on the right slot */
    }

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration: battery level unavailable");
        s_adc_cali = NULL;
    }
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    return ledc_channel_config(&ch);
}

/* Two panel makers ship on this board; register 0x04 says which. */
static void pick_init(esp_lcd_panel_io_handle_t io, st77916_vendor_config_t *vendor)
{
    uint8_t id[4] = { 0 };
    esp_err_t err = esp_lcd_panel_io_rx_param(io, (LCD_OPCODE_READ_CMD << 24) | (0x04 << 8), id, sizeof(id));
    ESP_LOGI(TAG, "ST77916 id %02x %02x %02x %02x (%s)", id[0], id[1], id[2], id[3], esp_err_to_name(err));
    if (err == ESP_OK && id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        vendor->init_cmds = s_init_new;
        vendor->init_cmds_size = sizeof(s_init_new) / sizeof(s_init_new[0]);
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    if (backlight_init() != ESP_OK) {
        return NULL;
    }
    exio_reset(EXIO_LCD_RST);

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 3 * 1000 * 1000,         /* the ID read needs it slow */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    esp_lcd_panel_io_handle_t probe;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &probe) != ESP_OK) {
        return NULL;
    }
    st77916_vendor_config_t vendor = { .flags.use_qspi_interface = 1 };
    pick_init(probe, &vendor);
    esp_lcd_panel_io_del(probe);

    io_cfg.pclk_hz = 80 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* on the expander, pulsed above */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK ||
        esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }

    exio_reset(EXIO_TP_RST);
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = GPIO_NUM_NC,        /* on the expander */
        .int_gpio_num = TP_INT,
    };
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK ||
        esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable");
        s_tp = NULL;
    } else {
        const esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
        *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void send_sleep(void *sleep)
{
    uint8_t cmd = *(bool *)sleep ? 0x10 : 0x11;   /* SLPIN / SLPOUT */
    esp_lcd_panel_io_tx_param(s_io, (LCD_OPCODE_WRITE_CMD << 24) | (cmd << 8), NULL, 0);
}

/* The ST77916 driver has no sleep op, so plain SLPIN/SLPOUT go over the QSPI
 * command path. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops and the CST816 goes to deep sleep (0xA5 = 0x03),
 * from which only its reset line, on the expander, brings it back. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        if (s_tp) {
            esp_lcd_panel_io_tx_param(s_tp->io, 0xA5, (uint8_t[]){ 0x03 }, 1);
        }
    } else {
        if (s_tp) {
            exio_reset(EXIO_TP_RST);
        }
        esp_lv_adapter_resume();
    }
}

/* V2: ES8311 plays and ES7210 records over one duplex I2S bus, MCLK on GPIO2. */
static esp_err_t audio_init_v2(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static int mic_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    return (on ? i2s_channel_enable(s_mic_rx) : i2s_channel_disable(s_mic_rx)) == ESP_OK
               ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR;
}

/* Two 32-bit slots in, as two 16-bit ones out with the gain applied, in place. */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    static int32_t raw[256];
    int16_t *out = (int16_t *)data;
    for (int n = size / 2; n > 0;) {
        int k = n > (int)(sizeof(raw) / sizeof(raw[0])) ? (int)(sizeof(raw) / sizeof(raw[0])) : n;
        size_t got;
        if (i2s_channel_read(s_mic_rx, raw, k * sizeof(raw[0]), &got, pdMS_TO_TICKS(1000)) != ESP_OK ||
            got != k * sizeof(raw[0])) {
            return ESP_CODEC_DEV_READ_FAIL;
        }
        for (int i = 0; i < k; i++) {
            int v = (raw[i] >> 16) * s_mic_gain_q8 >> 8;
            *out++ = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
        }
        n -= k;
    }
    return ESP_CODEC_DEV_OK;
}

/* V1: the PCM5101 needs no setup and takes its clock from BCLK, so the
 * speaker is plain I2S with esp_codec_dev's software volume. The mic is on
 * a second bus. */
static esp_err_t audio_init_v1(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t spk_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    spk_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan, &tx, NULL), TAG, "speaker channel");
    i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    /* With SCK grounded the PCM5101's PLL locks to BCK, which 16-bit slots at
     * 16 kHz leave too slow (512 kHz). 32-bit slots double it, as Waveshare's
     * and xiaozhi's V1 code run it. */
    spk_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &spk_cfg), TAG, "speaker i2s");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "speaker on");

    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan, NULL, &s_mic_rx), TAG, "mic channel");
    const i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx, &mic_cfg), TAG, "mic i2s");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *spk_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(spk_if, ESP_ERR_NO_MEM, TAG, "speaker interface");
    static const audio_codec_data_if_t mic_if = { .enable = mic_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    return s_v2 ? audio_init_v2(spk, mic) : audio_init_v1(spk, mic);
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    if (!s_v2) {
        s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
        return;
    }
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/* No charger status reaches the ESP32, so USB is the computer answering, and
 * charging is guessed from it. */
static esp_err_t read_power(muse_power_t *out)
{
    int mv = 0;
    if (s_adc_cali) {
        int sum = 0;
        for (int i = 0; i < 8; i++) {
            int raw, v;
            ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
            ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &v), TAG, "adc cali");
            sum += v;
        }
        mv = sum / 8 * 3;
    }
    out->usb = usb_serial_jtag_is_connected();
    out->battery_mv = mv;
    if (mv < 2500) {
        out->battery_pct = -1;      /* switched off, or not fitted */
        out->charging = false;
        return ESP_OK;
    }
    /* A LiPo's curve, as the StickS3 uses. */
    int pct = (-mv * mv + 9016 * mv - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->charging = out->usb && out->battery_pct < 100;
    return ESP_OK;
}

/*
 * No power latch: the battery switch is mechanical. Screen and amp off, then
 * deep sleep until BOOT is pressed (or RESET).
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    panel_sleep(true);
    if (s_v2) {
        gpio_set_level(PA_EN, 0);
    }
    while (gpio_get_level(BOOT_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT_GPIO, 0), TAG, "wake button");
    rtc_gpio_pullup_en(BOOT_GPIO);
    rtc_gpio_pulldown_dis(BOOT_GPIO);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85C",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    /* BOOT sits on the edge beside the USB-C port, at the bottom. There's no
     * aux button, so aux_button and aux_hint are left out. */
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -40 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,             /* V2's two mics: mix them. V1's one is set in init() */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
