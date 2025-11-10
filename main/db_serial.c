/*
 *   This file is part of DroneBridge: https://github.com/DroneBridge/ESP32
 *
 *   Copyright 2024 Wolfgang Christl
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <driver/usb_serial_jtag.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_vfs_dev.h>
#include <esp_wifi.h>
#include <inttypes.h>
#include <lwip/inet.h>
#include <stdint-gcc.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#ifdef CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST
#include "db_usb_cdc_host.h"
#endif
#include "db_esp32_control.h"
#include "db_protocol.h"
#include "db_serial.h"
#include "driver/uart.h"
#include "globals.h"
#include "main.h"
#include "msp_ltm_serial.h"
#include <db_parameters.h>

#define FASTMAVLINK_ROUTER_LINKS_MAX 3
#define FASTMAVLINK_ROUTER_COMPONENTS_MAX 5

#include "db_mavlink_msgs.h"

#define TAG "DB_SERIAL"

uint8_t DB_MAV_SYS_ID = 1;
uint32_t serial_total_byte_count = 0;
uint32_t serial_total_decoded_mav_msgs = 0;
uint16_t DB_SERIAL_READ_TIMEOUT_MS = DB_SERIAL_READ_TIMEOUT_MS_DEFAULT;

uint8_t ltm_frame_buffer[MAX_LTM_FRAMES_IN_BUFFER * LTM_MAX_FRAME_SIZE];
uint ltm_frames_in_buffer = 0;
uint ltm_frames_in_buffer_pnt = 0;

fmav_message_t msg;

// UART Self-Test variables
static bool uart_initialized_status = false;
static char uart_init_error_msg[128] = "";
static char loopback_test_status_str[32] = "not_tested";
static uint32_t bytes_received_window_start = 0;
static uint32_t bytes_received_at_window_start = 0;
static uint32_t last_reception_tick = 0;

// GPIO Scan variables
// Note: Scan is only started manually by user via web interface, never
// automatically
static gpio_scan_status_t gpio_scan_status = {0};
static TaskHandle_t gpio_scan_task_handle = NULL;
static SemaphoreHandle_t uart_mutex = NULL;

// Forward declaration
static void update_reception_stats(uint32_t bytes_received);
static void gpio_scan_task(void *pvParameters);

/**
 * Opens UART socket.
 * Enables UART flow control if RTS and CTS pins do NOT match.
 * Only open serial socket/UART if PINs are not matching - matching PIN nums
 * mean they still need to be defined by the user. No pre-defined pins since
 * ESP32 boards have wildly different pin configurations
 *
 * 8 data bits, no parity, 1 stop bit
 * @return ESP_ERROR or ESP_OK
 */
esp_err_t open_uart_serial_socket() {
  // Create mutex for UART access protection if not already created
  if (uart_mutex == NULL) {
    uart_mutex = xSemaphoreCreateMutex();
    if (uart_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create UART mutex");
      return ESP_FAIL;
    }
  }

  // Reset status tracking
  uart_initialized_status = false;
  memset(uart_init_error_msg, 0, sizeof(uart_init_error_msg));

  // only open serial socket/UART if PINs are not matching - matching PIN nums
  // mean they still need to be defined by the user no pre-defined pins as of
  // this release since ESP32 boards have wildly different pin configurations
  if (DB_PARAM_GPIO_RX == DB_PARAM_GPIO_TX) {
    ESP_LOGW(TAG,
             "Init UART socket aborted. TX GPIO == RX GPIO - Configure first!");
    snprintf(uart_init_error_msg, sizeof(uart_init_error_msg),
             "TX GPIO == RX GPIO (both %d) - Configure first!",
             DB_PARAM_GPIO_TX);
    return ESP_FAIL;
  }
  if (DB_PARAM_GPIO_TX > SOC_GPIO_IN_RANGE_MAX ||
      DB_PARAM_GPIO_RX > SOC_GPIO_IN_RANGE_MAX ||
      DB_PARAM_GPIO_CTS > SOC_GPIO_IN_RANGE_MAX ||
      DB_PARAM_GPIO_RTS > SOC_GPIO_IN_RANGE_MAX) {
    ESP_LOGW(TAG, "UART GPIO numbers out of range %i. Configure first!",
             SOC_GPIO_IN_RANGE_MAX);
    snprintf(uart_init_error_msg, sizeof(uart_init_error_msg),
             "GPIO numbers out of range (max: %d)", SOC_GPIO_IN_RANGE_MAX);
    return ESP_FAIL;
  }
  bool flow_control = DB_PARAM_GPIO_CTS != DB_PARAM_GPIO_RTS;
  ESP_LOGI(TAG,
           "UART configured: TX=GPIO%d, RX=GPIO%d, RTS=GPIO%d, CTS=GPIO%d, "
           "Baud=%" PRId32 ", FlowControl=%s",
           DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX, DB_PARAM_GPIO_RTS,
           DB_PARAM_GPIO_CTS, DB_PARAM_SERIAL_BAUD,
           flow_control ? "enabled" : "disabled");
  uart_config_t uart_config = {
      .baud_rate = DB_PARAM_SERIAL_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl =
          flow_control ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = DB_PARAM_SERIAL_RTS_THRESH,
  };
  ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
  ESP_ERROR_CHECK(
      uart_set_pin(UART_NUM, DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX,
                   flow_control ? DB_PARAM_GPIO_RTS : UART_PIN_NO_CHANGE,
                   flow_control ? DB_PARAM_GPIO_CTS : UART_PIN_NO_CHANGE));
  esp_err_t result = uart_driver_install(UART_NUM, 1024, 0, 10, NULL, 0);
  if (result == ESP_OK) {
    uart_initialized_status = true;
    ESP_LOGI(TAG, "UART driver installed successfully");
  } else {
    snprintf(uart_init_error_msg, sizeof(uart_init_error_msg),
             "uart_driver_install failed: %s", esp_err_to_name(result));
    ESP_LOGE(TAG, "UART driver installation failed: %s",
             esp_err_to_name(result));
  }
  return result;
}

/**
 * Configures the onboard USB/JTAG interface for serial communication (instead
 * of an UART). Board must support this interface.
 *
 * @return result of usb_serial_jtag_driver_install()
 */
esp_err_t open_jtag_serial_socket() {
  // Configure USB SERIAL JTAG
  usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
      .rx_buffer_size = 256,
      .tx_buffer_size = 256,
  };
  ESP_LOGI(TAG, "Initializing USB/JTAG serial interface.");
  return usb_serial_jtag_driver_install(&usb_serial_jtag_config);
}

/**
 * Opens a serial socket for communication with a serial source. On the GND this
 * is the GCS and on the air side this is the flight controller. Depending on
 * the configuration it may open a native UART socket or a JTAG based serial
 * interface. The JTAG serial based interface is a special feature of official
 * DroneBridge for ESP32 boards. Uses the onboard USB for serial I/O with GCS.
 * No FTDI required.
 *
 * @return ESP_FAIL on failure
 */
esp_err_t open_serial_socket() {
#if defined(CONFIG_DB_SERIAL_OPTION_JTAG)
  // open JTAG based serial socket for comms with FC or GCS via FTDI - special
  // feature of official DB for ESP32 boards. Uses the onboard USB for serial
  // I/O with GCS. this is basically the GND-Station mode for the ESP32
  return open_jtag_serial_socket();
#elif defined(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
  // open USB CDC-ACM host connection to the FCU via OTG port
  ESP_LOGI(TAG, "Opening USB CDC-ACM Host serial socket...");
  esp_err_t ret = db_usb_cdc_host_init();
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "USB CDC-ACM Host serial socket opened successfully");
  } else {
    ESP_LOGE(TAG, "USB CDC-ACM Host serial socket failed to open: %s (0x%x)", esp_err_to_name(ret), ret);
  }
  return ret;
#else
  // open UART based serial socket for comms with FC or GCS via FTDI -
  // configured by pins in the web interface
  return open_uart_serial_socket();
#endif
}

/**
 * Writes data from buffer to the opened serial device
 * @param data_buffer Payload to write to UART
 * @param data_length Size of payload to write to UART
 */
void write_to_serial(const uint8_t data_buffer[],
                     const unsigned int data_length) {
#if defined(CONFIG_DB_SERIAL_OPTION_JTAG)
  // Writes data from buffer to JTAG based serial interface
  int written = usb_serial_jtag_write_bytes(data_buffer, data_length,
                                            20 / portTICK_PERIOD_MS);
  if (written != data_length) {
    ESP_LOGW(TAG, "Wrote only %i of %i bytes to JTAG", written, data_length);
  } else {
    // all good. Wrote all bytes
  }
#elif defined(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
  int written = db_usb_cdc_host_write(data_buffer, data_length, 0);
  if (written != data_length) {
    ESP_LOGD(TAG, "Wrote only %i of %i bytes to USB CDC Host", written,
             data_length);
  }
#else
  // UART based serial socket for comms with FC or GCS via FTDI - configured by
  // pins in the web interface Writes data from buffer to native UART interface
  int written = uart_write_bytes(UART_NUM, data_buffer, data_length);
  if (written != data_length) {
    // This is a debug log since it happens very rarely that not all bytes get
    // written. Save some cpu cycles.
    ESP_LOGD(TAG, "Wrote only %i of %i bytes to UART", written, data_length);
  } else {
    // all good
  }
#endif
}

/**
 * Read data from the open serial interface in non-blocking fashion.
 * @param uart_read_buf Pointer to buffer to put the read bytes into
 * @param length Max length to read
 * @return number of read bytes
 */
int db_read_serial(uint8_t *uart_read_buf, uint length) {
  int bytes_read = 0;

  // Try to acquire mutex with zero timeout (non-blocking)
  // If scan is modifying UART, skip this read to avoid crash
  bool mutex_acquired = false;
  if (uart_mutex != NULL) {
    mutex_acquired = (xSemaphoreTake(uart_mutex, 0) == pdTRUE);
  }

  if (mutex_acquired || uart_mutex == NULL) {
#if defined(CONFIG_DB_SERIAL_OPTION_JTAG)
    bytes_read = usb_serial_jtag_read_bytes(uart_read_buf, length, 0);
#elif defined(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
    // Use non-blocking read (0 timeout) - the stream buffer accumulates data from USB callback
    // Reading non-blocking prevents task blocking and allows better scheduling
    bytes_read = db_usb_cdc_host_read(uart_read_buf, length, 0);
#else
    // UART based serial socket for communication with FC or GCS via FTDI -
    // configured by pins in the web interface
    bytes_read = uart_read_bytes(UART_NUM, uart_read_buf, length, 0);
#endif

    // Update reception statistics for self-test
    if (bytes_read > 0) {
      update_reception_stats(bytes_read);
    }

    if (mutex_acquired) {
      xSemaphoreGive(uart_mutex);
    }
  }
  // If mutex couldn't be acquired, return 0 (scan is modifying UART)

  return bytes_read;
}

/**
 * Check armed state of LTM packet if feature DB_PARAM_DIS_RADIO_ON_ARM is set
 * and we got a status frame. Triggers the enabling or disabling of the Wi-Fi.
 * @param db_msp_ltm_port MSP/LTM parser struct
 */
void db_ltm_check_arm_state_set_wifi(const msp_ltm_port_t *db_msp_ltm_port) {
  if (DB_PARAM_DIS_RADIO_ON_ARM && db_msp_ltm_port->ltm_type == LTM_TYPE_S) {
    if (db_msp_ltm_port->ltm_frame_buffer[2 + LTM_TYPE_S_PAYLOAD_SIZE] &
        LTM_ARMED_BIT_MASK) {
      // autopilot says it is armed
      db_set_radio_status(false); // disable Wi-Fi
    } else {
      // autopilot says it is <<not>> armed
      db_set_radio_status(true); // enable Wi-Fi
    }
  } else {
    // nothing to do
  }
}

/**
 * @brief Reads serial interface, parses & sends complete MSP & LTM messages
 * over the air.
 */
void db_parse_msp_ltm(int tcp_clients[], udp_conn_list_t *udp_connection,
                      uint8_t msp_message_buffer[],
                      unsigned int *serial_read_bytes,
                      msp_ltm_port_t *db_msp_ltm_port) {
  uint8_t serial_bytes[TRANS_RD_BYTES_NUM];
  unsigned int read;
  if ((read = db_read_serial(serial_bytes, TRANS_RD_BYTES_NUM)) > 0) {
    serial_total_byte_count += read;
    for (unsigned int j = 0; j < read; j++) {
      (*serial_read_bytes)++;
      uint8_t serial_byte = serial_bytes[j];
      if (parse_msp_ltm_byte(db_msp_ltm_port, serial_byte)) {
        msp_message_buffer[(*serial_read_bytes - 1)] = serial_byte;
        if (db_msp_ltm_port->parse_state == MSP_PACKET_RECEIVED) {
          db_send_to_all_clients(tcp_clients, udp_connection,
                                 msp_message_buffer, *serial_read_bytes);
          *serial_read_bytes = 0;
        } else if (db_msp_ltm_port->parse_state == LTM_PACKET_RECEIVED) {
          memcpy(&ltm_frame_buffer[ltm_frames_in_buffer_pnt],
                 db_msp_ltm_port->ltm_frame_buffer,
                 (db_msp_ltm_port->ltm_payload_cnt + 4));
          ltm_frames_in_buffer_pnt += (db_msp_ltm_port->ltm_payload_cnt + 4);
          ltm_frames_in_buffer++;
          db_ltm_check_arm_state_set_wifi(db_msp_ltm_port);
          if (ltm_frames_in_buffer ==
                  db_param_ltm_per_packet.value.db_param_u8.value &&
              (db_param_ltm_per_packet.value.db_param_u8.value <=
               MAX_LTM_FRAMES_IN_BUFFER)) {
            db_send_to_all_clients(tcp_clients, udp_connection,
                                   ltm_frame_buffer, *serial_read_bytes);
            ESP_LOGD(TAG, "Sent %i LTM message(s) to telemetry port!",
                     ltm_frames_in_buffer);
            ltm_frames_in_buffer = 0;
            ltm_frames_in_buffer_pnt = 0;
            *serial_read_bytes = 0;
          }
        }
      } else { // Leads to crashes of the ESP32 without it!
        *serial_read_bytes = 0;
      }
    }
  }
}

/**
 * We received some MAVLink request via the origin. Decide on which interface to
 * respond with an answer
 * @param buffer Data to send
 * @param length Data length to send
 * @param origin Origin of the MAVLink request
 * @param tcp_clients List of connected TCP client
 * @param udp_conns List of active UDP connections
 */
void db_route_mavlink_response(uint8_t *buffer, uint16_t length,
                               enum DB_MAVLINK_DATA_ORIGIN origin,
                               int *tcp_clients, udp_conn_list_t *udp_conns) {
  if (origin == DB_MAVLINK_DATA_ORIGIN_SERIAL) {
    write_to_serial(buffer, length);
  } else if (origin == DB_MAVLINK_DATA_ORIGIN_RADIO) {
    db_send_to_all_clients(tcp_clients, udp_conns, buffer, length);
  } else {
    ESP_LOGE(TAG, "Unknown msg origin. Do not know on which link to respond!");
  }
}

/**
 * Parses MAVLink coming from WiFi/ESPNOW - and sends the packet to the serial
 * output.
 *
 * @param tcp_clients Array of connected TCP clients
 * @param up_conns Structure containing all UDP connection data including the
 * sockets
 * @param buffer Buffer containing the raw bytes to be parsed
 * @param bytes_read Number of bytes in the buffer
 * @param origin Origin of the data - serial link or radio link
 */
void db_parse_mavlink_from_radio(int *tcp_clients, udp_conn_list_t *udp_conns,
                                 uint8_t *buffer, int bytes_read) {
  static uint8_t mav_parser_rx_buf[296];  // at least 280 bytes which is the max
                                          // len for a MAVLink v2 packet
  static fmav_status_t fmav_status_radio; // fmav parser status struct for
                                          // radio/ESPNOW/WiFi parser

  // Parse each byte received
  for (int i = 0; i < bytes_read; ++i) {
    fmav_result_t result = {0};
    if (fmav_parse_and_check_to_frame_buf(&result, mav_parser_rx_buf,
                                          &fmav_status_radio, buffer[i])) {
      // Parser detected a full message, write to serial
      write_to_serial(mav_parser_rx_buf, result.frame_len);
      // Decode message and react to it if it was for us
      fmav_frame_buf_to_msg(&msg, &result, mav_parser_rx_buf);
      if (result.res == FASTMAVLINK_PARSE_RESULT_OK) {
        if (fmav_msg_is_for_me(db_get_mav_sys_id(), db_get_mav_comp_id(),
                               &msg)) {
          handle_mavlink_message(&msg, tcp_clients, udp_conns,
                                 &fmav_status_radio,
                                 DB_MAVLINK_DATA_ORIGIN_RADIO);
        } else {
          // message was not for us so ignore it
        }
      } else {
        switch (result.res) {
        case FASTMAVLINK_PARSE_RESULT_MSGID_UNKNOWN:
          ESP_LOGW(TAG,
                   "fastmavlink parser had an error "
                   "FASTMAVLINK_PARSE_RESULT_MSGID_UNKNOWN msgID: %lu",
                   result.msgid);
          break;
        case FASTMAVLINK_PARSE_RESULT_LENGTH_ERROR:
          ESP_LOGW(TAG,
                   "fastmavlink parser had an error "
                   "FASTMAVLINK_PARSE_RESULT_LENGTH_ERROR msgID: %lu",
                   result.msgid);
          break;
        case FASTMAVLINK_PARSE_RESULT_CRC_ERROR:
          ESP_LOGW(TAG,
                   "fastmavlink parser had an error "
                   "FASTMAVLINK_PARSE_RESULT_CRC_ERROR msgID: %lu",
                   result.msgid);
          break;
        case FASTMAVLINK_PARSE_RESULT_SIGNATURE_ERROR:
          ESP_LOGW(TAG,
                   "fastmavlink parser had an error "
                   "FASTMAVLINK_PARSE_RESULT_SIGNATURE_ERROR msgID: %lu",
                   result.msgid);
          break;
        default:
          ESP_LOGW(TAG,
                   "fastmavlink parser had an error parsing the message: %i",
                   result.res);
          break;
        }
      }
    } else {
      // do nothing since parser did not detect a message
    }
  }
  // done parsing all received data via radio link
}

/**
 * Parses MAVLink messages and sends them via the radio link.
 * This function reads data from the serial interface, parses it for complete
 * MAVLink messages, and sends those messages in a buffer. It ensures that only
 * complete messages are sent and that the buffer does not exceed
 * TRANS_BUFF_SIZE. Checks for a serial read timeout. In case timeout is reached
 * all data read from serial so far will be flushed to radio interface The
 * parsing is done semi-transparent as in: parser understands the MavLink frame
 * format but performs no further checks
 *
 * @param tcp_clients Array of connected TCP clients
 * @param up_conns Structure containing all UDP connection data including the
 * sockets
 * @param serial_buffer Buffer that gets filled with data and then sent via
 * radio, shall be >x2 the max payload
 * @param serial_buff_pos Number of bytes already read for the current packet
 */
void db_read_serial_parse_mavlink(int *tcp_clients, udp_conn_list_t *udp_conns,
                                  uint8_t *serial_buffer,
                                  unsigned int *serial_buff_pos) {
  static uint8_t mav_parser_rx_buf[296]; // at least 280 bytes which is the max
                                         // len for a MAVLink v2 packet
  static fmav_status_t
      fmav_status_serial; // fmav parser status struct for serial parser
  uint8_t uart_read_buf[DB_PARAM_SERIAL_PACK_SIZE];
  // timeout variables
  static TickType_t last_tick = 0; // time when we received something from the
                                   // serial interface for the last time
  static TickType_t current_tick = 0;
  current_tick = xTaskGetTickCount(); // get current time

  // Read bytes from serial link (UART or USB/JTAG interface)
  int bytes_read = db_read_serial(uart_read_buf, DB_PARAM_SERIAL_PACK_SIZE);

  // Debug: Log when we read data from USB
  static int debug_read_counter = 0;
  if (bytes_read > 0) {
    debug_read_counter += bytes_read;
    if (debug_read_counter >= 100) {
      ESP_LOGI(TAG, "MAVLink: Read %d bytes from serial (total this session: %" PRIu32 ")", bytes_read, serial_total_byte_count);
      debug_read_counter = 0;
    }
  }

  if (bytes_read == 0) {
    // did not read anything this cycle -> check serial read timeout
    if (current_tick - last_tick >= pdMS_TO_TICKS(DB_SERIAL_READ_TIMEOUT_MS)) {
      // serial read timeout detected
      last_tick = current_tick; // reset timeout
      // flush buffer to air interface -> send what we have in the buffer
      // (already parsed)
      if (*serial_buff_pos > 0) {
        db_send_to_all_clients(tcp_clients, udp_conns, serial_buffer,
                               *serial_buff_pos);
        *serial_buff_pos = 0;
      } else {
        // do nothing since buffer is empty anyway
      }
    } else {
      // nothing received but no timeout yet -> do nothing
    }
  } else {
    // have received something -> reset timeout
    last_tick = current_tick;
  }

  serial_total_byte_count +=
      bytes_read; // increase total bytes read via serial interface
  // Parse each byte received
  for (int i = 0; i < bytes_read; ++i) {
    fmav_result_t result = {0};

    if (fmav_parse_and_check_to_frame_buf(&result, mav_parser_rx_buf,
                                          &fmav_status_serial,
                                          uart_read_buf[i])) {
      ESP_LOGD(
          TAG,
          "Parser detected a full message (%lu total): result.frame_len %i",
          serial_total_decoded_mav_msgs, result.frame_len);
      // Check if the new message will fit in the buffer
      if (*serial_buff_pos == 0 &&
          result.frame_len > DB_PARAM_SERIAL_PACK_SIZE) {
        // frame_len is bigger than DB_PARAM_SERIAL_PACK_SIZE -> Split into
        // multiple messages since e.g. ESP-NOW can only handle
        // DB_ESPNOW_PAYLOAD_MAXSIZE bytes which is less than MAVLink max msg
        // length
        uint16_t sent_bytes = 0;
        uint16_t next_chunk_len = 0;
        do {
          next_chunk_len = result.frame_len - sent_bytes;
          if (next_chunk_len > DB_PARAM_SERIAL_PACK_SIZE) {
            next_chunk_len = DB_PARAM_SERIAL_PACK_SIZE;
          } else {
          }
          db_send_to_all_clients(tcp_clients, udp_conns,
                                 &mav_parser_rx_buf[sent_bytes],
                                 next_chunk_len);
          sent_bytes += next_chunk_len;
        } while (sent_bytes < result.frame_len);
      } else if (*serial_buff_pos + result.frame_len >
                 DB_PARAM_SERIAL_PACK_SIZE) {
        // New message won't fit into the buffer, send buffer first
        db_send_to_all_clients(tcp_clients, udp_conns, serial_buffer,
                               *serial_buff_pos);
        *serial_buff_pos = 0;
        // copy the new message to the uart send buffer and set buffer position
        memcpy(&serial_buffer[*serial_buff_pos], mav_parser_rx_buf,
               result.frame_len);
        *serial_buff_pos += result.frame_len;
      } else {
        // copy the new message to the uart send buffer and set buffer position
        memcpy(&serial_buffer[*serial_buff_pos], mav_parser_rx_buf,
               result.frame_len);
        *serial_buff_pos += result.frame_len;
      }

      // Decode message and react to it if it was for us
      fmav_frame_buf_to_msg(&msg, &result, mav_parser_rx_buf);
      if (result.res == FASTMAVLINK_PARSE_RESULT_OK) {
        serial_total_decoded_mav_msgs++;
        if (fmav_msg_is_for_me(db_get_mav_sys_id(), db_get_mav_comp_id(),
                               &msg)) {
          // This will also instantly send a response. That is OK at this
          // position since we buffer and send out only full packets and this
          // "MAVLink packet injection" into the stream will not mess with the
          // main MAVLink packet stream.
          handle_mavlink_message(&msg, tcp_clients, udp_conns,
                                 &fmav_status_serial,
                                 DB_MAVLINK_DATA_ORIGIN_SERIAL);
        } else {
          // message was not for us so ignore it
        }
      } else {
        // message had a parsing error - we cannot decode it so skip
      }
    } else {
      // do nothing since parser had a now new message, LENGTH_ERROR, CRC_ERROR
      // or SIGNATURE_ERROR
    }
  }
  // done parsing all received data via UART
}

/**
 * Reads TRANS_RD_BYTES_NUM bytes from serial interface and checks if we already
 * got enough bytes to send them out. Timeout ensures that no data is getting
 * stuck in the buffer. Once serial read timeout is reached, the buffer will be
 * flushed to the radio interface (send what we have)
 *
 * @param tcp_clients Array of connected TCP clients
 * @param udp_connection Structure containing all UDP connection data including
 * the sockets
 * @param serial_buffer Buffer that gets filled with data and then sent via
 * radio
 * @param serial_read_bytes Number of bytes already read for the current packet
 */
void db_read_serial_parse_transparent(int tcp_clients[],
                                      udp_conn_list_t *udp_connection,
                                      uint8_t serial_buffer[],
                                      unsigned int *serial_read_bytes) {
  uint16_t read;
  static bool serial_read_timeout_reached = false;
  static TickType_t last_tick = 0; // time when we received something from the
                                   // serial interface for the last time
  static TickType_t current_tick = 0;
  current_tick = xTaskGetTickCount(); // get current time
  // read from UART directly into TCP & UDP send buffer
  if ((read = db_read_serial(
           &serial_buffer[*serial_read_bytes],
           (DB_PARAM_SERIAL_PACK_SIZE - *serial_read_bytes))) > 0) {
    serial_total_byte_count += read;     // increase total bytes read via UART
    *serial_read_bytes += read;          // set new buffer position
    serial_read_timeout_reached = false; // reset serial read timeout
    last_tick = current_tick;            // reset time for serial read timeout
  } else {
    /* did not read anything this cycle -> check serial read timeout */
    if (current_tick - last_tick >= pdMS_TO_TICKS(DB_SERIAL_READ_TIMEOUT_MS)) {
      serial_read_timeout_reached = true;
      last_tick = current_tick;
    } else {
      // no timeout detected
    }
  }
  // send serial data over the air interface
  if (*serial_read_bytes >= DB_PARAM_SERIAL_PACK_SIZE ||
      (serial_read_timeout_reached && *serial_read_bytes > 0)) {
    db_send_to_all_clients(tcp_clients, udp_connection, serial_buffer,
                           *serial_read_bytes);
    *serial_read_bytes = 0;              // reset buffer position
    serial_read_timeout_reached = false; // reset serial read timeout
  }
}

/**
 * UART Self-Test Functions
 */

/**
 * Updates reception statistics when data is received
 */
static void update_reception_stats(uint32_t bytes_received) {
  TickType_t current_tick = xTaskGetTickCount();

  // Initialize window start if first time
  if (bytes_received_window_start == 0) {
    bytes_received_window_start = current_tick;
    bytes_received_at_window_start = serial_total_byte_count;
  }

  // Update last reception time
  if (bytes_received > 0) {
    last_reception_tick = current_tick;
  }

  // Reset window every 10 seconds (10000ms / portTICK_PERIOD_MS ticks)
  if (current_tick - bytes_received_window_start >= pdMS_TO_TICKS(10000)) {
    bytes_received_window_start = current_tick;
    bytes_received_at_window_start = serial_total_byte_count;
  }
}

/**
 * Get current UART test status
 */
esp_err_t db_uart_get_test_status(uart_test_status_t *status) {
  if (status == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(status, 0, sizeof(uart_test_status_t));

  // Get pin configuration
  status->tx_pin = DB_PARAM_GPIO_TX;
  status->rx_pin = DB_PARAM_GPIO_RX;
  status->rts_pin = DB_PARAM_GPIO_RTS;
  status->cts_pin = DB_PARAM_GPIO_CTS;
  status->baud_rate = DB_PARAM_SERIAL_BAUD;

  // Get initialization status
  status->uart_initialized = uart_initialized_status;
  if (!uart_initialized_status && strlen(uart_init_error_msg) > 0) {
    strncpy(status->init_error, uart_init_error_msg,
            sizeof(status->init_error) - 1);
  }

  // Get loopback test status
  strncpy(status->loopback_test_status, loopback_test_status_str,
          sizeof(status->loopback_test_status) - 1);

  // Calculate bytes received in last 10 seconds
  TickType_t current_tick = xTaskGetTickCount();
  if (bytes_received_window_start > 0 &&
      current_tick >= bytes_received_window_start) {
    status->bytes_received_last_10s =
        serial_total_byte_count - bytes_received_at_window_start;
  } else {
    status->bytes_received_last_10s = 0;
  }

  // Get last reception timestamp
  if (last_reception_tick > 0) {
    status->last_reception_timestamp = last_reception_tick;
  } else {
    status->last_reception_timestamp = 0;
  }

  return ESP_OK;
}

/**
 * Run loopback test - sends test pattern and checks if it's received back
 * Requires TX pin to be connected to RX pin with a jumper wire
 */
esp_err_t db_uart_run_loopback_test(uint32_t duration_ms) {
#if defined(CONFIG_DB_SERIAL_OPTION_JTAG) ||                                   \
    defined(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
  // Loopback test only works with UART, not with JTAG or USB CDC
  strncpy(loopback_test_status_str, "fail",
          sizeof(loopback_test_status_str) - 1);
  ESP_LOGW(TAG, "Loopback test not available: UART mode not active");
  return ESP_FAIL;
#else
  if (!uart_initialized_status) {
    strncpy(loopback_test_status_str, "fail",
            sizeof(loopback_test_status_str) - 1);
    ESP_LOGW(TAG, "Loopback test failed: UART not initialized");
    return ESP_FAIL;
  }

  // Acquire mutex to prevent conflicts with GPIO scanner or other UART operations
  if (uart_mutex == NULL) {
    strncpy(loopback_test_status_str, "fail",
            sizeof(loopback_test_status_str) - 1);
    ESP_LOGW(TAG, "Loopback test failed: UART mutex not initialized");
    return ESP_FAIL;
  }

  // Wait for mutex with timeout (max 500ms) to let any ongoing UART operations complete
  if (xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    strncpy(loopback_test_status_str, "fail",
            sizeof(loopback_test_status_str) - 1);
    ESP_LOGW(TAG, "Loopback test failed: Could not acquire UART mutex (GPIO scanner may be running)");
    return ESP_FAIL;
  }

  strncpy(loopback_test_status_str, "running",
          sizeof(loopback_test_status_str) - 1);
  ESP_LOGI(TAG, "Starting loopback test on configured pins TX=GPIO%d, RX=GPIO%d (duration: %u ms)",
           DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX, (unsigned int)duration_ms);
  ESP_LOGI(TAG, "NOTE: Loopback test requires a jumper wire connecting TX (GPIO%d) to RX (GPIO%d)",
           DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX);

  // Test pattern: alternating 0xAA and 0x55
  uint8_t test_pattern[] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
  uint8_t receive_buffer[32];
  bool test_passed = false;
  TickType_t start_tick = xTaskGetTickCount();
  TickType_t end_tick = start_tick + pdMS_TO_TICKS(duration_ms);

  // Clear UART RX buffer by reading and discarding any pending data
  uint8_t discard_buffer[128];
  while (uart_read_bytes(UART_NUM, discard_buffer, sizeof(discard_buffer), 0) >
         0) {
    // Discard all pending data
  }

  // Send test pattern multiple times
  for (int i = 0; i < 10; i++) {
    write_to_serial(test_pattern, sizeof(test_pattern));
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Try to receive the pattern
  int total_received = 0;
  while (xTaskGetTickCount() < end_tick) {
    int received = db_read_serial(receive_buffer, sizeof(receive_buffer));
    if (received > 0) {
      total_received += received;
      // Check if we received our test pattern
      bool pattern_found = false;
      for (int i = 0; i <= received - (int)sizeof(test_pattern); i++) {
        if (memcmp(&receive_buffer[i], test_pattern, sizeof(test_pattern)) ==
            0) {
          pattern_found = true;
          break;
        }
      }
      if (pattern_found) {
        test_passed = true;
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (test_passed) {
    strncpy(loopback_test_status_str, "pass",
            sizeof(loopback_test_status_str) - 1);
    ESP_LOGI(TAG, "Loopback test PASSED on GPIO%d->GPIO%d - received %d bytes, pattern matched",
             DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX, total_received);
  } else {
    strncpy(loopback_test_status_str, "fail",
            sizeof(loopback_test_status_str) - 1);
    ESP_LOGW(TAG, "Loopback test FAILED on GPIO%d->GPIO%d - received %d bytes, pattern not found",
             DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX, total_received);
    ESP_LOGW(TAG, "Make sure TX (GPIO%d) is connected to RX (GPIO%d) with a jumper wire",
             DB_PARAM_GPIO_TX, DB_PARAM_GPIO_RX);
  }

  // Release mutex before returning
  xSemaphoreGive(uart_mutex);

  return test_passed ? ESP_OK : ESP_FAIL;
#endif
}

/**
 * Reset reception statistics
 */
void db_uart_reset_reception_stats(void) {
  bytes_received_window_start = 0;
  bytes_received_at_window_start = 0;
  last_reception_tick = 0;
}

/**
 * GPIO Scan Functions
 */

/**
 * Test a specific pin pair for UART loopback
 * Returns true if test passes, false otherwise
 */
static bool test_pin_pair(uint8_t tx_pin, uint8_t rx_pin) {
#if defined(CONFIG_DB_SERIAL_OPTION_JTAG) ||                                   \
    defined(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
  return false; // Scan only works with UART
#else
  // Validate pins are in valid range
  if (tx_pin > SOC_GPIO_IN_RANGE_MAX || rx_pin > SOC_GPIO_IN_RANGE_MAX) {
    return false;
  }

  // Skip invalid pins (strapping pins on ESP32-S3: 0, 3, 45, 46)
  // Also skip if TX == RX
  if (tx_pin == rx_pin || tx_pin == 0 || tx_pin == 3 || tx_pin == 45 ||
      tx_pin == 46 || rx_pin == 0 || rx_pin == 3 || rx_pin == 45 ||
      rx_pin == 46) {
    return false;
  }

  // Acquire mutex to prevent main task from accessing UART during test
  if (uart_mutex == NULL) {
    return false; // Mutex not initialized
  }

  // Wait for mutex with short timeout (max 100ms) to let any ongoing UART
  // operations complete, but don't block too long
  if (xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG,
             "GPIO scan: Could not acquire UART mutex, skipping pin pair %d-%d",
             tx_pin, rx_pin);
    return false;
  }

  // Save current UART driver state
  bool was_initialized = uart_initialized_status;

  // Temporarily uninstall UART driver if it's running
  if (was_initialized) {
    uart_driver_delete(UART_NUM);
    uart_initialized_status = false;
  }

  // Configure UART with test pins
  uart_config_t uart_config = {
      .baud_rate = 115200, // Use standard baud rate for testing
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
  };

  esp_err_t err = uart_param_config(UART_NUM, &uart_config);
  if (err != ESP_OK) {
    // Clean up on failure
    if (was_initialized) {
      // Restore original UART
      open_uart_serial_socket();
    } else {
      uart_initialized_status = false;
    }
    xSemaphoreGive(uart_mutex);
    taskYIELD();
    return false;
  }

  err = uart_set_pin(UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    // Clean up on failure
    if (was_initialized) {
      open_uart_serial_socket();
    } else {
      uart_initialized_status = false;
    }
    xSemaphoreGive(uart_mutex);
    taskYIELD();
    return false;
  }

  err = uart_driver_install(UART_NUM, 256, 0, 0, NULL, 0);
  if (err != ESP_OK) {
    // Clean up on failure
    if (was_initialized) {
      open_uart_serial_socket();
    } else {
      uart_initialized_status = false;
    }
    xSemaphoreGive(uart_mutex);
    taskYIELD();
    return false;
  }

  // Quick loopback test - send multiple patterns to increase chance of
  // detection
  uint8_t test_pattern[] = {0xAA, 0x55, 0xAA, 0x55};
  uint8_t receive_buffer[64];
  bool test_passed = false;

  // Clear RX buffer
  uint8_t discard[128];
  while (uart_read_bytes(UART_NUM, discard, sizeof(discard), 0) > 0) {
  }

  // Send test pattern multiple times
  for (int i = 0; i < 5; i++) {
    uart_write_bytes(UART_NUM, test_pattern, sizeof(test_pattern));
    vTaskDelay(pdMS_TO_TICKS(20)); // Wait between sends
    esp_task_wdt_reset();          // Feed watchdog during test
    taskYIELD();                   // Yield to let other tasks run
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // Reduced wait time
  esp_task_wdt_reset();

  // Try to receive - check for pattern anywhere in received data
  int total_received = 0;
  int attempts = 0;
  while (attempts < 5 &&
         total_received < (int)sizeof(test_pattern)) { // Reduced attempts
    esp_task_wdt_reset(); // Feed watchdog during receive loop
    taskYIELD();          // Yield frequently
    int received = uart_read_bytes(UART_NUM, &receive_buffer[total_received],
                                   sizeof(receive_buffer) - total_received,
                                   pdMS_TO_TICKS(30)); // Reduced timeout
    if (received > 0) {
      total_received += received;
    } else {
      attempts++;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // Check if we received our test pattern anywhere in the buffer
  if (total_received >= (int)sizeof(test_pattern)) {
    for (int i = 0; i <= total_received - (int)sizeof(test_pattern); i++) {
      if (memcmp(&receive_buffer[i], test_pattern, sizeof(test_pattern)) == 0) {
        test_passed = true;
        break;
      }
    }
  }

  // Clean up test UART
  uart_driver_delete(UART_NUM);

  // Yield before restoring to let other tasks run
  taskYIELD();
  esp_task_wdt_reset();

  // Restore original UART if it was initialized
  if (was_initialized) {
    open_uart_serial_socket();
  }

  // Release mutex after UART operations complete
  xSemaphoreGive(uart_mutex);

  // Yield after releasing mutex to let main task resume
  taskYIELD();

  return test_passed;
#endif
}

/**
 * GPIO scan task - tests adjacent pin pairs
 */
static void gpio_scan_task(void *pvParameters) {
#if defined(CONFIG_DB_SERIAL_OPTION_JTAG) ||                                   \
    defined(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
  // Scan only works with UART
  gpio_scan_status.scan_in_progress = false;
  snprintf(gpio_scan_status.scan_status, sizeof(gpio_scan_status.scan_status),
           "GPIO scan not available in JTAG/USB CDC mode");
  gpio_scan_task_handle = NULL;
  vTaskDelete(NULL);
  return;
#else
  gpio_scan_status.scan_in_progress = true;
  gpio_scan_status.result_count = 0;
  memset(gpio_scan_status.results, 0, sizeof(gpio_scan_status.results));

  snprintf(gpio_scan_status.scan_status, sizeof(gpio_scan_status.scan_status),
           "Scanning adjacent GPIO pairs...");

  ESP_LOGI(TAG, "Starting GPIO scan for adjacent pin pairs");

  // Add this task to watchdog to prevent resets during long scan
  esp_task_wdt_add(NULL);

  // Test all adjacent pin pairs in both directions
  // For each adjacent pair (n, n+1), test both (n->n+1) and (n+1->n)
  // This tests: 0->1, 1->0, 1->2, 2->1, 2->3, 3->2, etc.
  for (uint8_t pin = 0; pin < SOC_GPIO_IN_RANGE_MAX - 1 &&
                        gpio_scan_status.result_count < MAX_SCAN_RESULTS;
       pin++) {
    // Feed watchdog to prevent reset during long scan
    esp_task_wdt_reset();

    uint8_t pin_a = pin;
    uint8_t pin_b = pin + 1;

    // Skip if pins are out of valid range
    if (pin_a > SOC_GPIO_IN_RANGE_MAX || pin_b > SOC_GPIO_IN_RANGE_MAX) {
      continue;
    }

    // Skip invalid pins (strapping pins on ESP32-S3: 0, 3, 45, 46)
    // Skip the entire pair if either pin is invalid
    if (pin_a == 0 || pin_a == 3 || pin_a == 45 || pin_a == 46 || pin_b == 0 ||
        pin_b == 3 || pin_b == 45 || pin_b == 46) {
      continue; // Skip this pair entirely
    }

    // Test direction 1: pin_a as TX, pin_b as RX
    if (gpio_scan_status.result_count < MAX_SCAN_RESULTS) {
      snprintf(gpio_scan_status.scan_status,
               sizeof(gpio_scan_status.scan_status),
               "Testing GPIO%d (TX) + GPIO%d (RX)...", pin_a, pin_b);

      bool passed = test_pin_pair(pin_a, pin_b);

      gpio_scan_status.results[gpio_scan_status.result_count].tx_pin = pin_a;
      gpio_scan_status.results[gpio_scan_status.result_count].rx_pin = pin_b;
      gpio_scan_status.results[gpio_scan_status.result_count].test_passed =
          passed;

      if (passed) {
        snprintf(
            gpio_scan_status.results[gpio_scan_status.result_count].error_msg,
            sizeof(gpio_scan_status.results[gpio_scan_status.result_count]
                       .error_msg),
            "PASS");
        ESP_LOGI(TAG, "GPIO scan: GPIO%d->GPIO%d PASSED", pin_a, pin_b);
      } else {
        snprintf(
            gpio_scan_status.results[gpio_scan_status.result_count].error_msg,
            sizeof(gpio_scan_status.results[gpio_scan_status.result_count]
                       .error_msg),
            "FAIL");
      }

      gpio_scan_status.result_count++;

      // Small delay between tests to allow UART to settle and let other tasks
      // run
      vTaskDelay(pdMS_TO_TICKS(100));
      esp_task_wdt_reset();
      taskYIELD();
    }

    // Test direction 2: pin_b as TX, pin_a as RX
    if (gpio_scan_status.result_count < MAX_SCAN_RESULTS) {
      snprintf(gpio_scan_status.scan_status,
               sizeof(gpio_scan_status.scan_status),
               "Testing GPIO%d (TX) + GPIO%d (RX)...", pin_b, pin_a);

      bool passed = test_pin_pair(pin_b, pin_a);

      gpio_scan_status.results[gpio_scan_status.result_count].tx_pin = pin_b;
      gpio_scan_status.results[gpio_scan_status.result_count].rx_pin = pin_a;
      gpio_scan_status.results[gpio_scan_status.result_count].test_passed =
          passed;

      if (passed) {
        snprintf(
            gpio_scan_status.results[gpio_scan_status.result_count].error_msg,
            sizeof(gpio_scan_status.results[gpio_scan_status.result_count]
                       .error_msg),
            "PASS");
        ESP_LOGI(TAG, "GPIO scan: GPIO%d->GPIO%d PASSED", pin_b, pin_a);
      } else {
        snprintf(
            gpio_scan_status.results[gpio_scan_status.result_count].error_msg,
            sizeof(gpio_scan_status.results[gpio_scan_status.result_count]
                       .error_msg),
            "FAIL");
      }

      gpio_scan_status.result_count++;

      // Small delay between tests to allow UART to settle and let other tasks
      // run
      vTaskDelay(pdMS_TO_TICKS(100));
      esp_task_wdt_reset();
      taskYIELD();
    }
  }

  // Count passed tests
  uint8_t passed_count = 0;
  for (int i = 0; i < gpio_scan_status.result_count; i++) {
    if (gpio_scan_status.results[i].test_passed) {
      passed_count++;
    }
  }
  snprintf(gpio_scan_status.scan_status, sizeof(gpio_scan_status.scan_status),
           "Scan complete. Tested %d pairs, found %d working.",
           gpio_scan_status.result_count, passed_count);
  gpio_scan_status.scan_in_progress = false;

  ESP_LOGI(TAG, "GPIO scan completed. Tested %d pin pairs",
           gpio_scan_status.result_count);

  // Remove task from watchdog before deleting
  esp_task_wdt_delete(NULL);

  gpio_scan_task_handle = NULL;
  vTaskDelete(NULL);
#endif
}

/**
 * Get GPIO scan status
 */
esp_err_t db_uart_get_scan_status(gpio_scan_status_t *status) {
  if (status == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // Ensure result_count is within valid bounds
  uint8_t result_count = gpio_scan_status.result_count;
  if (result_count > MAX_SCAN_RESULTS) {
    result_count = MAX_SCAN_RESULTS;
  }

  // Ensure scan_status is null-terminated before copying
  gpio_scan_status.scan_status[sizeof(gpio_scan_status.scan_status) - 1] = '\0';

  // Ensure all error_msg strings are null-terminated before copying
  for (int i = 0; i < result_count; i++) {
    gpio_scan_status.results[i]
        .error_msg[sizeof(gpio_scan_status.results[i].error_msg) - 1] = '\0';
  }

  memcpy(status, &gpio_scan_status, sizeof(gpio_scan_status_t));

  // Ensure result_count is within bounds after copy
  if (status->result_count > MAX_SCAN_RESULTS) {
    status->result_count = MAX_SCAN_RESULTS;
  }

  // Ensure all strings are null-terminated after copy
  status->scan_status[sizeof(status->scan_status) - 1] = '\0';
  for (int i = 0; i < status->result_count; i++) {
    status->results[i].error_msg[sizeof(status->results[i].error_msg) - 1] =
        '\0';
  }

  // Set default message if scan hasn't started yet
  if (!status->scan_in_progress && status->result_count == 0 &&
      status->scan_status[0] == '\0') {
    strncpy(status->scan_status, "Ready - Click 'Start GPIO Scan' to begin",
            sizeof(status->scan_status) - 1);
    status->scan_status[sizeof(status->scan_status) - 1] = '\0';
  }

  return ESP_OK;
}

/**
 * Start GPIO scan
 * NOTE: This function is only called manually by user via web interface HTTP
 * handler. It is NEVER called automatically during system initialization.
 */
esp_err_t db_uart_start_gpio_scan(void) {
  if (gpio_scan_task_handle != NULL) {
    return ESP_ERR_INVALID_STATE; // Scan already in progress
  }

  BaseType_t result = xTaskCreate(gpio_scan_task, "gpio_scan_task",
                                  4096, // Stack size
                                  NULL,
                                  5, // Priority
                                  &gpio_scan_task_handle);

  if (result != pdPASS) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

/**
 * Stop GPIO scan
 */
void db_uart_stop_gpio_scan(void) {
  if (gpio_scan_task_handle != NULL) {
    vTaskDelete(gpio_scan_task_handle);
    gpio_scan_task_handle = NULL;
    gpio_scan_status.scan_in_progress = false;
  }
}
