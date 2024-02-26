#include <lv_demos.h>

#include "esp_log.h"

#include "lgfx_user/LGFX_Sunton_ESP32-8048S050.h"

#define SCREEN_HOR_RES   800
#define SCREEN_VER_RES   480
#define LV_TICK_CUSTOM 1
#define LV_TICK_PERIOD_MS 10
#define BUFF_SIZE 80

static const char *TAG = "my-tag";

extern "C" void app_main();

static const uint16_t screenWidth  = 800;
static const uint16_t screenHeight = 480;

#define DRAW_BUF_SIZE (SCREEN_HOR_RES * SCREEN_VER_RES / 10 * (LV_COLOR_DEPTH / 8))
static uint32_t draw_buf1[DRAW_BUF_SIZE / 8];
static uint32_t draw_buf2[DRAW_BUF_SIZE / 8];

static LGFX lcd;
static lv_display_t *disp;
static lv_indev_t *indev;

/* Display flushing */
void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // Processing if not yet started
    if (lcd.getStartCount() == 0) {
        lcd.startWrite();
    }
    lcd.pushImageDMA(area->x1
                    , area->y1
                    , area->x2 - area->x1 + 1
                    , area->y2 - area->y1 + 1
                    , (lgfx::rgb565_t*)px_map);

    lv_disp_flush_ready(disp);
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

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started.");
    while (true) {
        lv_timer_handler();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

/* Setting up tick task for lvgl */
static void lv_tick_task(void *arg)
{
    (void)arg;
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

void app_main(void)
{
    lcd.init();
    lcd.initDMA();
    lcd.setRotation(0);
    lcd.setBrightness(255);
    lcd.setColorDepth(LV_COLOR_DEPTH);

    lv_init();
    disp = lv_display_create(SCREEN_HOR_RES, SCREEN_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_buffers(disp, draw_buf1, draw_buf2, sizeof(draw_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

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

    xTaskCreate(lvgl_task, "lvgl_task", configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL);
}