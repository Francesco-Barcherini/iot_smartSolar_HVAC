#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"
#include "sys/log.h"
#define LOG_MODULE "RELAY"
#define LOG_LEVEL LOG_LEVEL_APP

#define MAX_POWER 3000.0 // Maximum flow of power in W

// external resources
extern coap_endpoint_t hvac_node_endpoint;
char* str(float value, char* output);
void updateChargeRate(float rate);

// Relay states. Destination of Solar Panel energy, souce of home energy.
enum relay_sp_t { RELAY_SP_HOME, RELAY_SP_BATTERY, RELAY_SP_GRID };
enum relay_home_t { RELAY_HOME_SP, RELAY_HOME_BATTERY, RELAY_HOME_GRID };

enum relay_sp_t relay_sp = RELAY_SP_BATTERY; // Relay state for solar panel
static enum relay_home_t relay_home = RELAY_HOME_GRID; // Relay state for home
float power_sp = 0.0; // Power from solar panel
static float power_home = 0.0; // Power for home

void updateBatteryChargeRate()
{
    float rate_battery = 0.0;
    if (relay_sp == RELAY_SP_BATTERY)
        rate_battery += power_sp;
    if (relay_home == RELAY_HOME_BATTERY)
        rate_battery -= power_home;
    updateChargeRate(rate_battery); // Update charge rate based on relay states
}

void update_relay(enum relay_sp_t new_relay_sp, enum relay_home_t new_relay_home, float new_power_sp, float new_power_home)
{
    relay_sp = new_relay_sp;
    relay_home = new_relay_home;
    power_sp = new_power_sp == -1.0 ? power_sp : new_power_sp;
    power_home = new_power_home == -1.0 ? power_home : new_power_home;

    updateBatteryChargeRate();

    char power_sp_str[16], power_home_str[16];
    LOG_INFO("Relay state updated: relay_sp=%d, relay_home=%d, power_sp=%s, power_home=%s\n",
             relay_sp, relay_home, str(power_sp, power_sp_str), str(power_home, power_home_str));
}

void relay_json_string(char* buffer)
{
    // json of relays state and power consumption
    char buf1[16], buf2[16];
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE,
            "{\"n\":\"relay\",\"r_sp\":%d,\"r_h\":%d,\"p_sp\":%s,\"p_h\":%s}",
            relay_sp, relay_home, str(power_sp, buf1), str(power_home, buf2));
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_relay,
                "title=\"Relay state\";rt=\"Control\";obs",
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
    const char *buffer_var = NULL;
    char req_relay_sp[16], req_relay_home[16];
    char req_power_sp[16], req_power_home[16];

    enum relay_sp_t new_relay_sp = relay_sp;
    enum relay_home_t new_relay_home = relay_home;
    float new_power_sp = power_sp;
    float new_power_home = power_home;

    int len;

    len = coap_get_post_variable(request, "r_sp", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_relay_sp, buffer_var, len);
        req_relay_sp[len] = '\0'; // Null-terminate the string
    }
    len = coap_get_post_variable(request, "r_h", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_relay_home, buffer_var, len);
        req_relay_home[len] = '\0'; // Null-terminate the string
    }
    len = coap_get_post_variable(request, "p_sp", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_power_sp, buffer_var, len);
        req_power_sp[len] = '\0'; // Null-terminate the string
    }
    len = coap_get_post_variable(request, "p_h", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_power_home, buffer_var, len);
        req_power_home[len] = '\0'; // Null-terminate the string
    }

    if (req_relay_sp != NULL) {
        new_relay_sp = (enum relay_sp_t) atoi(req_relay_sp);
        LOG_DBG("New relay_sp: %d\n", new_relay_sp);
    }

    if (req_relay_home != NULL) {
        new_relay_home = (enum relay_home_t) atoi(req_relay_home);
        LOG_DBG("New relay_home: %d\n", new_relay_home);
    }

    if (req_power_sp != NULL) {
        new_power_sp = atof(req_power_sp);
        LOG_DBG("New power_sp: %s\n", req_power_sp);
    }

    if (req_power_home != NULL) {
        new_power_home = atof(req_power_home);
        LOG_DBG("New power_home: %s\n", req_power_home);
    }

    // Check for valid relay states and power values

    if (new_power_sp < 0.0 || new_power_sp > MAX_POWER ||
        new_power_home < 0.0 || new_power_home > MAX_POWER)
    {
        LOG_ERR("Invalid power values: power_sp=%s, power_home=%s\n", req_power_sp, req_power_home);
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
        if (new_relay_home == RELAY_HOME_SP)
        {
            LOG_ERR("Invalid relay state: new_relay_sp=%d, new_relay_home=%d\n", 
                     new_relay_sp, new_relay_home);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
    }

    // Update relay states and power values
    relay_sp = new_relay_sp;
    relay_home = new_relay_home;
    power_sp = new_power_sp == -1.0 ? power_sp : new_power_sp;
    power_home = new_power_home == -1.0 ? power_home : new_power_home;
    LOG_INFO("Relay states updated: relay_sp=%d, relay_home=%d, power_sp=%s, power_home=%s\n",
             relay_sp, relay_home, req_power_sp, req_power_home);

    updateBatteryChargeRate(); // Update charge rate based on relay states

    coap_set_status_code(response, CHANGED_2_04);

    if(coap_endpoint_cmp(&hvac_node_endpoint, request->src_ep) != 0)
        res_event_handler(); // Notify observers if the request is from the HVAC (green mode)
}

static void res_event_handler(void)
{
    coap_notify_observers(&res_relay); 
    LOG_DBG("relay resource event handler called\n");
}
