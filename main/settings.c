#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "coco"

static const char *TAG = "settings";

settings_t settings;

static void get(nvs_handle_t h, const char *key, uint8_t *dst, uint8_t max)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK && v <= max) {
        *dst = v;
    }
}

void settings_load(void)
{
    settings.model = COCO_MODEL_3;
    settings.cpu = 0;
    settings.ram = 1;
    settings.monitor = COCO_MONITOR_RGB;
    settings.artifacts = 0;
    settings.joystick = 0;
    settings.tape_speed = 4;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    get(h, "model", &settings.model, COCO_MODEL_COUNT - 1);
    get(h, "cpu", &settings.cpu, 1);
    get(h, "ram", &settings.ram, 1);
    get(h, "monitor", &settings.monitor, COCO_MONITOR_COMPOSITE);
    get(h, "artifacts", &settings.artifacts, 2);
    get(h, "joystick", &settings.joystick, 1);
    get(h, "tape_speed", &settings.tape_speed, 8);
    if (settings.tape_speed == 0) {
        settings.tape_speed = 1;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u8(h, "cpu", settings.cpu);
    nvs_set_u8(h, "ram", settings.ram);
    nvs_set_u8(h, "monitor", settings.monitor);
    nvs_set_u8(h, "artifacts", settings.artifacts);
    nvs_set_u8(h, "joystick", settings.joystick);
    nvs_set_u8(h, "tape_speed", settings.tape_speed);
    nvs_commit(h);
    nvs_close(h);
}
