/*
 *   This file is part of DroneBridge: https://github.com/DroneBridge/ESP32
 *
 *   Copyright 2021 Wolfgang Christl
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

#include "http_server.h"

#include <db_parameters.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <esp_chip_info.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "cJSON.h"
#include "globals.h"
#include "main.h"
#include "db_serial.h"
#ifdef CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST
#include "db_usb_cdc_host.h"
#endif

#define TAG "DB_HTTP_REST"
#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

/**
 * Set HTTP response content type according to file extension
 */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath) {
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

/**
 * Send HTTP response with the contents of the requested file
 */
static esp_err_t rest_common_get_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX];

    rest_server_context_t *rest_context = (rest_server_context_t *) req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * Process incoming settings change and reply with HTTP success message
 * @param req
 * @return ESP error code
 */
static esp_err_t settings_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        // This should be HTTPD_414_PAYLOAD_TOO_LARGE, but that's not
        // implemented yet, so use 400 Bad Request instead.
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    db_param_read_all_params_json(root);
    db_write_settings_to_nvs();
    ESP_LOGI(TAG, "Settings changed!");
    
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\n"
                            "    \"status\": \"success\",\n"
                            "    \"msg\": \"Settings changed! Rebooting ...\"\n"
                            "  }");
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // wait to allow the website displaying the success message
    // Send reboot message
    httpd_resp_set_type(req, "application/json");
    cJSON *reboot_info = cJSON_CreateObject();
    cJSON_AddStringToObject(reboot_info, "msg", "Rebooting!");
    const char *sys_info = cJSON_Print(reboot_info);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(reboot_info);
    vTaskDelay(500 / portTICK_PERIOD_MS);  // wait 1s to allow the website displaying the success message
    esp_restart();
    return ESP_OK;
}

/**
 * Process incoming UDP connection add request. Only one IPv4 connection can be added at a time
 * Expecting JSON in the form of:
 * {
 *   "ip": "XXX.XXX.XXX.XXX",
 *   "port": 452
 * }
 * @param req
 * @return
 */
static esp_err_t settings_clients_udp_post(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    // Obtain & process JSON from request
    cJSON *root = cJSON_Parse(buf);

    int new_udp_port = 0;
    char new_ip[IP4ADDR_STRLEN_MAX];
    uint8_t save_to_nvm = false;
    cJSON *json = cJSON_GetObjectItem(root, (char *) db_param_udp_client_ip.db_name);
    if (json) strncpy(new_ip, json->valuestring, sizeof(new_ip));
    new_ip[IP4ADDR_STRLEN_MAX-1] = '\0';    // to remove warning and to be sure
    json = cJSON_GetObjectItem(root, (char *) db_param_udp_client_port.db_name);
    if (json) new_udp_port = json->valueint;
    json = cJSON_GetObjectItem(root, "save");
    if (json && cJSON_IsBool(json)) {
        if(cJSON_IsTrue(json)) {
            save_to_nvm = true;
        } else {
            save_to_nvm = false;
        }
    } else {}

    // populate the UDP connections list with a new connection
    struct sockaddr_in new_sockaddr;
    memset(&new_sockaddr, 0, sizeof(new_sockaddr));
    new_sockaddr.sin_family = AF_INET;
    inet_pton(AF_INET, new_ip, &new_sockaddr.sin_addr);
    new_sockaddr.sin_port = htons(new_udp_port);
    struct db_udp_client_t new_udp_client = {
            .udp_client = new_sockaddr,
            .mac = {0, 0, 0, 0, 0, 0}   // dummy MAC
    };
    // udp_conn_list is initialized as the very first thing during startup - we expect it to be there
    bool success = add_to_known_udp_clients(udp_conn_list, new_udp_client, save_to_nvm);

    // Clean up
    cJSON_Delete(root);
    if (success) {
        httpd_resp_sendstr(req, "{\n"
                                "    \"status\": \"success\",\n"
                                "    \"msg\": \"Added UDP connection!\"\n"
                                "  }");
    } else {
        httpd_resp_sendstr(req, "{\n"
                                "    \"status\": \"failed\",\n"
                                "    \"msg\": \"Failed to add UDP connection!\"\n"
                                "  }");
    }

    return ESP_OK;
}

/**
 * Process a request that shall clear all active UDP connections.
 * ESP32 will remove all maintained UDP connections from its internal list and UDP clients will have to register again.
 * This also clears the one UDP client connection that gets saved to NVM.
 *
 * @param req
 * @return ESP_OK if all was good. Else ESP_FAIL
 */
static esp_err_t settings_clients_clear_udp_get(httpd_req_t *req) {
    for (int i = 0; i < udp_conn_list->size; ++i) {
        memset(&udp_conn_list->db_udp_clients[i], 0, sizeof(struct db_udp_client_t));
    }
    udp_conn_list->size = 0;
    ESP_LOGI(TAG, "Removed all UDP clients from list!");
    // Clear saved client as well. Pass any client since it will be ignored as long as clear_client is set to true.
    save_udp_client_to_nvm(&udp_conn_list->db_udp_clients[0], true);
    return ESP_OK;
}

/**
 * Returns build information esp-idf version and build version
 * @param req
 * @return
 */
static esp_err_t system_info_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "idf_version", IDF_VER);
    cJSON_AddNumberToObject(root, "db_build_version", DB_BUILD_VERSION);
    cJSON_AddNumberToObject(root, "major_version", DB_MAJOR_VERSION);
    cJSON_AddNumberToObject(root, "minor_version", DB_MINOR_VERSION);
    cJSON_AddNumberToObject(root, "patch_version", DB_PATCH_VERSION);
    cJSON_AddStringToObject(root, "maturity_version", DB_MATURITY_VERSION);
    cJSON_AddNumberToObject(root, "esp_chip_model", chip_info.model);
    cJSON_AddNumberToObject(root, "has_rf_switch", DB_HAS_RF_SWITCH);
    char mac_str[18];
    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
            LOCAL_MAC_ADDRESS[0], LOCAL_MAC_ADDRESS[1], LOCAL_MAC_ADDRESS[2], LOCAL_MAC_ADDRESS[3], LOCAL_MAC_ADDRESS[4], LOCAL_MAC_ADDRESS[5]);
    cJSON_AddStringToObject(root, "esp_mac", mac_str);
#ifdef CONFIG_DB_SERIAL_OPTION_JTAG
    cJSON_AddNumberToObject(root, "serial_via_JTAG", 1);
#else
    cJSON_AddNumberToObject(root, "serial_via_JTAG", 0);
#endif
#ifdef CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST
    cJSON_AddNumberToObject(root, "serial_via_USB", 1);
#else
    cJSON_AddNumberToObject(root, "serial_via_USB", 0);
#endif
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Returns a JSON containing read bytes from UART, number of connected TCP connections and UDP broadcasts as well as the
 * current IP address of the ESP32
 * @param req
 * @return ESP_OK on successfully sending the http request
 */
static esp_err_t system_stats_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "read_bytes", serial_total_byte_count);
    cJSON_AddNumberToObject(root, "serial_dec_mav_msgs", serial_total_decoded_mav_msgs);
    cJSON_AddNumberToObject(root, "tcp_connected", num_connected_tcp_clients);
    cJSON_AddNumberToObject(root, "udp_connected", udp_conn_list->size);
    // add IP:PORT info on connected UDP clients
    cJSON *udp_clients = cJSON_CreateArray();
    for (int i = 0; i < udp_conn_list->size; i++) {
        char ip_string[INET_ADDRSTRLEN];
        char ip_port_string[INET_ADDRSTRLEN+10];
        inet_ntop(AF_INET, &(udp_conn_list->db_udp_clients[i].udp_client.sin_addr), ip_string, INET_ADDRSTRLEN);
        sprintf(ip_port_string, "%s:%d", ip_string, htons (udp_conn_list->db_udp_clients[i].udp_client.sin_port));
        cJSON_AddItemToArray(udp_clients, cJSON_CreateString(ip_port_string));
    }
    cJSON_AddItemToObject(root, "udp_clients", udp_clients);
    // add RSSI and IP info
    if (DB_PARAM_RADIO_MODE == DB_WIFI_MODE_STA) {
        cJSON_AddStringToObject(root, "current_client_ip", CURRENT_CLIENT_IP);
        cJSON_AddNumberToObject(root, "esp_rssi", db_esp_signal_quality.air_rssi);
    } else if (DB_PARAM_RADIO_MODE == DB_WIFI_MODE_AP || DB_PARAM_RADIO_MODE == DB_WIFI_MODE_AP_LR) {
        cJSON *sta_array = cJSON_AddArrayToObject(root, "connected_sta");
        for (int i = 0; i < wifi_sta_list.num; i++) {
            cJSON *connected_stations_status = cJSON_CreateObject();
            char mac_str[18];
            sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                    wifi_sta_list.sta[i].mac[0], wifi_sta_list.sta[i].mac[1], wifi_sta_list.sta[i].mac[2],
                    wifi_sta_list.sta[i].mac[3], wifi_sta_list.sta[i].mac[4], wifi_sta_list.sta[i].mac[5]);
            cJSON_AddStringToObject(connected_stations_status, "sta_mac", mac_str);
            cJSON_AddNumberToObject(connected_stations_status, "sta_rssi", wifi_sta_list.sta[i].rssi);
            cJSON_AddItemToArray(sta_array, connected_stations_status);
        }
    } else {
        // other modes like ESP-NOW do not activate HTTP server so do nothing
    }
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Returns a JSON containing all active UDP connections that the ESP32 sends to
 * @param req
 * @return ESP_OK on successfully sending the http request
 */
static esp_err_t system_clients_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
//    cJSON *tcp_clients = cJSON_CreateArray();
//    for (int i = 0; i < udp_conn_list->size; i++) {
//        //TODO: Save the TCP IPs and port during accept and put them here into a JSON
//    }
//    cJSON_AddItemToObject(root, "tcp_clients", tcp_clients);

    cJSON *udp_clients = cJSON_CreateArray();
    for (int i = 0; i < udp_conn_list->size; i++) {
        char ip_string[INET_ADDRSTRLEN];
        char ip_port_string[INET_ADDRSTRLEN+10];
        inet_ntop(AF_INET, &(udp_conn_list->db_udp_clients[i].udp_client.sin_addr), ip_string, INET_ADDRSTRLEN);
        sprintf(ip_port_string, "%s:%d", ip_string, htons (udp_conn_list->db_udp_clients[i].udp_client.sin_port));
        cJSON_AddItemToArray(udp_clients, cJSON_CreateString(ip_port_string));
    }
    cJSON_AddItemToObject(root, "udp_clients", udp_clients);

    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Respond with all internally known settings
 * @param req
 * @return ESP_OK on successfully sending the http request
 */
static esp_err_t settings_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    db_param_write_all_params_json(root);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Returns UART test status including pin configuration and reception statistics
 * @param req
 * @return ESP_OK on successfully sending the http request
 */
static esp_err_t uart_test_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    uart_test_status_t status;
    esp_err_t err = db_uart_get_test_status(&status);
    
    cJSON *root = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(root, "tx_pin", status.tx_pin);
        cJSON_AddNumberToObject(root, "rx_pin", status.rx_pin);
        cJSON_AddNumberToObject(root, "rts_pin", status.rts_pin);
        cJSON_AddNumberToObject(root, "cts_pin", status.cts_pin);
        cJSON_AddNumberToObject(root, "baud_rate", status.baud_rate);
        cJSON_AddBoolToObject(root, "uart_initialized", status.uart_initialized);
        if (strlen(status.init_error) > 0) {
            cJSON_AddStringToObject(root, "init_error", status.init_error);
        }
        cJSON_AddStringToObject(root, "loopback_test_status", status.loopback_test_status);
        cJSON_AddNumberToObject(root, "bytes_received_last_10s", status.bytes_received_last_10s);
        if (status.last_reception_timestamp > 0) {
            cJSON_AddNumberToObject(root, "last_reception_timestamp", status.last_reception_timestamp);
        } else {
            cJSON_AddStringToObject(root, "last_reception_timestamp", "never");
        }
    } else {
        cJSON_AddStringToObject(root, "error", "Failed to get UART test status");
    }
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_sendstr(req, json_str);
    free((void *) json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Handles UART test commands (e.g., loopback test)
 * Expects JSON: {"test": "loopback", "duration_ms": 1000}
 * @param req
 * @return ESP_OK on successfully processing the request
 */
static esp_err_t uart_test_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    
    if (total_len >= SCRATCH_BUFSIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "content too long");
        return ESP_FAIL;
    }
    
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive request");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';
    
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    cJSON *response = cJSON_CreateObject();
    
    cJSON *test_type = cJSON_GetObjectItem(root, "test");
    if (test_type && cJSON_IsString(test_type)) {
        if (strcmp(test_type->valuestring, "loopback") == 0) {
            uint32_t duration_ms = 1000; // default
            cJSON *duration = cJSON_GetObjectItem(root, "duration_ms");
            if (duration && cJSON_IsNumber(duration)) {
                duration_ms = duration->valueint;
            }
            
            esp_err_t result = db_uart_run_loopback_test(duration_ms);
            if (result == ESP_OK) {
                cJSON_AddStringToObject(response, "status", "success");
                cJSON_AddStringToObject(response, "message", "Loopback test passed");
            } else {
                cJSON_AddStringToObject(response, "status", "failed");
                cJSON_AddStringToObject(response, "message", "Loopback test failed - check if TX is connected to RX");
            }
        } else {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Unknown test type");
        }
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Missing or invalid 'test' field");
    }
    
    const char *json_str = cJSON_Print(response);
    httpd_resp_sendstr(req, json_str);
    free((void *) json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Returns GPIO scan status
 * @param req
 * @return ESP_OK on successfully sending the http request
 */
static esp_err_t uart_scan_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    gpio_scan_status_t *status = calloc(1, sizeof(gpio_scan_status_t));
    if (status == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    esp_err_t err = db_uart_get_scan_status(status);
    
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(status);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON object");
        return ESP_FAIL;
    }
    
    if (err == ESP_OK) {
        // Ensure strings are null-terminated
        status->scan_status[sizeof(status->scan_status) - 1] = '\0';
        
        cJSON_AddBoolToObject(root, "scan_in_progress", status->scan_in_progress);
        cJSON_AddStringToObject(root, "scan_status", status->scan_status);
        cJSON_AddNumberToObject(root, "result_count", status->result_count);
        
        cJSON *results = cJSON_CreateArray();
        if (results != NULL) {
            // Ensure result_count is within bounds
            int result_count = status->result_count;
            if (result_count > MAX_SCAN_RESULTS) {
                result_count = MAX_SCAN_RESULTS;
            }
            
            for (int i = 0; i < result_count; i++) {
                cJSON *result = cJSON_CreateObject();
                if (result != NULL) {
                    // Ensure error_msg is null-terminated
                    status->results[i].error_msg[sizeof(status->results[i].error_msg) - 1] = '\0';
                    
                    cJSON_AddNumberToObject(result, "tx_pin", status->results[i].tx_pin);
                    cJSON_AddNumberToObject(result, "rx_pin", status->results[i].rx_pin);
                    cJSON_AddBoolToObject(result, "test_passed", status->results[i].test_passed);
                    cJSON_AddStringToObject(result, "error_msg", status->results[i].error_msg);
                    cJSON_AddItemToArray(results, result);
                }
            }
            cJSON_AddItemToObject(root, "results", results);
        }
    } else {
        cJSON_AddStringToObject(root, "error", "Failed to get GPIO scan status");
    }
    
    const char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        free(status);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }
    
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free((void *) json_str);
    cJSON_Delete(root);
    free(status);
    return ret;
}

/**
 * Handles GPIO scan commands (start/stop)
 * Expects JSON: {"action": "start"} or {"action": "stop"}
 * @param req
 * @return ESP_OK on successfully processing the request
 */
static esp_err_t uart_scan_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    
    if (total_len >= SCRATCH_BUFSIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "content too long");
        return ESP_FAIL;
    }
    
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive request");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';
    
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    cJSON *response = cJSON_CreateObject();
    
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (action && cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "start") == 0) {
            esp_err_t result = db_uart_start_gpio_scan();
            if (result == ESP_OK) {
                cJSON_AddStringToObject(response, "status", "success");
                cJSON_AddStringToObject(response, "message", "GPIO scan started");
            } else if (result == ESP_ERR_INVALID_STATE) {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "message", "Scan already in progress");
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "message", "Failed to start scan");
            }
        } else if (strcmp(action->valuestring, "stop") == 0) {
            db_uart_stop_gpio_scan();
            cJSON_AddStringToObject(response, "status", "success");
            cJSON_AddStringToObject(response, "message", "GPIO scan stopped");
        } else {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Unknown action");
        }
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Missing or invalid 'action' field");
    }
    
    const char *json_str = cJSON_Print(response);
    httpd_resp_sendstr(req, json_str);
    free((void *) json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Returns USB CDC Host status
 * @param req
 * @return ESP_OK on successfully sending the http request
 */
static esp_err_t usb_status_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
#ifdef CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST
    usb_cdc_host_status_t status;
    esp_err_t err = db_usb_cdc_host_get_status(&status);
    
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON object");
        return ESP_FAIL;
    }
    
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "host_initialized", status.host_initialized);
        cJSON_AddBoolToObject(root, "device_connected", status.device_connected);
        if (status.device_connected) {
            cJSON_AddNumberToObject(root, "vid", status.vid);
            cJSON_AddNumberToObject(root, "pid", status.pid);
            cJSON_AddNumberToObject(root, "device_address", status.device_address);
            cJSON_AddNumberToObject(root, "baud_rate", status.baud_rate);
        }
        cJSON_AddNumberToObject(root, "bytes_received_last_10s", status.bytes_received_last_10s);
        if (status.last_reception_timestamp > 0) {
            cJSON_AddNumberToObject(root, "last_reception_timestamp", status.last_reception_timestamp);
        } else {
            cJSON_AddStringToObject(root, "last_reception_timestamp", "never");
        }
        cJSON_AddStringToObject(root, "status_message", status.status_message);
    } else {
        cJSON_AddStringToObject(root, "error", "Failed to get USB status");
    }
    
    const char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json_str);
    free((void *) json_str);
    cJSON_Delete(root);
#else
    // USB CDC Host not configured - return "not available" response
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON object");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "host_initialized", false);
    cJSON_AddBoolToObject(root, "device_connected", false);
    cJSON_AddStringToObject(root, "status_message", "USB CDC Host not configured");
    cJSON_AddStringToObject(root, "error", "USB CDC Host feature not enabled in this build");
    
    const char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to serialize JSON");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json_str);
    free((void *) json_str);
    cJSON_Delete(root);
#endif
    return ESP_OK;
}

esp_err_t start_rest_server(const char *base_path) {
    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 14; // Increased for USB status endpoint

    ESP_LOGI(TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
            .uri = "/api/system/info",
            .method = HTTP_GET,
            .handler = system_info_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for fetching client connection info */
    httpd_uri_t system_clients_get_uri = {
            .uri = "/api/system/clients",
            .method = HTTP_GET,
            .handler = system_clients_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_clients_get_uri);

    /* URI handler for fetching system info */
    httpd_uri_t system_stats_get_uri = {
            .uri = "/api/system/stats",
            .method = HTTP_GET,
            .handler = system_stats_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_stats_get_uri);

    /* URI handler for fetching settings data */
    httpd_uri_t settings_get_uri = {
            .uri = "/api/settings",
            .method = HTTP_GET,
            .handler = settings_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &settings_get_uri);

    httpd_uri_t settings_post_uri = {
            .uri = "/api/settings",
            .method = HTTP_POST,
            .handler = settings_post_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &settings_post_uri);

    /* URI handler for adding a new udp client connection */
    httpd_uri_t settings_clients_udp_post_uri = {
            .uri = "/api/settings/clients/udp",
            .method = HTTP_POST,
            .handler = settings_clients_udp_post,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &settings_clients_udp_post_uri);

    /* URI handler for removing all known UDP client connections */
    httpd_uri_t settings_clients_clear_udp_get_uri = {
            .uri = "/api/settings/clients/clear_udp",
            .method = HTTP_DELETE,
            .handler = settings_clients_clear_udp_get,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &settings_clients_clear_udp_get_uri);

    /* URI handler for UART test status (GET) */
    httpd_uri_t uart_test_get_uri = {
            .uri = "/api/uart_test",
            .method = HTTP_GET,
            .handler = uart_test_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &uart_test_get_uri);

    /* URI handler for UART test commands (POST) */
    httpd_uri_t uart_test_post_uri = {
            .uri = "/api/uart_test",
            .method = HTTP_POST,
            .handler = uart_test_post_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &uart_test_post_uri);

    /* URI handler for GPIO scan status (GET) */
    httpd_uri_t uart_scan_get_uri = {
            .uri = "/api/uart_scan",
            .method = HTTP_GET,
            .handler = uart_scan_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &uart_scan_get_uri);

    /* URI handler for GPIO scan commands (POST) */
    httpd_uri_t uart_scan_post_uri = {
            .uri = "/api/uart_scan",
            .method = HTTP_POST,
            .handler = uart_scan_post_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &uart_scan_post_uri);

    /* URI handler for USB CDC Host status (GET) - always register, handler checks if USB is configured */
    httpd_uri_t usb_status_get_uri = {
            .uri = "/api/usb_status",
            .method = HTTP_GET,
            .handler = usb_status_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &usb_status_get_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = rest_common_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    return ESP_OK;
    err_start:
    free(rest_context);
    err:
    return ESP_FAIL;
}
