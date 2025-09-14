/*
 *   This file is part of DroneBridge: https://github.com/DroneBridge/ESP32
 *
 *   USB CDC-ACM Host transport for ESP32-S2/S3 (USB-OTG)
 *
 *   Notes:
 *   - Requires IDF USB Host stack and CDC-ACM Host class driver (IDF 5.1+).
 *   - The ESP32 acts as USB Host; FCU must be USB Device (CDC-ACM).
 *   - Provide VBUS 5V externally (e.g., powered hub or VBUS switch).
 */

#include "db_usb_cdc_host.h"

#ifdef CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char *TAG = "DB_USB_HOST";

static StreamBufferHandle_t s_rx_stream = NULL;
static SemaphoreHandle_t s_dev_mutex = NULL;
static cdc_acm_dev_hdl_t s_dev = NULL;
static volatile bool s_ready = false;

// Forward declarations
static void db_usb_host_event_task(void *arg);
static void db_usb_cdc_acm_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static void db_usb_cdc_acm_rx_cb(cdc_acm_dev_hdl_t hdl, uint8_t *data, size_t data_len, void *user_ctx);

esp_err_t db_usb_cdc_host_init(void)
{
    if (s_rx_stream == NULL) {
        s_rx_stream = xStreamBufferCreate(8192, 1);
        if (!s_rx_stream) {
            ESP_LOGE(TAG, "Failed to create RX stream buffer");
            return ESP_FAIL;
        }
    }
    if (s_dev_mutex == NULL) {
        s_dev_mutex = xSemaphoreCreateMutex();
        if (!s_dev_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_FAIL;
        }
    }

    // Install low-level USB Host stack
    const usb_host_config_t host_cfg = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install failed");

    // Install CDC-ACM class driver
    const cdc_acm_host_driver_config_t cdc_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .event_cb = db_usb_cdc_acm_event_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(cdc_acm_host_install(&cdc_cfg), TAG, "cdc_acm_host_install failed");

    // Start a helper task to service low-level host events
    xTaskCreatePinnedToCore(db_usb_host_event_task, "usb_host_events", 4096, NULL, 5, NULL, tskNO_AFFINITY);

    s_ready = true; // Host stack running; device may attach later
    ESP_LOGI(TAG, "USB CDC Host initialized; waiting for device");
    return ESP_OK;
}

int db_usb_cdc_host_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait)
{
    if (!s_ready || !s_rx_stream || len == 0) return 0;
    return (int)xStreamBufferReceive(s_rx_stream, buf, len, ticks_to_wait);
}

int db_usb_cdc_host_write(const uint8_t *buf, size_t len, TickType_t ticks_to_wait)
{
    if (!s_ready || !buf || len == 0) return 0;
    int written = 0;
    if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_dev) {
            // Transmit as a single bulk transfer
            esp_err_t err = cdc_acm_host_data_tx_blocking(s_dev, (uint8_t *)buf, len, ticks_to_wait);
            if (err == ESP_OK) {
                written = (int)len;
            } else {
                ESP_LOGD(TAG, "cdc_acm_host_data_tx_blocking err=%d", err);
            }
        }
        xSemaphoreGive(s_dev_mutex);
    }
    return written;
}

// Event callback from CDC-ACM class driver
static void db_usb_cdc_acm_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_EVENT_DEVICE_CONNECTED: {
        ESP_LOGI(TAG, "CDC-ACM device connected: addr=%u vid=0x%04x pid=0x%04x", event->conn.dev_addr, event->conn.idVendor, event->conn.idProduct);
        cdc_acm_dev_hdl_t dev = NULL;
        cdc_acm_host_device_config_t dev_cfg = {
            .connection = {
                .dev_addr = event->conn.dev_addr,
                .vid = event->conn.idVendor,
                .pid = event->conn.idProduct,
            },
            .data_cb = db_usb_cdc_acm_rx_cb,
            .user_arg = NULL,
        };
        if (cdc_acm_host_open(&dev_cfg, &dev) == ESP_OK) {
            // Configure 115200 8N1 (harmless if ignored by the device)
            cdc_acm_line_coding_t lc = {
                .dwDTERate = 115200,
                .bDataBits = 8,
                .bParityType = 0,
                .bCharFormat = 0,
            };
            (void)cdc_acm_host_line_coding_set(dev, &lc);
            (void)cdc_acm_host_set_control_line_state(dev, true, true); // DTR/RTS

            if (xSemaphoreTake(s_dev_mutex, portMAX_DELAY) == pdTRUE) {
                s_dev = dev;
                xSemaphoreGive(s_dev_mutex);
            }
            ESP_LOGI(TAG, "CDC-ACM opened and configured");
        } else {
            ESP_LOGE(TAG, "Failed to open CDC-ACM device");
        }
        break;
    }
    case CDC_ACM_HOST_EVENT_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "CDC-ACM device disconnected");
        if (xSemaphoreTake(s_dev_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_dev) {
                (void)cdc_acm_host_close(s_dev);
                s_dev = NULL;
            }
            xSemaphoreGive(s_dev_mutex);
        }
        break;
    default:
        break;
    }
}

// Data callback when new bytes arrive from the device
static void db_usb_cdc_acm_rx_cb(cdc_acm_dev_hdl_t hdl, uint8_t *data, size_t data_len, void *user_ctx)
{
    if (!s_rx_stream || !data || data_len == 0) return;
    (void)xStreamBufferSend(s_rx_stream, data, data_len, 0);
}

// Service low-level host library events
static void db_usb_host_event_task(void *arg)
{
    while (1) {
        uint32_t events = 0;
        usb_host_lib_handle_events(1, &events);
        // Yield to other tasks; event task is lightweight
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

#endif // CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST
