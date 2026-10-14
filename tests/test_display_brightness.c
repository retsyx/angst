#include "display_mock_idf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/board_display.c"

struct mock_panel_io {
    esp_lcd_panel_io_spi_config_t configuration;
    bool pending_pixels;
};

static lv_disp_t mock_display;
static TaskHandle_t current_task = 1;
static unsigned writes;
static unsigned last_command;
static size_t last_size;
static uint8_t last_data[4];
static unsigned completed_flushes;
static unsigned live_allocations;
static unsigned live_panel_ios;
static unsigned live_buses;
static bool fail_allocation;
static bool fail_write;

esp_err_t gpio_config(const gpio_config_t *configuration)
{
    assert(configuration);
    return ESP_OK;
}

esp_err_t gpio_set_level(int gpio, int level)
{
    (void)gpio;
    (void)level;
    return ESP_OK;
}

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *configuration, int dma)
{
    assert(host == SPI3_HOST && configuration && dma == SPI_DMA_CH_AUTO);
    ++live_buses;
    return ESP_OK;
}

esp_err_t spi_bus_free(int host)
{
    assert(host == SPI3_HOST && live_buses == 1);
    --live_buses;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                  const esp_lcd_panel_io_spi_config_t *configuration,
                                  esp_lcd_panel_io_handle_t *output)
{
    assert(bus == SPI3_HOST && configuration && output);
    struct mock_panel_io *io = calloc(1, sizeof(*io));
    assert(io);
    io->configuration = *configuration;
    *output = io;
    ++live_panel_ios;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command,
                                   const void *data, size_t size)
{
    assert(io && size <= sizeof(last_data));
    /* Model ESP-IDF's contract: pending DMA finishes before a polling command. */
    if (io->pending_pixels) {
        io->pending_pixels = false;
        io->configuration.on_color_trans_done(io, NULL, io->configuration.user_ctx);
    }
    ++writes;
    last_command = (unsigned)command;
    last_size = size;
    if (size) {
        assert(data);
        memcpy(last_data, data, size);
    }
    return fail_write ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int command,
                                   const void *pixels, size_t size)
{
    assert(io && command == 0x32002C00 && pixels && size);
    assert(!io->pending_pixels);
    io->pending_pixels = true;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    assert(io && !io->pending_pixels && live_panel_ios == 1);
    free(io);
    --live_panel_ios;
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, unsigned flags)
{
    assert(size && flags);
    if (fail_allocation) return NULL;
    void *allocation = malloc(size);
    assert(allocation);
    ++live_allocations;
    return allocation;
}

void heap_caps_free(void *allocation)
{
    if (allocation) {
        assert(live_allocations);
        --live_allocations;
        free(allocation);
    }
}

void vTaskDelay(unsigned ticks) { (void)ticks; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }

void lv_disp_draw_buf_init(lv_disp_draw_buf_t *buffer, void *first, void *second,
                           unsigned pixels)
{
    *buffer = (lv_disp_draw_buf_t){ first, second, pixels };
}

void lv_disp_drv_init(lv_disp_drv_t *driver) { memset(driver, 0, sizeof(*driver)); }

lv_disp_t *lv_disp_drv_register(lv_disp_drv_t *driver)
{
    assert(driver->flush_cb && driver->draw_buf);
    mock_display.initialized = 1;
    return &mock_display;
}

void lv_disp_flush_ready(lv_disp_drv_t *driver)
{
    assert(driver == &display_driver);
    ++completed_flushes;
}

static void expect_brightness(uint8_t brightness)
{
    const unsigned before = writes;
    assert(board_display_set_brightness(brightness) == ESP_OK);
    assert(writes == before + 1);
    assert(last_command == 0x02005100 && last_size == 1 && last_data[0] == brightness);
}

int main(void)
{
    assert(board_display_set_brightness(255) == ESP_ERR_INVALID_STATE);
    assert(writes == 0);
    assert(board_display_init(NULL) == ESP_ERR_INVALID_ARG);

    /* A failed initialization cannot expose a partially configured panel. */
    lv_disp_t *display = NULL;
    fail_allocation = true;
    assert(board_display_init(&display) == ESP_ERR_NO_MEM);
    assert(display == NULL);
    assert(!live_allocations && !live_panel_ios && !live_buses);
    unsigned before = writes;
    assert(board_display_set_brightness(128) == ESP_ERR_INVALID_STATE);
    assert(writes == before);

    fail_allocation = false;
    assert(board_display_init(&display) == ESP_OK && display == &mock_display);
    assert(last_command == 0x02005100 && last_size == 1 && last_data[0] == 255);
    const uint8_t values[] = { 0, 1, 127, 128, 254, 255 };
    for (size_t i = 0; i < sizeof(values); ++i) expect_brightness(values[i]);

    /* Matter callbacks on a different task must never write to this SPI device. */
    current_task = 2;
    before = writes;
    assert(board_display_set_brightness(128) == ESP_ERR_INVALID_STATE);
    assert(writes == before);
    current_task = 1;

    /* Brightness uses blocking parameter IO; pending LVGL DMA completes first. */
    lv_color_t pixels[4] = { 0 };
    const lv_area_t area = { 0, 0, 1, 1 };
    display_driver.flush_cb(&display_driver, &area, pixels);
    assert(panel_io->pending_pixels && completed_flushes == 0);
    expect_brightness(64);
    assert(!panel_io->pending_pixels && completed_flushes == 1);

    /* Errors reach the caller and the same value can be retried successfully. */
    fail_write = true;
    assert(board_display_set_brightness(32) == ESP_FAIL);
    fail_write = false;
    expect_brightness(32);

    assert(board_display_init(&display) == ESP_ERR_INVALID_STATE);
    esp_lcd_panel_io_del(panel_io);
    spi_bus_free(SPI3_HOST);
    heap_caps_free(draw_pixels[0]);
    heap_caps_free(draw_pixels[1]);
    assert(!live_allocations && !live_panel_ios && !live_buses);
    puts("Display brightness tests passed");
    return 0;
}
