/*
 *   This file is part of DroneBridge: https://github.com/DroneBridge/ESP32
 *
 *   USB CDC-ACM Host transport for ESP32-S2/S3 (USB-OTG).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize USB Host + CDC-ACM host and wait for a CDC device to be available.
// Returns ESP_OK when the host stack is ready (device may attach later).
esp_err_t db_usb_cdc_host_init(void);

// Non-blocking read; returns number of bytes copied into buf (0 if none).
int db_usb_cdc_host_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait);

// Non-blocking write; returns number of bytes accepted for transmission.
int db_usb_cdc_host_write(const uint8_t *buf, size_t len, TickType_t ticks_to_wait);

// USB CDC Host status structure
typedef struct {
    bool host_initialized;
    bool device_connected;
    uint16_t vid;
    uint16_t pid;
    uint8_t device_address;
    uint32_t baud_rate;
    uint32_t bytes_received_last_10s;
    uint32_t last_reception_timestamp;
    char status_message[128];
} usb_cdc_host_status_t;

// Get current USB CDC Host status
esp_err_t db_usb_cdc_host_get_status(usb_cdc_host_status_t *status);

#ifdef __cplusplus
}
#endif

