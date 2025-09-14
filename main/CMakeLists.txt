set(SRCS
        main.c
        tcp_server.c
        msp_ltm_serial.c
        http_server.c
        db_esp32_control.c
        db_crc.c
        db_esp_now.c
        db_serial.c
        db_mavlink_msgs.c
        db_parameters.c)

if(CONFIG_BT_ENABLED)
    list(APPEND SRCS db_ble.c)
endif()

if(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
    list(APPEND SRCS db_usb_cdc_host.c)
endif()

set(REQUIRES_LIST )
if(CONFIG_DB_SERIAL_OPTION_USB_CDC_HOST)
    list(APPEND REQUIRES_LIST usb cdc_acm_host)
endif()

idf_component_register(SRCS ${SRCS}
        INCLUDE_DIRS "."
        REQUIRES ${REQUIRES_LIST})


if(CONFIG_WEB_DEPLOY_SF)
    set(WEB_SRC_DIR "${CMAKE_BINARY_DIR}/frontend")
    spiffs_create_partition_image(www ${WEB_SRC_DIR} FLASH_IN_PROJECT DEPENDS frontend)
endif()
