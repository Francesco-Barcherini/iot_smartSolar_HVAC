#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_APP

// Generated power parameters
#define MAX_POWER 1500.0 // in W
#define MAX_OFFSET_PREDICTION 0.2 * MAX_POWER

float gen_power = 0.0; // in W

float solar_power_prediction();

static void update_gen_power(bool defected = false)
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

void gen_power_json_string(char* buffer)
{
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE, 
            "{\"n\":\"gen_power\",\"v\":%.2f}", 
            gen_power);
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_gen_power,
                "title=\"gen_power data\";rt=\"gen_power\";obs",
                res_get_handler,
                NULL,
                NULL,
                NULL,                
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    gen_power_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("gen_power resource GET handler called\n");
}

static void res_event_handler(void)
{
    update_gen_power();
    coap_notify_observers(&res_gen_power);
    
    LOG_DBG("gen_power resource event handler called\n");
}
