#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"

#include "sys/clock.h"

#include "sys/log.h"
#define LOG_MODULE "BATT"
#define LOG_LEVEL LOG_LEVEL_APP

char* str(float value, char* output);

// Battery parameters
#define BATTERY_CAPACITY 10000 // in Wh
#define DC_AC_COEFF 10.0

static float battery_level = 0.0; // in Wh
float charge_rate = 0.0; // in W
static unsigned long lastUpdateTime = 0;

static void update_battery_level()
{
    if (lastUpdateTime == 0.0)
        lastUpdateTime = clock_seconds();
    unsigned long currentTime = clock_seconds();
    unsigned long elapsedTime = currentTime - lastUpdateTime;

    // Update battery level based on charge rate and elapsed time
    battery_level += DC_AC_COEFF * charge_rate * (elapsedTime / 3600.0); // convert seconds to hours

    // Ensure battery level stays within bounds
    if (battery_level < 0) {
        battery_level = 0;
    } else if (battery_level > BATTERY_CAPACITY) {
        battery_level = BATTERY_CAPACITY;
    }

    lastUpdateTime = currentTime;

    if (charge_rate != 0.0)
    {
        char level[16];
        LOG_INFO("Battery level updated: %s Wh\n", str(battery_level, level));
    }
}

// callable from outside
void updateChargeRate(float rate)
{
    update_battery_level();
    charge_rate = rate;

    char charge_rate_str[16];
    LOG_INFO("Charge rate set to: %s W\n", str(charge_rate, charge_rate_str));
}

void battery_json_string(char* buffer)
{
    char buf[16];
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE, 
            "{\"n\":\"battery\",\"v\":\"%s\"}", 
            str(battery_level, buf));
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_battery,
                "title=\"Battery data\";rt=\"battery\";obs",
                res_get_handler,
                NULL,
                NULL,
                NULL,
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    update_battery_level();
    battery_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("Battery resource GET handler called\n");

    char buf[16];
    LOG_DBG("Battery level: %s Wh\n", str(battery_level, buf));
}

static void res_event_handler(void)
{    
    update_battery_level();
    coap_notify_observers(&res_battery);

    LOG_DBG("Battery resource event handler called\n");
}
