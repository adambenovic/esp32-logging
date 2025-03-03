#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h" 
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "driver/gpio.h"

#define WIFI_SSID           "kancelaria2.4"
#define WIFI_PASS           "123beno4"
#define TELNET_PORT         23
#define GPIO_INTERCOM_PIN   GPIO_NUM_6

static const char *TAG = "ESP32C3";
static int client_sock = -1;
static int server_sock = -1;

// Custom log handler to send logs over Telnet
int custom_log_handler(const char *fmt, va_list args) {
    if (client_sock >= 0) {
        char buffer[256];
        int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
        if (len > 0) {
            send(client_sock, buffer, len, 0);
        }
    }
    return vprintf(fmt, args);
}

// Event handler for Wi-Fi events
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected from AP, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// Telnet server task
void telnet_server_task(void *pvParameters) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char rx_buffer[128];

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TELNET_PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(server_sock);
        vTaskDelete(NULL);
    }

    if (listen(server_sock, 1) != 0) {
        ESP_LOGE(TAG, "Error listening: errno %d", errno);
        close(server_sock);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Telnet server listening on port %d", TELNET_PORT);

    while (1) {
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }

        ESP_LOGI(TAG, "Client connected");

        while (1) {
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                break;
            }
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
        }

        close(client_sock);
        client_sock = -1;
    }
}

// Pulse timing task (GPIO Pin 6)
void gpio_intercom_task(void *pvParameters) {
    gpio_set_direction(GPIO_INTERCOM_PIN, GPIO_MODE_INPUT);
    int last_state = 0;
    int pulse_start_time = 0;
    int pulse_width = 0;
    ESP_LOGI(TAG, "started listening to intercom");
    ESP_LOGI(TAG, "state %d", gpio_get_level(GPIO_INTERCOM_PIN));

    while (1) {
        int current_state = gpio_get_level(GPIO_INTERCOM_PIN);

        if (current_state != last_state) {
            ESP_LOGI(TAG, "state changed %d", current_state);

            // We have a state change, so measure the pulse width
            if (current_state == 0) {
                // Start of pulse
                pulse_start_time = esp_timer_get_time();  // Get current time in microseconds
            } else {
                // End of pulse
                pulse_width = esp_timer_get_time() - pulse_start_time;  // Calculate pulse width
                pulse_width /= 1000;  // Convert to milliseconds
                ESP_LOGI(TAG, "Pulse width: %d ms", pulse_width);
            }
            last_state = current_state;
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay for debouncing
    }
}

void app_main() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize TCP/IP and Wi-Fi
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    // Register event handler
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    esp_wifi_start();

    // Set custom log handler for remote logging
    esp_log_set_vprintf(custom_log_handler);

    // Start Telnet server task
    xTaskCreate(telnet_server_task, "telnet_server_task", 4096, NULL, 5, NULL);

    // Start GPIO intercom data reading task
    xTaskCreate(gpio_intercom_task, "gpio_intercom_task", 2048, NULL, 5, NULL);
}
