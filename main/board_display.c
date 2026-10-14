#include "board_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 1
#error "The AMOLED driver requires LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=1"
#endif

#define DISPLAY_SPI_HOST SPI3_HOST
#define DISPLAY_POWER_GPIO GPIO_NUM_38
#define DISPLAY_RESET_GPIO GPIO_NUM_17
#define DISPLAY_CS_GPIO GPIO_NUM_6
#define DISPLAY_CLK_GPIO GPIO_NUM_47
#define DISPLAY_D0_GPIO GPIO_NUM_18
#define DISPLAY_D1_GPIO GPIO_NUM_7
#define DISPLAY_D2_GPIO GPIO_NUM_48
#define DISPLAY_D3_GPIO GPIO_NUM_5
#define DISPLAY_CLOCK_HZ 75000000
/* Leave internal SRAM for the BLE controller while retaining two DMA buffers. */
#define DISPLAY_DRAW_ROWS 16
#define DISPLAY_DRAW_PIXELS (BOARD_DISPLAY_WIDTH * DISPLAY_DRAW_ROWS)
#define DISPLAY_DRAW_BYTES (DISPLAY_DRAW_PIXELS * sizeof(lv_color_t))

#define RM67162_CASET 0x2A
#define RM67162_RASET 0x2B
#define RM67162_RAMWR 0x2C
#define RM67162_BRIGHTNESS 0x51
#define RM67162_WRITE_COMMAND 0x02000000
#define RM67162_WRITE_PIXELS 0x32000000

static const char *TAG = "board_display";
static esp_lcd_panel_io_handle_t panel_io;
static lv_disp_drv_t display_driver;
static lv_disp_draw_buf_t draw_buffer;
static lv_color_t *draw_pixels[2];
static lv_disp_t *registered_display;
static TaskHandle_t display_task;

typedef struct {
    uint8_t command;
    uint8_t data;
    uint8_t data_size;
    uint16_t delay_ms;
} init_command_t;

/*
 * RM67162 QSPI initialization and power/reset timing follow LilyGo-AMOLED-Series
 * (MIT, Copyright (c) 2023 Shenzhen Xin Yuan Electronic Technology Co., Ltd.).
 * MADCTL 0xA0 is the sketch's setRotation(2): MV | MY, RGB, 536 x 240.
 */
static const init_command_t init_commands[] = {
    {0xFE, 0x00, 1, 0},
    {0x11, 0x00, 0, 120},
    {0xFE, 0x05, 1, 0},
    {0x05, 0x05, 1, 0},
    {0xFE, 0x01, 1, 0},
    {0x73, 0x25, 1, 0},
    {0xFE, 0x00, 1, 0},
    {0x36, 0xA0, 1, 0},
    {0x3A, 0x55, 1, 0},
    {RM67162_BRIGHTNESS, 0x00, 1, 0},
    {0x29, 0x00, 0, 120},
    {RM67162_BRIGHTNESS, 0xFF, 1, 0},
};

static esp_err_t write_command(uint8_t command, const void *data, size_t size)
{
    /* QSPI header on the wire is [0x02, 0x00, command, 0x00]. */
    const int encoded_command = RM67162_WRITE_COMMAND | ((int)command << 8);
    return esp_lcd_panel_io_tx_param(panel_io, encoded_command, data, size);
}

static bool color_transfer_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event,
                                void *user_context)
{
    (void)io;
    (void)event;
    /* This LVGL API may run in the SPI ISR. Buffers remain owned by DMA until now. */
    lv_disp_flush_ready((lv_disp_drv_t *)user_context);
    return false;
}

static void round_area(lv_disp_drv_t *driver, lv_area_t *area)
{
    (void)driver;
    /* Even start coordinates and even widths/heights keep RGB565 DMA aligned. */
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void flush_display(lv_disp_drv_t *driver, const lv_area_t *area,
                          lv_color_t *pixels)
{
    (void)driver;
    const uint8_t column_address[] = {
        (uint8_t)(area->x1 >> 8), (uint8_t)area->x1,
        (uint8_t)(area->x2 >> 8), (uint8_t)area->x2,
    };
    const uint8_t row_address[] = {
        (uint8_t)(area->y1 >> 8), (uint8_t)area->y1,
        (uint8_t)(area->y2 >> 8), (uint8_t)area->y2,
    };
    const size_t pixel_count = (size_t)(area->x2 - area->x1 + 1) *
                               (size_t)(area->y2 - area->y1 + 1);

    /* Parameter transfers block, so these stack arrays are safe to reuse. */
    ESP_ERROR_CHECK(write_command(RM67162_CASET, column_address, sizeof(column_address)));
    ESP_ERROR_CHECK(write_command(RM67162_RASET, row_address, sizeof(row_address)));
    ESP_ERROR_CHECK(write_command(RM67162_RAMWR, NULL, 0));

    /*
     * Pixel data is queued asynchronously with header [0x32, 0x00, 0x2C, 0x00].
     * LV_COLOR_16_SWAP already places the RGB565 high byte first. Never signal
     * flush_ready here: the completion callback releases the LVGL draw buffer.
     */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(panel_io,
                    RM67162_WRITE_PIXELS | (RM67162_RAMWR << 8), pixels,
                    pixel_count * sizeof(*pixels)));
}

esp_err_t board_display_init(lv_disp_t **display)
{
    ESP_RETURN_ON_FALSE(display, ESP_ERR_INVALID_ARG, TAG, "missing display output");
    *display = NULL;
    ESP_RETURN_ON_FALSE(!panel_io && !registered_display, ESP_ERR_INVALID_STATE,
                        TAG, "display already initialized");

    esp_err_t ret = ESP_OK;
    bool bus_initialized = false;
    const gpio_config_t power_reset_config = {
        .pin_bit_mask = (1ULL << DISPLAY_POWER_GPIO) | (1ULL << DISPLAY_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&power_reset_config), fail, TAG, "power/reset GPIO setup failed");
    ESP_GOTO_ON_ERROR(gpio_set_level(DISPLAY_POWER_GPIO, 1), fail, TAG, "display power failed");
    ESP_GOTO_ON_ERROR(gpio_set_level(DISPLAY_RESET_GPIO, 1), fail, TAG, "display reset failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_GOTO_ON_ERROR(gpio_set_level(DISPLAY_RESET_GPIO, 0), fail, TAG, "display reset failed");
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_GOTO_ON_ERROR(gpio_set_level(DISPLAY_RESET_GPIO, 1), fail, TAG, "display reset failed");
    vTaskDelay(pdMS_TO_TICKS(200));

    const spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY_CLK_GPIO,
        .data0_io_num = DISPLAY_D0_GPIO,
        .data1_io_num = DISPLAY_D1_GPIO,
        .data2_io_num = DISPLAY_D2_GPIO,
        .data3_io_num = DISPLAY_D3_GPIO,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = DISPLAY_DRAW_BYTES + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
                      fail, TAG, "QSPI bus initialization failed");
    bus_initialized = true;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = color_transfer_done,
        .user_ctx = &display_driver,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST,
                                             &io_config, &panel_io),
                      fail, TAG, "QSPI panel IO initialization failed");

    /* LilyGo runs its initialization twice to prevent intermittent startup failures. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < sizeof(init_commands) / sizeof(init_commands[0]); ++i) {
            const init_command_t *command = &init_commands[i];
            ESP_GOTO_ON_ERROR(write_command(command->command, &command->data, command->data_size),
                              fail, TAG, "RM67162 initialization failed");
            if (command->delay_ms) {
                vTaskDelay(pdMS_TO_TICKS(command->delay_ms));
            }
        }
    }

    for (unsigned i = 0; i < 2; ++i) {
        draw_pixels[i] = heap_caps_malloc(DISPLAY_DRAW_BYTES,
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_GOTO_ON_FALSE(draw_pixels[i], ESP_ERR_NO_MEM, fail, TAG, "DMA draw buffer allocation failed");
    }
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels[0], draw_pixels[1], DISPLAY_DRAW_PIXELS);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = BOARD_DISPLAY_WIDTH;
    display_driver.ver_res = BOARD_DISPLAY_HEIGHT;
    display_driver.draw_buf = &draw_buffer;
    display_driver.flush_cb = flush_display;
    display_driver.rounder_cb = round_area;
    registered_display = lv_disp_drv_register(&display_driver);
    ESP_GOTO_ON_FALSE(registered_display, ESP_ERR_NO_MEM, fail, TAG, "LVGL display registration failed");

    *display = registered_display;
    display_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "RM67162 AMOLED ready: %d x %d, landscape rotation 2",
              BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
    return ESP_OK;

fail:
    if (panel_io) {
        esp_lcd_panel_io_del(panel_io);
        panel_io = NULL;
    }
    if (bus_initialized) {
        spi_bus_free(DISPLAY_SPI_HOST);
    }
    for (unsigned i = 0; i < 2; ++i) {
        heap_caps_free(draw_pixels[i]);
        draw_pixels[i] = NULL;
    }
    gpio_set_level(DISPLAY_POWER_GPIO, 0);
    return ret;
}

esp_err_t board_display_set_brightness(uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(panel_io && registered_display &&
                        display_task == xTaskGetCurrentTaskHandle(),
                        ESP_ERR_INVALID_STATE, TAG, "brightness requires the LVGL task");

    /*
     * ESP-IDF tx_param drains pending tx_color DMA transfers before sending
     * this polling command. Keeping all writes on the LVGL task also prevents
     * the brightness command from interrupting a flush's address/RAMWR sequence.
     */
    return write_command(RM67162_BRIGHTNESS, &brightness, sizeof(brightness));
}
