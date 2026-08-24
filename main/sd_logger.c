#include <stdio.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "sd_logger.h"
#include "config.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_logger";
static sdmmc_card_t *s_card = NULL;
static bool s_available = false;

esp_err_t sd_logger_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed (%s); SD logging disabled", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s); logging to SD disabled", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    s_available = true;
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);

    struct stat st;
    if (stat(SD_LOG_FILENAME, &st) != 0) {
        FILE *f = fopen(SD_LOG_FILENAME, "w");
        if (f) {
            fprintf(f, "uptime_s,unix_time,meat_c,grill_c\n");
            fclose(f);
        }
    }
    return ESP_OK;
}

bool sd_logger_is_available(void)
{
    return s_available;
}

esp_err_t sd_logger_log(uint32_t uptime_s, int64_t unix_time, float meat_c, float grill_c)
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(SD_LOG_FILENAME, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for append", SD_LOG_FILENAME);
        return ESP_FAIL;
    }
    fprintf(f, "%" PRIu32 ",%" PRId64 ",%.2f,%.2f\n", uptime_s, unix_time, meat_c, grill_c);
    fclose(f);
    return ESP_OK;
}
