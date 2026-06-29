#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define REQ_MAGIC "PSW2"
#define ACK_MAGIC "PSA1"
#define REQ_LEN 12
#define ACK_LEN 10

#define WIFI_CONNECTED_BIT BIT0

#ifndef CONFIG_POWER_SWITCH_CONTROL_GPIO_OPEN_DRAIN
#define CONFIG_POWER_SWITCH_CONTROL_GPIO_OPEN_DRAIN 0
#endif

#ifndef CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL
#define CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL 1
#endif

typedef enum {
    CMD_STATUS = 1,
    CMD_SET_LINES = 2,
    CMD_PULSE_LINES = 3,
} command_t;

typedef enum {
    ACK_STATUS_OK = 0,
    ACK_STATUS_BAD_MAGIC = 1,
    ACK_STATUS_BAD_LENGTH = 2,
    ACK_STATUS_BAD_COMMAND = 3,
    ACK_STATUS_INTERNAL_ERROR = 4,
    ACK_STATUS_BAD_FLAGS = 5,
    ACK_STATUS_BAD_MASK = 6,
} ack_status_t;

enum {
    STATE_RESET_ASSERTED = BIT0,
    STATE_BOOT_ASSERTED = BIT1,
    STATE_ALL_LINES = STATE_RESET_ASSERTED | STATE_BOOT_ASSERTED,
};

static const char *TAG = "power_switch";
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_gpio_mutex;
static led_strip_handle_t s_status_led;
static esp_ip4_addr_t s_sta_ip;
static bool s_reset_asserted;
static bool s_boot_asserted;

static uint16_t read_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static void write_u16_be(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value >> 8);
    buf[1] = (uint8_t)(value & 0xff);
}

static gpio_num_t reset_gpio(void)
{
    return (gpio_num_t)CONFIG_POWER_SWITCH_RESET_GPIO;
}

static gpio_num_t boot_gpio(void)
{
    return (gpio_num_t)CONFIG_POWER_SWITCH_BOOT_GPIO;
}

static void status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_status_led == NULL) {
        return;
    }

    esp_err_t err;
    if (red == 0 && green == 0 && blue == 0) {
        err = led_strip_clear(s_status_led);
    } else {
        err = led_strip_set_pixel(s_status_led, 0, red, green, blue);
        if (err == ESP_OK) {
            err = led_strip_refresh(s_status_led);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update status LED: %s", esp_err_to_name(err));
    }
}

static void status_led_connecting(void)
{
    status_led_set_rgb(16, 8, 0);
}

static void status_led_ready(void)
{
    status_led_set_rgb(0, 16, 0);
}

static void status_led_activity(void)
{
    status_led_set_rgb(0, 0, 16);
}

static void status_led_reset_active(void)
{
    status_led_set_rgb(24, 0, 0);
}

static void status_led_maskrom_active(void)
{
    status_led_set_rgb(16, 0, 16);
}

static void status_led_for_state(uint8_t state)
{
    if ((state & STATE_ALL_LINES) == STATE_ALL_LINES) {
        status_led_maskrom_active();
    } else if (state & STATE_RESET_ASSERTED) {
        status_led_reset_active();
    } else if (state & STATE_BOOT_ASSERTED) {
        status_led_maskrom_active();
    } else {
        status_led_ready();
    }
}

static esp_err_t init_status_led(void)
{
#if CONFIG_POWER_SWITCH_STATUS_LED_ENABLE
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_POWER_SWITCH_STATUS_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d RGB status LED disabled: %s",
                 CONFIG_POWER_SWITCH_STATUS_LED_GPIO, esp_err_to_name(err));
        s_status_led = NULL;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Configured RGB status LED on GPIO%d", CONFIG_POWER_SWITCH_STATUS_LED_GPIO);
    status_led_connecting();
#endif
    return ESP_OK;
}

static uint16_t clamp_duration_ms(uint16_t requested)
{
    if (requested < CONFIG_POWER_SWITCH_MIN_PULSE_MS) {
        return CONFIG_POWER_SWITCH_MIN_PULSE_MS;
    }
    if (requested > CONFIG_POWER_SWITCH_MAX_PULSE_MS) {
        return CONFIG_POWER_SWITCH_MAX_PULSE_MS;
    }
    return requested;
}

static void set_reset_asserted(bool asserted)
{
    int level = asserted ? CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL
                         : !CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL;
    ESP_ERROR_CHECK(gpio_set_level(reset_gpio(), level));
    s_reset_asserted = asserted;
}

static void set_boot_asserted(bool asserted)
{
    int level = asserted ? CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL
                         : !CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL;
    ESP_ERROR_CHECK(gpio_set_level(boot_gpio(), level));
    s_boot_asserted = asserted;
}

static uint8_t state_bits_locked(void)
{
    uint8_t state = 0;

    if (s_reset_asserted) {
        state |= STATE_RESET_ASSERTED;
    }
    if (s_boot_asserted) {
        state |= STATE_BOOT_ASSERTED;
    }
    return state;
}

static void set_lines_locked(uint8_t mask, uint8_t asserted_bits)
{
    if (mask & STATE_BOOT_ASSERTED) {
        set_boot_asserted((asserted_bits & STATE_BOOT_ASSERTED) != 0);
    }
    if (mask & STATE_RESET_ASSERTED) {
        set_reset_asserted((asserted_bits & STATE_RESET_ASSERTED) != 0);
    }
}

static esp_err_t init_control_gpios(void)
{
    gpio_num_t rst = reset_gpio();
    gpio_num_t boot = boot_gpio();
    int inactive_level = !CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL;

    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(rst), ESP_ERR_INVALID_ARG, TAG,
                        "GPIO%d cannot be used as RESETn output", rst);
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(boot), ESP_ERR_INVALID_ARG, TAG,
                        "GPIO%d cannot be used as SARADC0_BOOT output", boot);
    ESP_RETURN_ON_FALSE(rst != boot, ESP_ERR_INVALID_ARG, TAG,
                        "RESETn and SARADC0_BOOT must use different GPIOs");

    ESP_ERROR_CHECK(gpio_set_level(rst, inactive_level));
    ESP_ERROR_CHECK(gpio_set_level(boot, inactive_level));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << rst) | (1ULL << boot),
        .mode = CONFIG_POWER_SWITCH_CONTROL_GPIO_OPEN_DRAIN ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure GPIOs");

    ESP_ERROR_CHECK(gpio_set_level(rst, inactive_level));
    ESP_ERROR_CHECK(gpio_set_level(boot, inactive_level));
    s_reset_asserted = false;
    s_boot_asserted = false;

    ESP_LOGI(TAG, "Configured K6 RESETn on GPIO%d and K5 SARADC0_BOOT on GPIO%d", rst, boot);
    ESP_LOGI(TAG, "Control GPIO mode: %s, assert GPIO level: %d",
             CONFIG_POWER_SWITCH_CONTROL_GPIO_OPEN_DRAIN ? "open-drain" : "push-pull",
             CONFIG_POWER_SWITCH_CONTROL_GPIO_ASSERT_LEVEL);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        status_led_connecting();
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_ip = event->ip_info.ip;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR ", UDP port %d",
                 IP2STR(&s_sta_ip), CONFIG_POWER_SWITCH_UDP_PORT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    configASSERT(s_wifi_event_group != NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_POWER_SWITCH_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_POWER_SWITCH_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID %s", CONFIG_POWER_SWITCH_WIFI_SSID);
    status_led_connecting();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

static ack_status_t execute_command_locked(uint8_t command, uint16_t arg0,
                                           uint16_t arg1, uint16_t *detail)
{
    *detail = 0;

    switch ((command_t)command) {
    case CMD_STATUS:
        *detail = state_bits_locked();
        return ACK_STATUS_OK;

    case CMD_SET_LINES: {
        if ((arg0 & ~STATE_ALL_LINES) != 0 || (arg1 & ~STATE_ALL_LINES) != 0) {
            *detail = ((arg0 & 0xff) << 8) | (arg1 & 0xff);
            return ACK_STATUS_BAD_MASK;
        }

        uint8_t mask = (uint8_t)arg0;
        uint8_t asserted_bits = (uint8_t)arg1;
        ESP_LOGI(TAG, "Set lines: mask=0x%02x asserted=0x%02x", mask, asserted_bits);
        set_lines_locked(mask, asserted_bits);
        *detail = state_bits_locked();
        status_led_for_state(*detail);
        return ACK_STATUS_OK;
    }

    case CMD_PULSE_LINES: {
        if ((arg0 & ~STATE_ALL_LINES) != 0 || arg0 == 0) {
            *detail = arg0;
            return ACK_STATUS_BAD_MASK;
        }

        uint8_t mask = (uint8_t)arg0;
        bool prev_reset = s_reset_asserted;
        bool prev_boot = s_boot_asserted;
        uint16_t pulse_ms = clamp_duration_ms(arg1);
        ESP_LOGI(TAG, "Pulse lines: mask=0x%02x duration=%u ms", mask, pulse_ms);

        set_lines_locked(mask, mask);
        status_led_for_state(state_bits_locked());
        vTaskDelay(pdMS_TO_TICKS(pulse_ms));

        if (mask & STATE_BOOT_ASSERTED) {
            set_boot_asserted(prev_boot);
        }
        if (mask & STATE_RESET_ASSERTED) {
            set_reset_asserted(prev_reset);
        }

        *detail = pulse_ms;
        status_led_for_state(state_bits_locked());
        return ACK_STATUS_OK;
    }

    default:
        *detail = command;
        return ACK_STATUS_BAD_COMMAND;
    }
}

static int send_ack(int sock, const struct sockaddr_in *dest_addr, socklen_t dest_len,
                    uint16_t sequence, ack_status_t status, uint8_t state_bits,
                    uint16_t detail)
{
    uint8_t ack[ACK_LEN] = {0};

    memcpy(&ack[0], ACK_MAGIC, 4);
    write_u16_be(&ack[4], sequence);
    ack[6] = (uint8_t)status;
    ack[7] = state_bits;
    write_u16_be(&ack[8], detail);

    return sendto(sock, ack, sizeof(ack), 0, (const struct sockaddr *)dest_addr, dest_len);
}

static void handle_packet(int sock, const uint8_t *rx, int len,
                          const struct sockaddr_in *source_addr, socklen_t source_len)
{
    uint16_t sequence = len >= 6 ? read_u16_be(&rx[4]) : 0;
    ack_status_t status = ACK_STATUS_OK;
    uint16_t detail = 0;
    uint8_t state = 0;

    if (len != REQ_LEN) {
        status = ACK_STATUS_BAD_LENGTH;
        detail = (uint16_t)len;
    } else if (memcmp(rx, REQ_MAGIC, 4) != 0) {
        status = ACK_STATUS_BAD_MAGIC;
    } else if (rx[7] != 0) {
        status = ACK_STATUS_BAD_FLAGS;
        detail = rx[7];
    } else {
        uint8_t command = rx[6];
        uint16_t arg0_ms = read_u16_be(&rx[8]);
        uint16_t arg1_ms = read_u16_be(&rx[10]);

        status_led_activity();
        vTaskDelay(pdMS_TO_TICKS(25));
        xSemaphoreTake(s_gpio_mutex, portMAX_DELAY);
        status = execute_command_locked(command, arg0_ms, arg1_ms, &detail);
        state = state_bits_locked();
        xSemaphoreGive(s_gpio_mutex);
    }

    if (status != ACK_STATUS_OK && state == 0) {
        xSemaphoreTake(s_gpio_mutex, portMAX_DELAY);
        state = state_bits_locked();
        xSemaphoreGive(s_gpio_mutex);
    }

    int err = send_ack(sock, source_addr, source_len, sequence, status, state, detail);
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to send ack: errno %d", errno);
    }

    status_led_for_state(state);
}

static void udp_server_task(void *pv_parameters)
{
    (void)pv_parameters;
    uint8_t rx_buffer[64];
    char addr_str[16];

    while (true) {
        struct sockaddr_in listen_addr = {
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_family = AF_INET,
            .sin_port = htons(CONFIG_POWER_SWITCH_UDP_PORT),
        };

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        int err = bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Unable to bind UDP socket: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "UDP server listening on 0.0.0.0:%d", CONFIG_POWER_SWITCH_UDP_PORT);
        ESP_LOGI(TAG, "UDP server ready at " IPSTR ":%d",
                 IP2STR(&s_sta_ip), CONFIG_POWER_SWITCH_UDP_PORT);
        status_led_ready();

        while (true) {
            struct sockaddr_in source_addr = {0};
            socklen_t source_len = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                               (struct sockaddr *)&source_addr, &source_len);
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }

            inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(TAG, "Received %d bytes from %s:%u", len, addr_str,
                     ntohs(source_addr.sin_port));
            handle_packet(sock, rx_buffer, len, &source_addr, source_len);
        }

        shutdown(sock, 0);
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_gpio_mutex = xSemaphoreCreateMutex();
    configASSERT(s_gpio_mutex != NULL);
    ESP_ERROR_CHECK(init_status_led());
    ESP_ERROR_CHECK(init_control_gpios());

    wifi_init_sta();
    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
}
