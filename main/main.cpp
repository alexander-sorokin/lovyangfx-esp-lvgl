#include <lv_demos.h>

#include "esp_log.h"

#include "lgfx_user/LGFX_Sunton_ESP32-8048S050.h"

static const char *TAG = "my-tag";

extern "C" void app_main();

static const uint16_t screenWidth  = 800;
static const uint16_t screenHeight = 480;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[2][ screenWidth * 10 ];

static LGFX lcd;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    // Processing if not yet started
    ESP_LOGI(TAG, "Flushing display.");
    if (lcd.getStartCount() == 0) {
        lcd.startWrite();
    }
    lcd.pushImageDMA(area->x1
                    , area->y1
                    , area->x2 - area->x1 + 1
                    , area->y2 - area->y1 + 1
                    , (lgfx::swap565_t*)&color_p->full);
    lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data)
{
    uint16_t touchX, touchY;

    data->state = LV_INDEV_STATE_REL;

    if (lcd.getTouch(&touchX, &touchY)) {
        data->state = LV_INDEV_STATE_PR;

        /*Set the coordinates*/
        data->point.x = touchX;
        data->point.y = touchY;
    }

    ESP_LOGI(TAG, "Touch at X %d; Y %d", touchX, touchY);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started.");
    while (true) {
        static uint32_t suggested_time = lv_timer_handler();
        ESP_LOGI(TAG, "LVGL suggested redraw in %lu milliseconds.", suggested_time);
        vTaskDelay(suggested_time / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    lcd.init();
    lcd.setRotation(0);
    lcd.setBrightness(128);
    lcd.setColorDepth(16);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf[0], buf[1], screenWidth * 10);

    /*Initialize the display*/
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the input device driver*/
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    lv_demo_benchmark();

    xTaskCreate(lvgl_task, "lvgl_task", configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL);
}