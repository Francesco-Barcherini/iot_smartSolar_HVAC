#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_APP

#define MAX_POWER 1500.0 // Maximum flow of power in W

// Relay states. Destination of Solar Panel energy, souce of home energy.
enum relay_sp_t { RELAY_SP_HOME, RELAY_SP_BATTERY, RELAY_SP_GRID };
enum relay_home_t { RELAY_HOME_SP, RELAY_HOME_BATTERY, RELAY_HOME_GRID };

static relay_sp_t relay_sp = RELAY_SP_HOME; // Relay state for solar panel
static relay_home_t relay_home = RELAY_HOME_SP; // Relay state for home
static float power_sp = 0.0; // Power from solar panel
static float power_home = 0.0; // Power for home

static void update_relay(bool defected = false)
{
    float expected_power = solar_power_prediction();
    
    float step;
    
    if (defected)
        step = - 1.5 * MAX_OFFSET_PREDICTION;
    else
    {
        step = (float) random_rand() / (float) RANDOM_RAND_MAX * 2.0;
        step -= 1.0; // range [-1.0, 1.0]
        step *=  MAX_OFFSET_PREDICTION;
    }

    gen_power = (gen_power + step < 0.0) ? 0.0 : 
                (gen_power + step > MAX_POWER) ? MAX_POWER :
                gen_power + step;

    LOG_INFO("Generated power updated: %.2f W (defected: %d)\n", gen_power, defected);
}

void relay_json_string(char* buffer)
{
    // json of relays state and power consumption
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE,
            "{\"n\":\"relay\",\"r_sp\":%d,\"r_h\":%d,\"p_sp\":%.2f,\"p_h\":%.2f}",
            relay_sp, relay_home, power_sp, power_home);
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_relay,
                "title=\"relay state\";rt=\"control\";obs",
                res_get_handler,
                res_post_put_handler,
                res_post_put_handler,
                NULL,                
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    relay_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("relay resource GET handler called\n");
}

static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    const char *req_relay_sp = NULL, *req_relay_home = NULL;
    const char *req_power_sp = NULL, *req_power_home = NULL;

    relay_sp_t new_relay_sp = relay_sp;
    relay_home_t new_relay_home = relay_home;
    float new_power_sp = power_sp;
    float new_power_home = power_home;

    coap_get_post_variable(request, "relay_sp", &req_relay_sp);
    coap_get_post_variable(request, "relay_home", &req_relay_home);
    coap_get_post_variable(request, "power_sp", &req_power_sp);
    coap_get_post_variable(request, "power_home", &req_power_home);

    if (req_relay_sp != NULL) {
        new_relay_sp = (relay_sp_t) atoi(req_relay_sp);
        LOG_DBG("New relay_sp: %d\n", new_relay_sp);
    }

    if (req_relay_home != NULL) {
        new_relay_home = (relay_home_t) atoi(req_relay_home);
        LOG_DBG("New relay_home: %d\n", new_relay_home);
    }

    if (req_power_sp != NULL) {
        new_power_sp = atof(req_power_sp);
        LOG_DBG("New power_sp: %.2f\n", new_power_sp);
    }

    if (req_power_home != NULL) {
        new_power_home = atof(req_power_home);
        LOG_DBG("New power_home: %.2f\n", new_power_home);
    }

    // Check for valid relay states and power values
    unsigned int result = BAD_REQUEST_4_00;

    if (new_power_sp < 0.0 || new_power_sp > MAX_POWER ||
        new_power_home < 0.0 || new_power_home > MAX_POWER)
    {
        LOG_ERR("Invalid power values: power_sp=%.2f, power_home=%.2f\n", new_power_sp, new_power_home);
        coap_set_status_code(response, BAD_REQUEST_4_00);
        return;
    }

    if (new_relay_sp == RELAY_SP_HOME)
    {
        if (new_relay_home != RELAY_HOME_SP)
        {
            LOG_ERR("Invalid relay state: new_relay_sp=%d, new_relay_home=%d\n", 
                     new_relay_sp, new_relay_home);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
    }
    else 
    {
        if (new_relay_home == RELAY_HOME_SP
            || new_relay_home == new_relay_sp)
        {
            LOG_ERR("Invalid relay state: new_relay_sp=%d, new_relay_home=%d\n", 
                     new_relay_sp, new_relay_home);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
    }

    // Update relay states and power values
    relay_sp = new_relay_sp == NULL ? relay_sp : new_relay_sp;
    relay_home = new_relay_home == NULL ? relay_home : new_relay_home;
    power_sp = new_power_sp == NULL ? power_sp : new_power_sp;
    power_home = new_power_home == NULL ? power_home : new_power_home;
    LOG_INFO("Relay states updated: relay_sp=%d, relay_home=%d, power_sp=%.2f, power_home=%.2f\n",
             relay_sp, relay_home, power_sp, power_home);

    coap_set_status_code(response, CHANGED_2_04);
}

static void res_event_handler(void)
{
    coap_notify_observers(&res_relay);
    
    LOG_DBG("relay resource event handler called\n");
}
