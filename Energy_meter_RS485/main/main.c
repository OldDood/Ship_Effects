#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define TXD_PIN (GPIO_NUM_17) // Connect to MAX3485 DI
#define RXD_PIN (GPIO_NUM_16) // Connect to MAX3485 RO
#define RTS_PIN (GPIO_NUM_4)  // Connect to MAX3485 DE/RE (Driver/Receiver enable)
#define UART_PORT_NUM (UART_NUM_1)
#define BAUD_RATE (9600)

#define GM1000_SLAVE_ID (0x03)  // Default GoodWe meter ID
#define REG_START_ADDR (0x006A) // 106 Decimal (Active Power L3)
#define REG_COUNT (0x0001)      // Only need 1 register

static const char *TAG = "GM1000_MASTER";

// Standard Modbus CRC16 calculation function
uint16_t modbus_crc16(const uint8_t *buffer, uint16_t buffer_length)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t pos = 0; pos < buffer_length; pos++)
    {
        crc ^= (uint16_t)buffer[pos];
        for (int i = 8; i != 0; i--)
        {
            if ((crc & 0x0001) != 0)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void init_rs485_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_PORT_NUM, 512, 0, 0, NULL, 0);
    uart_param_config(UART_PORT_NUM, &uart_config);

    // Configure RS485 half-duplex mode via UART driver RTS pin control
    uart_set_mode(UART_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX);

    // Pin configuration
    uart_set_pin(UART_PORT_NUM, TXD_PIN, RXD_PIN, RTS_PIN, UART_PIN_NO_CHANGE);
}

void modbus_master_task(void *pvParameters)
{
    init_rs485_uart();
    ESP_LOGI(TAG, "GoodWe GM1000 Modbus Master Initialised.");

    uint8_t request_frame[8];
    uint8_t response[32];

    while (1)
    {
        uart_flush_input(UART_PORT_NUM);

        // Construct 1-register query frame
        request_frame[0] = GM1000_SLAVE_ID;
        request_frame[1] = 0x03;
        request_frame[2] = (REG_START_ADDR >> 8) & 0xFF;
        request_frame[3] = REG_START_ADDR & 0xFF;
        request_frame[4] = (REG_COUNT >> 8) & 0xFF;
        request_frame[5] = REG_COUNT & 0xFF;
        uint16_t crc = modbus_crc16(request_frame, 6);
        request_frame[6] = crc & 0xFF;
        request_frame[7] = (crc >> 8) & 0xFF;

        uart_write_bytes(UART_PORT_NUM, (const char *)request_frame, sizeof(request_frame));

        // 3 header bytes + 2 data bytes + 2 CRC bytes = 7 bytes expected
        int rx_len = uart_read_bytes(UART_PORT_NUM, response, sizeof(response), pdMS_TO_TICKS(300));

        if (rx_len >= 7)
        {
            // Unpack 16-bit signed integer from data payload bytes (indices 3 and 4)
            int16_t raw_power_watts = (int16_t)((response[3] << 8) | response[4]);

            // Separate into Import / Export status based on the sign
            float power_kw = (float)abs(raw_power_watts) / 1000.0f;

            if (raw_power_watts < 0)
            {
                ESP_LOGI(TAG, "Grid Status: IMPORTING | Power: %.3f kW (%d W)", power_kw, abs(raw_power_watts));
            }
            else if (raw_power_watts > 0)
            {
                ESP_LOGI(TAG, "Grid Status: EXPORTING | Power: %.3f kW (%d W)", power_kw, raw_power_watts);
            }
            else
            {
                ESP_LOGI(TAG, "Grid Status: IDLE      | Power: 0.000 kW");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Modbus Timeout or Error. Bytes received: %d", rx_len);
        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // Precise 10-second cadence
    }
}

void app_main(void)
{
    xTaskCreate(modbus_master_task, "modbus_master_task", 4096, NULL, 5, NULL);
}
