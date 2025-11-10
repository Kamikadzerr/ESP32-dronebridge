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
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include <inttypes.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/cdc_acm_host.h"
#include "usb/cdc_acm_host_ops.h"

static const char *TAG = "DB_USB_HOST";

static StreamBufferHandle_t s_rx_stream = NULL;
static SemaphoreHandle_t s_dev_mutex = NULL;
static cdc_acm_dev_hdl_t s_dev = NULL;
static volatile bool s_ready = false;

// Status tracking
static uint16_t s_vid = 0;
static uint16_t s_pid = 0;
static uint8_t s_dev_addr = 0;
static uint32_t s_total_bytes_received = 0;
static uint32_t s_bytes_received_window_start = 0;
static uint32_t s_bytes_received_at_window_start = 0;
static TickType_t s_last_reception_tick = 0;
static uint32_t s_baud_rate = 115200; // Default baud rate

// Device detection tracking
static uint16_t s_detected_vid = 0;
static uint16_t s_detected_pid = 0;
static uint8_t s_detected_addr = 0;
static SemaphoreHandle_t s_detection_mutex = NULL;

// Forward declarations
static void db_usb_host_event_task(void *arg);
static void db_usb_cdc_acm_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);
static bool db_usb_cdc_acm_rx_cb(const uint8_t *data, size_t data_len, void *user_ctx);
static void db_usb_device_open_task(void *arg);
static void db_usb_new_device_cb(usb_device_handle_t usb_dev);

esp_err_t db_usb_cdc_host_init(void)
{
    ESP_LOGI(TAG, "=== Starting USB CDC Host initialization ===");
    
    // Check if already initialized
    if (s_ready) {
        ESP_LOGW(TAG, "USB CDC Host already initialized, skipping re-initialization");
        return ESP_OK;
    }
    
    if (s_rx_stream == NULL) {
        ESP_LOGI(TAG, "Creating RX stream buffer (16384 bytes)");
        s_rx_stream = xStreamBufferCreate(16384, 1);
        if (!s_rx_stream) {
            ESP_LOGE(TAG, "Failed to create RX stream buffer");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "RX stream buffer created successfully");
    } else {
        ESP_LOGI(TAG, "RX stream buffer already exists");
    }
    
    if (s_dev_mutex == NULL) {
        ESP_LOGI(TAG, "Creating device mutex");
        s_dev_mutex = xSemaphoreCreateMutex();
        if (!s_dev_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Device mutex created successfully");
    } else {
        ESP_LOGI(TAG, "Device mutex already exists");
    }
    
    if (s_detection_mutex == NULL) {
        ESP_LOGI(TAG, "Creating device detection mutex");
        s_detection_mutex = xSemaphoreCreateMutex();
        if (!s_detection_mutex) {
            ESP_LOGE(TAG, "Failed to create detection mutex");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Device detection mutex created successfully");
    } else {
        ESP_LOGI(TAG, "Device detection mutex already exists");
    }

    // Install low-level USB Host stack
    ESP_LOGI(TAG, "Installing USB Host stack...");
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t ret = usb_host_install(&host_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }
    ESP_LOGI(TAG, "USB Host stack installed successfully");

    // Install CDC-ACM class driver with new device callback
    ESP_LOGI(TAG, "Installing CDC-ACM class driver...");
    
    // Check available heap before installing driver
    uint32_t free_heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap before CDC driver install: %" PRIu32 " bytes", free_heap_before);
    
    const cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = tskNO_AFFINITY,
        .new_dev_cb = db_usb_new_device_cb,
    };
    ret = cdc_acm_host_install(&cdc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s (0x%x)", esp_err_to_name(ret), ret);
        uint32_t free_heap = esp_get_free_heap_size();
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        ESP_LOGE(TAG, "Free heap: %" PRIu32 " bytes", free_heap);
        ESP_LOGE(TAG, "Largest free block: %zu bytes", largest_block);
        return ret;
    }
    
    uint32_t free_heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap after CDC driver install: %" PRIu32 " bytes (used: %" PRIu32 " bytes)", 
             free_heap_after, free_heap_before - free_heap_after);
    ESP_LOGI(TAG, "CDC-ACM class driver installed successfully");

    // Start a helper task to service low-level host events
    ESP_LOGI(TAG, "Creating USB host event task...");
    BaseType_t task_ret = xTaskCreatePinnedToCore(db_usb_host_event_task, "usb_host_events", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB host event task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "USB host event task created successfully");

    // Start a task to periodically try to open USB devices
    ESP_LOGI(TAG, "Creating USB device open task...");
    task_ret = xTaskCreatePinnedToCore(db_usb_device_open_task, "usb_dev_open", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB device open task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "USB device open task created successfully");

    s_ready = true; // Host stack running; device may attach later
    ESP_LOGI(TAG, "=== USB CDC Host initialization complete ===");
    ESP_LOGI(TAG, "USB Host is ready and waiting for CDC-ACM device to connect");
    ESP_LOGI(TAG, "Note: Connect your FCU via USB to the ESP32-S3 USB-OTG port");
    ESP_LOGI(TAG, "Note: Ensure external 5V VBUS power is provided (via powered hub or VBUS switch)");
    return ESP_OK;
}

int db_usb_cdc_host_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait)
{
    if (!s_ready || !s_rx_stream || len == 0) return 0;
    int bytes_read = (int)xStreamBufferReceive(s_rx_stream, buf, len, ticks_to_wait);
    
    // Debug: Log stream buffer status occasionally
    static int debug_read_counter = 0;
    if (bytes_read > 0) {
        debug_read_counter += bytes_read;
        if (debug_read_counter >= 200) {
            size_t bytes_available = xStreamBufferBytesAvailable(s_rx_stream);
            ESP_LOGD(TAG, "USB read: %d bytes (stream has %zu bytes available)", bytes_read, bytes_available);
            debug_read_counter = 0;
        }
    }
    
    return bytes_read;
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
    ESP_LOGI(TAG, "=== USB CDC-ACM Event: type=%d ===", event->type);
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "=== CDC-ACM device disconnected ===");
        if (xSemaphoreTake(s_dev_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_dev) {
                ESP_LOGI(TAG, "Closing CDC-ACM device handle...");
                esp_err_t close_ret = cdc_acm_host_close(s_dev);
                if (close_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Device handle closed successfully");
                } else {
                    ESP_LOGW(TAG, "Device close returned: %s (0x%x)", esp_err_to_name(close_ret), close_ret);
                }
                s_dev = NULL;
            }
            s_vid = 0;
            s_pid = 0;
            s_dev_addr = 0;
            xSemaphoreGive(s_dev_mutex);
            
            // Don't clear detected VID/PID on disconnect - preserve it for faster reconnection
            // The device will likely reconnect with the same VID/PID
            ESP_LOGI(TAG, "Device state cleared, waiting for reconnection...");
            ESP_LOGI(TAG, "Preserving VID/PID (0x%04X/0x%04X) for faster reconnection", s_detected_vid, s_detected_pid);
        } else {
            ESP_LOGE(TAG, "Failed to acquire mutex during device disconnect");
        }
        break;
    default:
        ESP_LOGD(TAG, "Unhandled event type: %d", event->type);
        break;
    }
}

// Data callback when new bytes arrive from the device
static bool db_usb_cdc_acm_rx_cb(const uint8_t *data, size_t data_len, void *user_ctx)
{
    if (!s_rx_stream || !data || data_len == 0) return false;
    
    size_t sent = xStreamBufferSend(s_rx_stream, data, data_len, 0);
    if (sent != data_len) {
        ESP_LOGW(TAG, "RX buffer full: received %zu bytes, only %zu bytes sent to stream", data_len, sent);
    } else {
        // Only log occasionally to avoid spam (log every 100 bytes or so)
        static size_t log_counter = 0;
        log_counter += data_len;
        if (log_counter >= 100) {
            ESP_LOGD(TAG, "Received %zu bytes from USB device (total: %" PRIu32 ")", data_len, s_total_bytes_received);
            log_counter = 0;
        }
    }
    
    // Update reception statistics
    TickType_t current_tick = xTaskGetTickCount();
    s_total_bytes_received += data_len;
    s_last_reception_tick = current_tick;
    
    // Initialize window start if first time
    if (s_bytes_received_window_start == 0) {
        s_bytes_received_window_start = current_tick;
        s_bytes_received_at_window_start = s_total_bytes_received;
    }
    
    // Reset window every 10 seconds
    if (current_tick - s_bytes_received_window_start >= pdMS_TO_TICKS(10000)) {
        s_bytes_received_window_start = current_tick;
        s_bytes_received_at_window_start = s_total_bytes_received;
    }
    
    return true; // Data processed, flush RX buffer
}

// Task to periodically try to open USB CDC-ACM device
static void db_usb_device_open_task(void *arg)
{
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = db_usb_cdc_acm_event_cb,
        .data_cb = db_usb_cdc_acm_rx_cb,
        .user_arg = NULL,
    };

    while (1) {
        // Check if device is already open
        bool device_open = false;
        if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            device_open = (s_dev != NULL);
            xSemaphoreGive(s_dev_mutex);
        }

        if (!device_open) {
            // Get detected VID/PID from callback (if available)
            uint16_t vid_to_use = CDC_HOST_ANY_VID;
            uint16_t pid_to_use = CDC_HOST_ANY_PID;
            uint8_t dev_addr = 0;
            
            if (xSemaphoreTake(s_detection_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (s_detected_vid != 0 && s_detected_pid != 0) {
                    vid_to_use = s_detected_vid;
                    pid_to_use = s_detected_pid;
                    dev_addr = s_detected_addr;
                    ESP_LOGI(TAG, "Using detected device: VID=0x%04X, PID=0x%04X, Addr=%u", vid_to_use, pid_to_use, dev_addr);
                }
                xSemaphoreGive(s_detection_mutex);
            }
            
            // Try to open CDC-ACM device
            cdc_acm_dev_hdl_t dev = NULL;
            if (vid_to_use == CDC_HOST_ANY_VID) {
                ESP_LOGI(TAG, "Attempting to open CDC-ACM device (any VID/PID)...");
            } else {
                ESP_LOGI(TAG, "Attempting to open CDC-ACM device VID=0x%04X PID=0x%04X...", vid_to_use, pid_to_use);
            }
            esp_err_t open_ret = cdc_acm_host_open(vid_to_use, pid_to_use, 0, &dev_config, &dev);
            if (open_ret == ESP_OK) {
                ESP_LOGI(TAG, "CDC-ACM device opened successfully");
                
                // Use detected VID/PID or get from device descriptor if available
                uint16_t vid = vid_to_use;
                uint16_t pid = pid_to_use;
                
                // If we used ANY_VID/ANY_PID, try to get actual values from detection
                if (vid == CDC_HOST_ANY_VID) {
                    if (xSemaphoreTake(s_detection_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        if (s_detected_vid != 0 && s_detected_pid != 0) {
                            vid = s_detected_vid;
                            pid = s_detected_pid;
                            dev_addr = s_detected_addr;
                            ESP_LOGI(TAG, "Retrieved device info from detection: VID=0x%04X, PID=0x%04X, Addr=%u", vid, pid, dev_addr);
                        }
                        xSemaphoreGive(s_detection_mutex);
                    }
                } else {
                    // We used specific VID/PID - verify device address matches detected one
                    if (xSemaphoreTake(s_detection_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        if (s_detected_vid == vid && s_detected_pid == pid) {
                            // Use the detected device address if VID/PID match
                            dev_addr = s_detected_addr;
                            ESP_LOGI(TAG, "Using detected device address %u for VID=0x%04X PID=0x%04X", dev_addr, vid, pid);
                        } else {
                            ESP_LOGW(TAG, "Detected device (VID=0x%04X PID=0x%04X Addr=%u) differs from opened device (VID=0x%04X PID=0x%04X)", 
                                     s_detected_vid, s_detected_pid, s_detected_addr, vid, pid);
                        }
                        xSemaphoreGive(s_detection_mutex);
                    }
                }
                
                // Configure 115200 8N1 (harmless if ignored by the device)
                ESP_LOGI(TAG, "Configuring line coding: 115200 8N1");
                cdc_acm_line_coding_t lc = {
                    .dwDTERate = 115200,
                    .bDataBits = 8,
                    .bParityType = 0,
                    .bCharFormat = 0,
                };
                esp_err_t lc_ret = cdc_acm_host_line_coding_set(dev, &lc);
                if (lc_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Line coding set successfully");
                } else {
                    ESP_LOGW(TAG, "Line coding set returned: %s (0x%x) - device may ignore this", esp_err_to_name(lc_ret), lc_ret);
                }
                
                ESP_LOGI(TAG, "Setting control line state (DTR/RTS)...");
                esp_err_t ctrl_ret = cdc_acm_host_set_control_line_state(dev, true, true); // DTR/RTS
                if (ctrl_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Control line state set successfully");
                } else {
                    ESP_LOGW(TAG, "Control line state set returned: %s (0x%x)", esp_err_to_name(ctrl_ret), ctrl_ret);
                }

                if (xSemaphoreTake(s_dev_mutex, portMAX_DELAY) == pdTRUE) {
                    s_dev = dev;
                    s_vid = vid;
                    s_pid = pid;
                    s_dev_addr = dev_addr;
                    s_baud_rate = 115200; // Default, may be updated if device reports different
                    xSemaphoreGive(s_dev_mutex);
                    ESP_LOGI(TAG, "Device handle stored: VID=0x%04X, PID=0x%04X, Addr=%u", s_vid, s_pid, s_dev_addr);
                    ESP_LOGI(TAG, "=== CDC-ACM device fully configured and ready ===");
                } else {
                    ESP_LOGE(TAG, "Failed to acquire mutex to store device handle");
                    cdc_acm_host_close(dev);
                }
            } else {
                // Device not available yet, wait before retrying
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            // Device is open, wait longer before checking again
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

// New device callback - called when a USB device is detected
// Note: This callback is called for ALL USB devices, not just CDC-ACM devices
// We store the VID/PID here, and the CDC-ACM driver will filter for CDC-ACM class devices
static void db_usb_new_device_cb(usb_device_handle_t usb_dev)
{
    if (!usb_dev) return;
    
    // Get device descriptor to extract VID/PID
    const usb_device_desc_t *device_desc = NULL;
    esp_err_t ret = usb_host_get_device_descriptor(usb_dev, &device_desc);
    if (ret == ESP_OK && device_desc) {
        uint16_t vid = device_desc->idVendor;
        uint16_t pid = device_desc->idProduct;
        
        // Get device address
        usb_device_info_t dev_info;
        uint8_t dev_addr = 0;
        if (usb_host_device_info(usb_dev, &dev_info) == ESP_OK) {
            dev_addr = dev_info.dev_addr;
        }
        
        ESP_LOGI(TAG, "New USB device detected: VID=0x%04X, PID=0x%04X, Addr=%u", vid, pid, dev_addr);
        
        // Store detected device info (will be used when opening CDC-ACM device)
        // Note: We store VID/PID for any device, but cdc_acm_host_open will only
        // succeed if the device is actually a CDC-ACM device
        if (xSemaphoreTake(s_detection_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_detected_vid = vid;
            s_detected_pid = pid;
            s_detected_addr = dev_addr;
            xSemaphoreGive(s_detection_mutex);
            ESP_LOGI(TAG, "Stored device info: VID=0x%04X, PID=0x%04X, Addr=%u (will try to open as CDC-ACM)", vid, pid, dev_addr);
        } else {
            ESP_LOGW(TAG, "Failed to acquire detection mutex");
        }
    } else {
        ESP_LOGW(TAG, "Failed to get device descriptor: %s (0x%x)", esp_err_to_name(ret), ret);
    }
}

// Service low-level host library events
static void db_usb_host_event_task(void *arg)
{
    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
        }
    }
}

// Get current USB CDC Host status
esp_err_t db_usb_cdc_host_get_status(usb_cdc_host_status_t *status)
{
    if (status == NULL) {
        ESP_LOGE(TAG, "db_usb_cdc_host_get_status: status pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(usb_cdc_host_status_t));

    ESP_LOGD(TAG, "Getting USB status: s_ready=%d, s_dev=%p", s_ready, s_dev);
    status->host_initialized = s_ready;
    
    if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        status->device_connected = (s_dev != NULL);
        status->vid = s_vid;
        status->pid = s_pid;
        status->device_address = s_dev_addr;
        status->baud_rate = s_baud_rate;
        xSemaphoreGive(s_dev_mutex);
        ESP_LOGD(TAG, "Status: initialized=%d, connected=%d, VID=0x%04X, PID=0x%04X", 
                 status->host_initialized, status->device_connected, status->vid, status->pid);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for status check");
        status->device_connected = false;
    }

    // Calculate bytes received in last 10 seconds
    TickType_t current_tick = xTaskGetTickCount();
    if (s_bytes_received_window_start > 0 && current_tick >= s_bytes_received_window_start) {
        status->bytes_received_last_10s = s_total_bytes_received - s_bytes_received_at_window_start;
    } else {
        status->bytes_received_last_10s = 0;
    }

    // Get last reception timestamp
    if (s_last_reception_tick > 0) {
        status->last_reception_timestamp = s_last_reception_tick;
    } else {
        status->last_reception_timestamp = 0;
    }

    // Set status message
    if (!s_ready) {
        snprintf(status->status_message, sizeof(status->status_message), "USB Host not initialized");
    } else if (!status->device_connected) {
        snprintf(status->status_message, sizeof(status->status_message), "Waiting for USB device...");
    } else {
        snprintf(status->status_message, sizeof(status->status_message), 
                 "Connected: VID=0x%04X PID=0x%04X", status->vid, status->pid);
    }

    return ESP_OK;
}

#endif // CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST
