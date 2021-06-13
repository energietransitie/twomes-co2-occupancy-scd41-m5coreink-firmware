#include "../lib/generic_esp_32/generic_esp_32.h"
#include "string.h"
#include "../include/util.h"
#include "../include/i2c.h"
#include "../include/wifi.h"
#include "../include/timer.h"

#include "../include/scd41.h"

#define DEVICE_NAME "Generic-Test"

#define MESSAGE_BUFFER_SIZE         4096
#define MEASUREMENT_TYPE_CO2        "\"CO2concentration\""
#define MEASUREMENT_TYPE_RH         "\"relativeHumidity\""
#define MEASUREMENT_TYPE_ROOMTEMP   "\"roomTemp\""

char temp[64];
char *msg_start = "{\"upload_time\": \"%d\",\"property_measurements\": [";
char *meas_str  = "{\"property_name\": %s,"
                      "\"timestamp\":\"%d\","
                      "\"timestamp_type\": \"end\","
                      "\"interval\": %d,"
                      "\"measurements\": [";
char *msg_end   = "] }";
    
static const char *TAG = "Twomes Heartbeat Test Application ESP32";
char strftime_buf[64]; // FIXME: weird things happen when you remove this one

const char *device_activation_url = TWOMES_TEST_SERVER "/device/activate";
const char *variable_interval_upload_url = TWOMES_TEST_SERVER "/device/measurements/fixed-interval";
char *bearer;
const char *rootCA;

// Function:    wifi_init_espnow()
// Params:      N/A
// Returns:     N/A
// Description: used to initialise Wi-Fi for ESP-NOW
void wifi_init_espnow(void)
{
    initialize_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start());
}

// Function:    initialise_wifi()
// Params:      N/A
// Returns:     N/A
// Description: used to intialize Wi-Fi for HTTPS
void initialize_wifi(){
 
    initialize_nvs();
    initialize();
    /* initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    wifi_prov_mgr_config_t config = initialize_provisioning();

    // make sure to have this here otherwise the device names won't match because
    // of config changes made by the above function call.
    prepare_device();

    // starts provisioning if not provisioned, otherwise skips provisioning.
    // if set to false it will not autoconnect after provisioning.
    // if set to true it will autonnect.
    start_provisioning(config, true);

    // initialize time with timezone UTC; building timezone is stored in central database
    initialize_time("UTC");

    // gets time as epoch time.
    ESP_LOGI(TAG, "Getting time!");
    uint32_t now = time(NULL);
    ESP_LOGI(TAG, "Time is: %d", now);

    // get bearer token
    bearer = get_bearer();
    char *device_name;
    device_name = malloc(DEVICE_NAME_SIZE);
    get_device_service_name(device_name, DEVICE_NAME_SIZE);
    rootCA = get_root_ca();

    if (strlen(bearer) > 1)
    {
        ESP_LOGI(TAG, "Bearer read: %s", bearer);
    }

    else if (strcmp(bearer, "") == 0)
    {
        ESP_LOGI(TAG, "Bearer not found, activating device!");
        activate_device(device_activation_url, device_name, rootCA);
        bearer = get_bearer();
    }

    else if (!bearer)
    {
        ESP_LOGE(TAG, "Something went wrong whilst reading the bearer!");
    }
}

// Function:        append_uint16()
// Params:
//      - (uint32_t *)      buffer [TODO]
//      - (size_t)          [TODO]
//      - (char *)          pointer to message
//      - (const char *)    type of measurement (CO2, temp or RH)
// Returns:         N/A
// Description:     puts measurements at the end of the buffer [TODO]
void append_uint16(uint16_t *b, size_t size, char *msg_ptr, const char *type)
{
    time_t now = time(NULL);

    // measurement type header
    int msgSize = variable_sprintf_size(meas_str, 3, type, now, (SCD41_SAMPLE_INTERVAL * 1000));
    snprintf(temp, msgSize, meas_str, type, now, (SCD41_SAMPLE_INTERVAL * 1000));
    strcat(msg_ptr, temp);

    // append measurements
    for(uint32_t i = 0; i < size-1; i++)
    {
        msgSize = variable_sprintf_size("\"%u\",", 1, b[i]);
        snprintf(temp, msgSize, "\"%u\",", b[i]);
        strcat(msg_ptr, temp);
    }
    
    // add last measurement and end of this object
    msgSize = variable_sprintf_size("\"%u\"] }", 1, b[size-1]);
    snprintf(temp, msgSize, "\"%u\"] }", b[size-1]);
    strcat(msg_ptr, temp);
}

// TODO: REMOVE
void append_floats(float *b, size_t size, char *msg_ptr, const char *type)
{
    time_t now = time(NULL);

    // measurement type header
    int msgSize = variable_sprintf_size(meas_str, 3, type, now, (SCD41_SAMPLE_INTERVAL * 1000));
    snprintf(temp, msgSize, meas_str, type, now, (SCD41_SAMPLE_INTERVAL * 1000));
    strcat(msg_ptr, temp);

    // append measurements
    for(uint32_t i = 0; i < size-1; i++)
    {
        msgSize = variable_sprintf_size("\"%f\",", 1, (double) b[i]);
        snprintf(temp, msgSize, "\"%f\",", (double) b[i]);
        strcat(msg_ptr, temp);
    }
    
    // add last measurement and end of this object
    msgSize = variable_sprintf_size("\"%f\"] }", 1, (double) b[size-1]);
    snprintf(temp, msgSize, "\"%f\"] }", (double) b[size-1]);
    strcat(msg_ptr, temp);
}

// Function:        upload()
// Params:
//      - (uint16_t *)      buffer for CO2 measurements
//      - (float *)         buffer for temperature measurements
//      - (uint8_t *)       buffer for relative humidity measurements
//      - (size_t)          [TODO]
// Returns:         N/A
// Description:     used to upload measurements to the API
void upload(uint16_t *b_co2, uint16_t *b_temp, uint16_t *b_rh, size_t size)
{
    time_t now = time(NULL);
    char *msg = malloc(MESSAGE_BUFFER_SIZE);

    int msgSize = variable_sprintf_size(msg_start, 1, now);
    snprintf(msg, msgSize, msg_start, now);

    append_uint16(b_co2, size, msg, MEASUREMENT_TYPE_CO2); 
    strcat(msg, ",");
    append_uint16(b_rh, size, msg, MEASUREMENT_TYPE_RH);
    strcat(msg, ",");
    append_uint16(b_temp, size, msg, MEASUREMENT_TYPE_ROOMTEMP);

    strcat(msg, "] }");

    ESP_LOGI("test", "data: %s", msg );

    post_https(variable_interval_upload_url, msg, rootCA, bearer, NULL, 0); // msg is freed by this function
    vTaskDelay(500 / portTICK_PERIOD_MS);
}

// Function:        send_HTTPS()
// Params:
//      - (uint16_t *)      co2 buffer
//      - (float *)         temperature buffer
//      - (uint8_t *)       relative humidity buffer
//      - (size_t)          [TODO]
// Returns:         N/A
// Description:     sends measurements to the API
void send_HTTPS(uint16_t *co2, uint16_t *temp, uint16_t *rh, size_t size)
{
    enable_wifi();
    vTaskDelay(2000 / portTICK_PERIOD_MS);  // wait to make sure Wi-Fi is enabled.
    upload(co2, temp, rh, size);              
    vTaskDelay(500 / portTICK_PERIOD_MS);   // wait to make sure uploading is finished.
    disable_wifi();
}