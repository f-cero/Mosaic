/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic ESP32 port — ESP-IDF only. Build with:
 *   idf.py set-target esp32s3 && idf.py build
 * Display: esp_lcd (ILI9341/ST7789), audio: I2S or LEDC PWM, storage: SDMMC,
 * Wi-Fi/BLE: esp_wifi + NimBLE, link: spi_master with one CS per node. */
#include "ms_port_esp32.h"
#include "mosaic/ms_link.h"
#include "mosaic/ms_input.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static uint16_t s_fb[MS_SCREEN_W * MS_SCREEN_H];
static uint8_t  s_arena[8192];
static ms_core_t s_core;
static ms_game_t s_game;
static ms_link_t s_link;
static spi_device_handle_t s_link_spi;
static i2s_chan_handle_t s_i2s;
static esp_lcd_panel_handle_t s_panel;
static volatile uint16_t s_pad;

/* ---- audio: frame-driven, never allocates ---- */
void ms_port_audio_sink(void *user, const int16_t *stereo, int frames)
{
    (void)user;
    size_t written = 0;
    if (s_i2s) i2s_channel_write(s_i2s, stereo, (size_t)frames * 4, &written, 0);
}

/* ---- display: one DMA flush per frame; band mode for link nodes ---- */
static void push_frame(const uint16_t *fb)
{
    if (!s_panel) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, MS_SCREEN_W, MS_SCREEN_H, fb);
}

/* ---- input: two buttons + start/select on GPIO, optional ADC pad ---- */
static uint16_t read_pad(void)
{
    uint16_t b = s_pad;
    if (!gpio_get_level(GPIO_NUM_0))  b |= MS_BTN_A;
    if (!gpio_get_level(GPIO_NUM_14)) b |= MS_BTN_B;
    if (!gpio_get_level(GPIO_NUM_27)) b |= MS_BTN_START;
    return b;
}

/* ---- SPI link: 8-byte framed register deltas ---- */
int ms_port_link_start(const ms_device_t *dev)
{
    spi_bus_config_t bus = {
        .mosi_io_num = 11, .miso_io_num = 13, .sclk_io_num = 12,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = MS_LINK_MTU + 16
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    spi_device_interface_config_t d = { .clock_speed_hz = (int)dev->spi_hz, .mode = 0, .spics_io_num = 10, .queue_size = 4 };
    if (spi_bus_add_device(SPI2_HOST, &d, &s_link_spi) != ESP_OK) return -2;
    ms_link_init(&s_link);
    return 0;
}

static void link_sync(const ms_ppu_t *ppu)
{
    if (!s_link_spi) return;
    static uint8_t pkt[MS_LINK_MTU];
    ms_frame_t f = { .ver = 1, .type = MS_LINK_REG_DELTA, .node = 0, .seq = (uint16_t)s_link.txseq++ };
    f.len = (uint16_t)ms_link_encode_ppu_delta(ppu, f.payload, sizeof(f.payload));
    size_t n = ms_link_encode(&f, pkt, sizeof(pkt));
    spi_transaction_t t = { .length = n * 8, .tx_buffer = pkt };
    spi_device_polling_transmit(s_link_spi, &t);
    s_link.st.bytes_tx += (uint32_t)n; s_link.st.frames_tx++;
}

/* ---- network: non-blocking state machine, offline-first ---- */
int ms_port_net_poll(void)
{
    static int inited;
    if (!inited) {
        esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_sta();
        wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&c); esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start();
        inited = 1;
    }
    return 0;
}

int ms_port_init(ms_core_t *core, const ms_device_t *dev)
{
    ms_config_t cfg = { .mode = MS_MODE_RICH, .qfa = 0, .voices = MS_VOICES,
                        .audio_hz = MS_AUDIO_HZ, .music_voices = 12, .sfx_voices = 4 };
    if (ms_core_init(core, &cfg, s_fb, s_arena, sizeof(s_arena)) != 0) return -1;
    ms_core_set_audio_sink(core, ms_port_audio_sink, NULL);
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_INPUT);
    if (dev && ms_device_role_count(dev, MS_ROLE_PPU)) ms_port_link_start(dev);
    return 0;
}

void ms_port_frame(ms_core_t *core, ms_game_t *game)
{
    ms_input_t in; ms_input_clear(&in);
    ms_input_edge(&in, read_pad());
    ms_core_run_frame(core, game, &in);
    push_frame(core->fb);
    link_sync(&core->ppu);
}
