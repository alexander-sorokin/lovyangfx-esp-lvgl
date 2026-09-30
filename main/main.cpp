#include <lv_demos.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_DISPLAY_BACKEND_LOVYANGFX
 #include "lgfx_user/LGFX_Sunton_ESP32-8048S050.h"
#else
 #include "esp_lcd_panel_ops.h"
 #include "esp_lcd_panel_rgb.h"
 #include "driver/gpio.h"

 #define LGFX_USE_V1
 #include <LovyanGFX.hpp>
 #include <lgfx/v1/touch/Touch_GT911.hpp>

 #if __has_include(<driver/i2c_master.h>)
  #include <driver/i2c_master.h>
 #else
  #include <driver/i2c.h>
 #endif
#endif

#define SCREEN_HOR_RES   800
#define SCREEN_VER_RES   480
#define LV_TICK_CUSTOM 1
#define LV_TICK_PERIOD_MS 10

static const char *TAG = "my-tag";

extern "C" void app_main();

static lv_display_t *disp;
static lv_indev_t *indev;
static TaskHandle_t lvgl_task_handle = nullptr;

#if CONFIG_DISPLAY_BACKEND_LOVYANGFX

// LovyanGFX's ESP32-S3 Bus_RGB/Panel_RGB drives the LCD_CAM peripheral with a
// single, continuously-scanned PSRAM frame buffer and no vsync-gated buffer
// swap - the RGB peripheral can't apply backpressure, so heavy redraws (e.g.
// scrolling) can tear. Kept as a simpler reference/fallback; see the
// DISPLAY_BACKEND_ESP_LCD path below (default) for the tear-free,
// double-buffered implementation.

// LV_COLOR_DEPTH is a *bit* depth (16 for RGB565) - divide by 8 for bytes/pixel.
// Buffers live in internal SRAM, not PSRAM, so they don't contend with the
// continuous RGB scanout DMA for PSRAM bandwidth.
#define DRAW_BUF_LINES (SCREEN_VER_RES / 10)
#define DRAW_BUF_SIZE (SCREEN_HOR_RES * DRAW_BUF_LINES * (LV_COLOR_DEPTH / 8))

static LGFX lcd;

/* Display flushing */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (lcd.getStartCount() == 0) {
        lcd.startWrite();
    }
    lcd.pushImageDMA(area->x1, area->y1,
                      area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                      (lgfx::rgb565_t*)px_map);
    lv_display_flush_ready(disp);
}

/*Read the touchpad*/
void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t touchX, touchY;

    data->state = LV_INDEV_STATE_REL;

    if (lcd.getTouch(&touchX, &touchY)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

#else // CONFIG_DISPLAY_BACKEND_ESP_LCD

#define PIN_BACKLIGHT GPIO_NUM_2

// LV_COLOR_DEPTH is a *bit* depth (16 for RGB565) - divide by 8 for bytes/pixel.
#define FRAME_BUF_SIZE (SCREEN_HOR_RES * SCREEN_VER_RES * (LV_COLOR_DEPTH / 8))

static esp_lcd_panel_handle_t panel_handle = nullptr;
static lgfx::Touch_GT911 touch;

// Runs in ISR context once the RGB panel has actually switched its DMA over to
// the frame buffer submitted by the last draw_bitmap() call - only then is the
// *other* buffer (the one LVGL just finished with) safe to render into again.
static bool on_frame_buf_complete(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t need_yield = pdFALSE;
    if (lvgl_task_handle) {
        vTaskNotifyGiveFromISR(lvgl_task_handle, &need_yield);
    }
    return need_yield == pdTRUE;
}

/* Display flushing */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    if (!lv_display_flush_is_last(disp)) {
        // LVGL (direct render mode) drew straight into the back frame buffer;
        // nothing to submit to the panel driver until the last dirty area.
        lv_display_flush_ready(disp);
        return;
    }
    // Clear any stale completion from an earlier frame before waiting for this one.
    ulTaskNotifyTake(pdTRUE, 0);
    // px_map points inside one of the panel driver's own frame buffers, so this
    // is a zero-copy handover: the GDMA link switches to it at the next frame
    // boundary instead of the CPU racing the scanout mid-frame.
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SCREEN_HOR_RES, SCREEN_VER_RES, px_map);
}

static void disp_flush_wait(lv_display_t *disp)
{
    if (lv_display_flush_is_last(disp)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    lv_display_flush_ready(disp);
}

/*Read the touchpad*/
void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    lgfx::touch_point_t tp;

    data->state = LV_INDEV_STATE_REL;

    if (touch.getTouchRaw(&tp, 1)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = tp.x;
        data->point.y = tp.y;
    }
}

static void init_touch(void)
{
    auto cfg = touch.config();
    cfg.x_min = 0;
    cfg.x_max = SCREEN_HOR_RES;
    cfg.y_min = 0;
    cfg.y_max = SCREEN_VER_RES;
    cfg.pin_int = GPIO_NUM_NC;
    cfg.bus_shared = false;
    cfg.offset_rotation = 0;
    cfg.i2c_port = I2C_NUM_1;
    cfg.pin_sda = GPIO_NUM_19;
    cfg.pin_scl = GPIO_NUM_20;
    cfg.pin_rst = GPIO_NUM_38;
    cfg.freq = 400000;
    cfg.i2c_addr = 0x14;
    touch.config(cfg);
    if (!touch.init()) {
        ESP_LOGE(TAG, "Touch init failed.");
    }
}

// Drives the RGB panel directly via ESP-IDF's esp_lcd_rgb_panel (num_fbs=2)
// instead of LovyanGFX's Panel_RGB/Bus_RGB, which only supports a single,
// continuously-scanned frame buffer with no vsync-gated swap. Timings/pins
// below are carried over from lgfx_user/LGFX_Sunton_ESP32-8048S050.h.
static void init_panel(void)
{
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_config.data_width = 16;
    panel_config.num_fbs = 2;
    panel_config.dma_burst_size = 64;
    // Bounce buffer: GDMA refills this small internal-SRAM staging buffer ahead
    // of time instead of the LCD peripheral reading PSRAM directly on its fixed
    // real-time schedule, so a transient PSRAM latency spike can't starve it.
    panel_config.bounce_buffer_size_px = 20 * SCREEN_HOR_RES;
    panel_config.hsync_gpio_num = GPIO_NUM_39;
    panel_config.vsync_gpio_num = GPIO_NUM_41;
    panel_config.de_gpio_num = GPIO_NUM_40;
    panel_config.pclk_gpio_num = GPIO_NUM_42;
    panel_config.disp_gpio_num = GPIO_NUM_NC;
    panel_config.data_gpio_nums[0]  = GPIO_NUM_8;  // B0
    panel_config.data_gpio_nums[1]  = GPIO_NUM_3;  // B1
    panel_config.data_gpio_nums[2]  = GPIO_NUM_46; // B2
    panel_config.data_gpio_nums[3]  = GPIO_NUM_9;  // B3
    panel_config.data_gpio_nums[4]  = GPIO_NUM_1;  // B4
    panel_config.data_gpio_nums[5]  = GPIO_NUM_5;  // G0
    panel_config.data_gpio_nums[6]  = GPIO_NUM_6;  // G1
    panel_config.data_gpio_nums[7]  = GPIO_NUM_7;  // G2
    panel_config.data_gpio_nums[8]  = GPIO_NUM_15; // G3
    panel_config.data_gpio_nums[9]  = GPIO_NUM_16; // G4
    panel_config.data_gpio_nums[10] = GPIO_NUM_4;  // G5
    panel_config.data_gpio_nums[11] = GPIO_NUM_45; // R0
    panel_config.data_gpio_nums[12] = GPIO_NUM_48; // R1
    panel_config.data_gpio_nums[13] = GPIO_NUM_47; // R2
    panel_config.data_gpio_nums[14] = GPIO_NUM_21; // R3
    panel_config.data_gpio_nums[15] = GPIO_NUM_14; // R4
    panel_config.timings.pclk_hz = 16000000;
    panel_config.timings.h_res = SCREEN_HOR_RES;
    panel_config.timings.v_res = SCREEN_VER_RES;
    panel_config.timings.hsync_pulse_width = 4;
    panel_config.timings.hsync_back_porch = 8;
    panel_config.timings.hsync_front_porch = 8;
    panel_config.timings.vsync_pulse_width = 4;
    panel_config.timings.vsync_back_porch = 8;
    panel_config.timings.vsync_front_porch = 8;
    // Derived from LGFX_Sunton_ESP32-8048S050.h's Bus_RGB register writes
    // (hsync/vsync_polarity=0, pclk_idle_high=1), translated to esp_lcd's flags.
    panel_config.timings.flags.hsync_idle_low = true;
    panel_config.timings.flags.vsync_idle_low = true;
    panel_config.timings.flags.pclk_active_neg = true;
    panel_config.flags.fb_in_psram = true;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_frame_buf_complete = on_frame_buf_complete;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, nullptr));
}

#endif // CONFIG_DISPLAY_BACKEND_ESP_LCD

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started.");
    while (true) {
        lv_timer_handler();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

/* Setting up tick task for lvgl */
static void lv_tick_task(void *arg)
{
    (void)arg;
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

void app_main(void)
{
#if CONFIG_DISPLAY_BACKEND_LOVYANGFX
    if (!lcd.init()) {
        // Typically the RGB framebuffer allocation failed: check CONFIG_SPIRAM (see sdkconfig.defaults).
        ESP_LOGE(TAG, "LCD init failed (free PSRAM: %u bytes).",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        abort();
    }
    lcd.initDMA();
    lcd.setRotation(0);
    lcd.setBrightness(255);
    lcd.setColorDepth(LV_COLOR_DEPTH);

    uint8_t *draw_buf1 = static_cast<uint8_t *>(heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    assert(draw_buf1);
    uint8_t *draw_buf2 = static_cast<uint8_t *>(heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    assert(draw_buf2);

    lv_init();
    disp = lv_display_create(SCREEN_HOR_RES, SCREEN_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_buffers(disp, draw_buf1, draw_buf2, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    gpio_config_t bl_cfg = {};
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    bl_cfg.pin_bit_mask = 1ULL << PIN_BACKLIGHT;
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_BACKLIGHT, 0);

    init_panel();
    init_touch();

    gpio_set_level(PIN_BACKLIGHT, 1);

    lv_init();
    disp = lv_display_create(SCREEN_HOR_RES, SCREEN_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_flush_wait_cb(disp, disp_flush_wait);

    // Give LVGL the panel driver's own two frame buffers directly (DIRECT mode)
    // so it draws straight into the back buffer instead of a separate render
    // buffer that would then need copying - see disp_flush().
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1));
    lv_display_set_buffers(disp, fb0, fb1, FRAME_BUF_SIZE, LV_DISPLAY_RENDER_MODE_DIRECT);
#endif

    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read);

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t lv_periodic_timer_args = {
        .callback = &lv_tick_task,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "periodic_gui",
        .skip_unhandled_events  = true
        };
    esp_timer_handle_t lv_periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&lv_periodic_timer_args, &lv_periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lv_periodic_timer, LV_TICK_PERIOD_MS * 1000));

    lv_demo_widgets();

    xTaskCreate(lvgl_task, "lvgl_task", configMINIMAL_STACK_SIZE * 8, NULL, 5, &lvgl_task_handle);
}
