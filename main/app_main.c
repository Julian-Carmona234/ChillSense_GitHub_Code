/*
 * ChillSense data flow
 *
 * Flow meter
 *    |
 *    | dry contact pulse
 *    v
 * GPIO4 interrupt
 *    |
 *    v
 * Pulse processing task
 *    |
 *    | pulse count / GPM / total gallons
 *    v
 * Telemetry task
 *    |
 *    | JSON every 5 seconds
 *    v
 * MQTT over WiFi/TLS
 *    |
 *    v
 * MQTT broker
 *    |
 *    v
 * MQTTX / future cloud dashboard
 */

 
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "protocol_examples_common.h"

#include "esp_crt_bundle.h"
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

//---------------------------------------------------------
//                    Configuration
//---------------------------------------------------------
#define FLOW_GPIO GPIO_NUM_4

//meter is 1 gallon per pulse.
#define GALLONS_PER_PULSE 1.0f

//ignore additional switch closures that happen too soon afterthe previous pulse
//100 ms is fine for this slow mechanical/dry-contact meter
#define DEBOUNCE_TIME_US 100000LL

//if no pulse occurs for this long, report flow as zero
//meter nominal minimum is around 1 GPM, which is one pulse every 60 seconds with a 1 gallon per pulse output
#define FLOW_TIMEOUT_US 90000000LL

#define TELEMETRY_PERIOD_MS 5000

static const char *TAG = "chillsense";


//---------------------------------------------------------
//                      MQTT state
//--------------------------------------------------------- 
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;


//---------------------------------------------------------
//                  Flowmeter State
//--------------------------------------------------------- 
static TaskHandle_t pulse_task_handle = NULL;
static SemaphoreHandle_t meter_mutex = NULL;

static uint32_t total_pulses = 0;
static float flow_gpm = 0.0f;

static int64_t last_valid_pulse_us = 0;
static int64_t previous_valid_pulse_us = 0;

//---------------------------------------------------------
//           MQTT Certificate Configuration
//--------------------------------------------------------- 
#if CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDDEN
static const char cert_override_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDE "\n"
    "-----END CERTIFICATE-----";
#endif

#if CONFIG_EXAMPLE_CERT_VALIDATE_MOSQUITTO_CA
extern const uint8_t mosquitto_org_crt_start[]
    asm("_binary_mosquitto_org_crt_start");
extern const uint8_t mosquitto_org_crt_end[]
    asm("_binary_mosquitto_org_crt_end");
#endif


//---------------------------------------------------------
//                      GPIO Interrupt
//---------------------------------------------------------
//Called automatically whenever the GPIO4 detects a falling edge
//
//In the circuit:
//HIGH = meter contact open
//LOW  = meter contact closed
//
//So HIGH -> LOW means one meter pulse occurred
//
//The ISR does NOT process the pulse directly
//It only wakes the pulse processing task below
static void flow_gpio_isr_handler(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (pulse_task_handle != NULL) 
    {
        //notify the normal FreeRTOS task that a pulse occurred
        vTaskNotifyGiveFromISR(pulse_task_handle, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken == pdTRUE) 
    {
        //immediately allow the awakened task to run if needed
        portYIELD_FROM_ISR();
    }
}

//---------------------------------------------------------
//              Flow pulse processing task
//--------------------------------------------------------- 
//This task sleeps until the GPIO interrupt reports a pulse
//Once awake:
//1. rejects switch bounce
//2. counts the pulse
//3. calculates total gallons
//4. calculates GPM from the time between pulses
static void pulse_task(void *pvParameters)
{
    int64_t last_interrupt_us = 0;

    while (1) 
    {
        //wait here until the GPIO interrupt wakes
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        //ESP timer returns elapsed time in microseconds
        int64_t now_us = esp_timer_get_time();

        //ignore duplicate electrical transitions that occur within 100 ms of the previous accepted pulse
        if ((now_us - last_interrupt_us) < DEBOUNCE_TIME_US) 
        {
            continue;
        }

        last_interrupt_us = now_us;

        //lock shared flow meter variables so the MQTT task cannot read them while they are being modified
        xSemaphoreTake(meter_mutex, portMAX_DELAY);

        total_pulses++;

        previous_valid_pulse_us = last_valid_pulse_us;
        last_valid_pulse_us = now_us;

        // one pulse alone only tells us volume.
        // two pulses are required to calculate flow rate because GPM depends on time between pulses
        if (previous_valid_pulse_us > 0) 
        {
            int64_t pulse_interval_us =
                last_valid_pulse_us - previous_valid_pulse_us;

            if (pulse_interval_us > 0) 
            {   
                // flow calculation:
                // 1 pulse = 1 gallon
                //
                //general flow formula:
                //GPM = gallons per pulse × 60 seconds/minute / seconds between pulses
                //
                //since our timing is in microseconds, multiply by 60,000,000 instead.
                flow_gpm = (GALLONS_PER_PULSE * 60000000.0f) / (float)pulse_interval_us;
            }
        }

        float total_gallons = total_pulses * GALLONS_PER_PULSE;

        ESP_LOGI(TAG, "Flow pulse detected: pulses=%" PRIu32 ", total=%.2f gal, flow=%.2f GPM", total_pulses, total_gallons, flow_gpm);
        //release the shared data for the MQTT task
        xSemaphoreGive(meter_mutex);
    }
}


//---------------------------------------------------------
//flow GPIO initialization
//--------------------------------------------------------- 
static void flow_meter_init(void)
{
    gpio_config_t flow_gpio_config = 
    {
        .pin_bit_mask = (1ULL << FLOW_GPIO),

        //input only.
        .mode = GPIO_MODE_INPUT,

        //enable internal pull-up as an additional safeguard. 
        //should still use the external 10k pull-up resistor from GPIO4 to 3.3 V.
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        //meter contact closing pulls GPIO4 LOW.
        .intr_type = GPIO_INTR_NEGEDGE
    };

    ESP_ERROR_CHECK(gpio_config(&flow_gpio_config));

    //install GPIO interrupt service.
    esp_err_t isr_result = gpio_install_isr_service(0);

    //if "ESP_ERR_INVALID_STATE" appears it means the ISR service was already installed elsewhere.
    if (isr_result != ESP_OK && isr_result != ESP_ERR_INVALID_STATE) 
    {
        ESP_ERROR_CHECK(isr_result);
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(FLOW_GPIO, flow_gpio_isr_handler, NULL));

    ESP_LOGI(TAG, "Flow meter input initialized on GPIO%d", FLOW_GPIO);
}

//---------------------------------------------------------
//          Periodic MQTT telemetry task
//--------------------------------------------------------- 
// Runs every 5 seconds
// This task does:
// 1. reads the latest flow-meter values
// 2. builds a JSON message
// 3. publishes that message to the MQTT broker
static void telemetry_task(void *pvParameters)
{
    while (1) 
    {
        if (mqtt_connected && mqtt_client != NULL) 
        {
            uint32_t pulses_copy;
            float flow_copy;
            float gallons_copy;
            int64_t last_pulse_copy;

            //copy shared meter values while the mutex is locked.
            //this prevents the pulse task from chaning them halfway through the copy.
            xSemaphoreTake(meter_mutex, portMAX_DELAY);

            pulses_copy = total_pulses;
            flow_copy = flow_gpm;
            gallons_copy =
                total_pulses * GALLONS_PER_PULSE;
            last_pulse_copy = last_valid_pulse_us;

            xSemaphoreGive(meter_mutex);

            //if no pulse has been detected for 90 seconds assume the flow the has stopped
            if (last_pulse_copy > 0) 
            {
                int64_t now_us = esp_timer_get_time();

                if ((now_us - last_pulse_copy) > FLOW_TIMEOUT_US) 
                {
                    flow_copy = 0.0f;
                }
            }

            //temperature values are placeholders for now
            float supply_temp_f = 44.0f;
            float return_temp_f = 55.0f;

            char payload[256];

            //format the sensor data as JSON
            snprintf(payload, sizeof(payload),

                "{"
                "\"device_id\":\"chillsense_test_01\","
                "\"flow_gpm\":%.2f,"
                "\"total_gallons\":%.2f,"
                "\"pulse_count\":%" PRIu32 ","
                "\"supply_temp_f\":%.1f,"
                "\"return_temp_f\":%.1f"
                "}",

                flow_copy, gallons_copy, pulses_copy, supply_temp_f, return_temp_f);

            //publish the JSON message to the MQTT broker
            //Qos 1 means deliver this message at least once
            int msg_id = esp_mqtt_client_publish(mqtt_client, "chillsense/test/telemetry", payload, 0, 1, 0);

            ESP_LOGI(TAG, "Telemetry sent: %s | msg_id=%d", payload, msg_id);
        }
        //wait 5 seconds before sending the next telemetry message
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

//---------------------------------------------------------
//MQTT event handler
//--------------------------------------------------------- 
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) 
    {
    case MQTT_EVENT_CONNECTED:

        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:

        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_connected = false;
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

        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) 
        {

            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);

            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);

            ESP_LOGI(TAG, "Last captured errno: %d (%s)", event->error_handle->esp_transport_sock_errno, strerror(event->error_handle->esp_transport_sock_errno));

        } 

            else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) 
            {
                ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
            } 

                else 
                {
                    ESP_LOGW(TAG, "Unknown MQTT error type: 0x%x",event->error_handle->error_type);
                }

        break;

    default:
        ESP_LOGI(TAG, "Other MQTT event id: %d", event->event_id);
        break;
    }
}

//---------------------------------------------------------
//MQTT initialization (came with template)
//--------------------------------------------------------- 
// creates the MQTT client and connects it to the broker
//
// MQTT acts as the communication layer between:
// ESP32 -> MQTT broker -> MQTTX / cloud application
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
    .broker = { 
        //broker address is configured through menuconfig ">ESP-IDF: SDK Configuration Editor (Menuconfig)"
        .address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
#if CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDDEN
            .verification.certificate = cert_override_pem,

#elif CONFIG_EXAMPLE_CERT_VALIDATE_MOSQUITTO_CA
            .verification.certificate = (const char *)mosquitto_org_crt_start,
#else
            //use ESP-IDF's built in trusted CA bundle to validate the broker's transport layer security (TLS) certificate.
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
#endif
        },
            //send MQTT keepalive ping every 30 seconds to maintain the connection
            .session = {.keepalive = 30,}, 
            //if the connections drops retry after 5 seconds
            .network = {.reconnect_timeout_ms = 5000,},
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",esp_get_free_heap_size());

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    //register our event handler function so esp-idf can tell when MQTT connects, disconnects, publishes, or errors.
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler,NULL);

    esp_mqtt_client_start(mqtt_client);

    //start the seperate task that publishes telemetry every 5 seconds
    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}

//---------------------------------------------------------
//                          Main
//---------------------------------------------------------
// Program startup sequence:
// 1. create synchronization tools
// 2. start pulse processing task
// 3. configure GPIO4 for flowmeter pulses
// 4. initialize WiFi/network stack
// 5. connect to WiFi
// 6. start MQTT client and telemetry task
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    //mutex protects shared flow-meter data from being accessed by two tasks at exactly the same time.
    meter_mutex = xSemaphoreCreateMutex();

    if (meter_mutex == NULL) 
    {
        ESP_LOGE(TAG, "Failed to create meter mutex");
        return;
    }

    //strta the pulse processing task before enabling GPIO interupts so that the task is ready to handle pulses immediately
    xTaskCreate(pulse_task, "pulse_task", 3072, NULL, 10, &pulse_task_handle);

    //configure the GPIO4 for dry contact pulse detection
    flow_meter_init();

    //initialize esp-idf networking services
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //connect using the wifi SSID/Password through menuconfig ">ESP-IDF: SDK Configuration Editor (Menuconfig)"
    ESP_ERROR_CHECK(example_connect());

    //start secure MQTT communication
    mqtt_app_start();


//while (1) 
//{
//    ESP_LOGI(TAG, "GPIO4 level = %d", gpio_get_level(FLOW_GPIO));
//    vTaskDelay(pdMS_TO_TICKS(1000));
//}
}