#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "driver/adc.h"
#include "pm_wifi.h"
#include "pm_log.h"
#include <math.h>
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "pm_influxdb.h"
#include "pm_config.h"
#include "esp_sleep.h"
#include "esp_mac.h"

#include "mqtt_client.h"

#define TAG "plantmonitoring"
#define TRUE (1==1)
#define FALSE (!TRUE)

#define PM_ADC_BIT_WIDTH ADC_WIDTH_BIT_DEFAULT
#define uS_TO_S_FACTOR (uint64_t)1000000
#define PM_MEASURE_EVERY_MINS (uint64_t)60
#define CONFIG_BROKER_URL "mqtt://192.168.1.15"

mac_to_station **stations = NULL;
mac_to_station *config = NULL;
int num_stations = 0;

int calculate_vdc_mv(int reading)
{
    return (reading * 2500) / (pow(2, PM_ADC_BIT_WIDTH) - 1);
}

void sync_time(void)
{
    // Set timezone
    ESP_LOGI(TAG, "Timezone is set to: %s", CONFIG_ESP_NTP_TZ);
    setenv("TZ", CONFIG_ESP_NTP_TZ, 1);
    tzset();
    
    // Synchronize time with NTP
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    ESP_LOGI(TAG, "Getting time via NTP from: %s", CONFIG_ESP_NTP_SERVER);
    esp_sntp_setservername(0, CONFIG_ESP_NTP_SERVER);
    
    const int total_max_retries = 3;
    int total_retries = 0;
    time_t now = 0;
    struct tm timeinfo = { 0 };
    do {
        esp_sntp_init();

        int retry = 0;

        // NTP request is sent every 15 seconds, so wait 25 seconds for sync.
        const int retry_count = 10;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
            ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
        if (retry == retry_count) {
            total_retries += 1;
            esp_sntp_stop();
        } else {
            break;
        }
    } while (total_retries < total_max_retries);
    assert(total_retries < total_max_retries);

    time(&now);
    localtime_r(&now, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
}


void load_config(void) {
    uint8_t mac[8] = { 0 };
    char macstr[24] = { '\0' };
    esp_err_t err;

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    for (int i = 0; i < 6; i++) {
        sprintf((char *)&macstr[i*3], "%02x:", mac[i]);
    }
    macstr[17] = '\0';
    ESP_LOGI(TAG, "WiFi MAC address: %s", macstr);

    stations = get_config_from_url(CONFIG_ESP_CONFIG_URL, &num_stations, &err);
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Configuration has %d stations:", num_stations);
    for (int i = 0; i < num_stations; i++) {
        ESP_LOGI(TAG, " %03d MAC: %s Station: %s", i+1, stations[i]->mac, stations[i]->station);
        if (strcasecmp(stations[i]->mac, macstr) == 0) {
            config = stations[i];
        }
    }
    assert(config != NULL);
    ESP_LOGI(TAG, "My station name: %s", config->station);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

esp_mqtt_client_handle_t mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    return client;
}

void app_main(void)
{
    printf("Plant monitoring system\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }
    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    // Connect to WiFi
    wifi_init();

    esp_mqtt_client_handle_t mqtt_client;
    mqtt_client = mqtt_init();


    // Synchronize time
    sync_time();
    
    // Init logging
    init_logging();

    // Load config
    load_config();

    // Configure ADC channels
    adc1_config_width((adc_bits_width_t)ADC_WIDTH_BIT_DEFAULT);
    adc1_config_channel_atten(ADC1_CHANNEL_5, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);
    
    int adc1_reading[3][5];
    int adc1_values[3] = {0, 0, 0};
    const char TAG_CH[][10] = {"plant1", "plant2", "plant3"};

    while (TRUE) {
        for (int i = 0; i < 5; i++) {
            adc1_reading[0][i] = adc1_get_raw(ADC1_CHANNEL_5);
            adc1_reading[1][i] = adc1_get_raw(ADC1_CHANNEL_3);
            adc1_reading[2][i] = adc1_get_raw(ADC1_CHANNEL_4);
        }
        for (int i = 0; i < 3; i++) {
            adc1_values[i] = 0;
            for (int o = 0; o < 5; o++) {
                adc1_values[i] += adc1_reading[i][o];
            }
            adc1_values[i] = adc1_values[i]/5;
            ESP_LOGI(TAG_CH[i], "avg=%d", adc1_values[i]);
            if (mqtt_client) {
                char* topic;
                topic = "plants/monitor";
                ESP_LOGI(TAG_CH[i], "publishing %d to topic %s", adc1_values[i], topic);
                int published_bytes = esp_mqtt_client_publish(mqtt_client, topic, sprintf("%d", adc1_values[i]), 0, 0, 0); // sizeof(adc1_values[i]), 0, 0);
                //int published_bytes = mqtt_client->publish(topic, sprintf("%d", adc1_values[i]), 0, 0, 0);
                ESP_LOGI(TAG_CH[i], "published_bytes=%d", published_bytes);
            } else {
                ESP_LOGI(TAG_CH[i], "No mqtt client, cannot publish data");
            }
        }
        for (int o = 0; o < 3; o++) {
            char value[14] = { '\0' };
                
            snprintf(value, 8, "%u", 4095 - calculate_vdc_mv(adc1_values[o]));


            // write_influxdb(config->station, TAG_CH[o], value);
        }
        printf("Entering deep sleep...\n");
        esp_deep_sleep(PM_MEASURE_EVERY_MINS * 60 * uS_TO_S_FACTOR);
        printf("Woke up from deep sleep. Connecting WiFi and syncing time.\n");

        wifi_init();
        sync_time();
        mqtt_client = mqtt_init();
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}