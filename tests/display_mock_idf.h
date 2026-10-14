#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { \
    (void)(tag); if (!(condition)) return (error); \
} while (0)
#define ESP_GOTO_ON_ERROR(call, label, tag, ...) do { \
    (void)(tag); ret = (call); if (ret != ESP_OK) goto label; \
} while (0)
#define ESP_GOTO_ON_FALSE(condition, error, label, tag, ...) do { \
    (void)(tag); if (!(condition)) { ret = (error); goto label; } \
} while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1
typedef uint16_t lv_color_t;
typedef struct { int initialized; } lv_disp_t;
typedef struct { lv_color_t *first; lv_color_t *second; unsigned pixels; } lv_disp_draw_buf_t;
typedef struct { int x1; int y1; int x2; int y2; } lv_area_t;
typedef struct lv_disp_drv_t {
    int hor_res;
    int ver_res;
    lv_disp_draw_buf_t *draw_buf;
    void (*flush_cb)(struct lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
    void (*rounder_cb)(struct lv_disp_drv_t *, lv_area_t *);
} lv_disp_drv_t;
void lv_disp_flush_ready(lv_disp_drv_t *driver);
void lv_disp_draw_buf_init(lv_disp_draw_buf_t *, void *, void *, unsigned);
void lv_disp_drv_init(lv_disp_drv_t *);
lv_disp_t *lv_disp_drv_register(lv_disp_drv_t *);

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;
#define GPIO_NUM_38 38
#define GPIO_NUM_17 17
#define GPIO_NUM_6 6
#define GPIO_NUM_47 47
#define GPIO_NUM_18 18
#define GPIO_NUM_7 7
#define GPIO_NUM_48 48
#define GPIO_NUM_5 5
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, int);

typedef struct {
    int sclk_io_num;
    int data0_io_num;
    int data1_io_num;
    int data2_io_num;
    int data3_io_num;
    int data4_io_num;
    int data5_io_num;
    int data6_io_num;
    int data7_io_num;
    size_t max_transfer_sz;
    unsigned flags;
} spi_bus_config_t;
#define SPI3_HOST 3
#define SPI_DMA_CH_AUTO 0
#define SPICOMMON_BUSFLAG_MASTER 1
#define SPICOMMON_BUSFLAG_GPIO_PINS 2
esp_err_t spi_bus_initialize(int, const spi_bus_config_t *, int);
esp_err_t spi_bus_free(int);

typedef struct mock_panel_io *esp_lcd_panel_io_handle_t;
typedef intptr_t esp_lcd_spi_bus_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_event_data_t;
typedef struct {
    int cs_gpio_num;
    int dc_gpio_num;
    int spi_mode;
    unsigned pclk_hz;
    unsigned trans_queue_depth;
    bool (*on_color_trans_done)(esp_lcd_panel_io_handle_t,
                               esp_lcd_panel_io_event_data_t *, void *);
    void *user_ctx;
    int lcd_cmd_bits;
    int lcd_param_bits;
    struct { unsigned quad_mode : 1; } flags;
} esp_lcd_panel_io_spi_config_t;
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
                                  const esp_lcd_panel_io_spi_config_t *,
                                  esp_lcd_panel_io_handle_t *);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void *, size_t);
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t, int, const void *, size_t);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t);

#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
typedef uintptr_t TaskHandle_t;
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
void vTaskDelay(unsigned);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
