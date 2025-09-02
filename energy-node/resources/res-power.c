#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

#include "sys/log.h"
#define LOG_MODULE "PW"
#define LOG_LEVEL LOG_LEVEL_APP

// extern resources
char* str(float value, char* output);
enum status_t {STATUS_ON, STATUS_ANTIDUST, STATUS_ALARM};
extern enum status_t energyNodeStatus;

// Generated power parameters
#define MAX_POWER 3000.0 // in W
#define MAX_OFFSET_PREDICTION 0.1 * MAX_POWER
#define MAX_STEP 0.005 * MAX_POWER

float gen_power = 0.0; // in W

bool defected = false; // true if the solar panel is defected

float solar_power_predict();

static void update_gen_power()
{
    if (energyNodeStatus != STATUS_ON)
    {
        gen_power = 0.0;
        LOG_INFO("Energy Node is not ON, generated power set to 0.0 W\n");
        return;
    }
    
    float expected_power = solar_power_predict();
    
    float step;
    
    if (defected)
        step = - 1.5 * MAX_OFFSET_PREDICTION;
    else
    {
        step = (float) random_rand() / (float) RANDOM_RAND_MAX * 2.0;
        step -= 1.0; // range [-1.0, 1.0]
        step *=  MAX_STEP;
    }

    gen_power = (expected_power + step < 0.0) ? 0.0 : 
                (expected_power + step > MAX_POWER) ? MAX_POWER :
                expected_power + step;

    char gp[16];
    LOG_INFO("Generated power updated: %s W (defected: %d)\n", str(gen_power, gp), defected);
}

void gen_power_json_string(char* buffer)
{
    char buf[16];
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE, 
            "{\"n\":\"gen_power\",\"v\":\"%s\"}", 
            str(gen_power, buf));
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
    LOG_DBG("Sending generated power: %s W\n", (char *)buffer);
}

static void res_event_handler(void)
{
    update_gen_power();
    coap_notify_observers(&res_gen_power);
    
    LOG_DBG("gen_power resource event handler called\n");
}
