#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

// Solar Power Prediction
#include "../solar-power-model.h"
#define NUM_INPUT 3
#define MAX_POWER 1500.0

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_APP

// Generated weather parameters
#define MIN_IRRADIATION 0.0
#define MAX_IRRADIATION 1.5
#define MAX_IRR_DIFF 0.05

#define MIN_OUT_TEMPERATURE 20.0
#define MAX_OUT_TEMPERATURE 35.0
#define MAX_TEMP_DIFF 0.5

#define MIN_MODULE_TEMPERATURE 15.0
#define MAX_MODULE_TEMPERATURE 65.0
#define MAX_MODULE_TEMP_DIFF 0.5

static float irradiation = (MIN_IRRADIATION + MAX_IRRADIATION) / 2.0;
static float out_temperature = (MIN_OUT_TEMPERATURE + MAX_OUT_TEMPERATURE) / 2.0;
static float module_temperature = (MIN_MODULE_TEMPERATURE + MAX_MODULE_TEMPERATURE) / 2.0;

// Callable from outside: expected power prediction
float solar_power_predict()
{
    float inputs[NUM_INPUT];
    inputs[0] = out_temperature;
    inputs[1] = module_temperature;
    inputs[2] = irradiation;

    float prediction = solar_power_prediction_regress1(inputs, NUM_INPUT);
    
    if (prediction < 0.0) {
        LOG_ERR("Negative power prediction, returning 0.0\n");
        return 0.0;
    }

    if (prediction > MAX_POWER) {
        LOG_ERR("Power prediction exceeds maximum limit, returning MAX_POWER\n");
        return MAX_POWER;
    }
    
    LOG_INFO("Solar power prediction: %.2f W\n", prediction);
    return prediction;
}

static void update_weather()
{
    float step_irr = (float) random_rand() / (float) RANDOM_RAND_MAX * 2.0;
    step_irr -= 1.0; // range [-1.0, 1.0]
    step_irr *= MAX_IRR_DIFF;
    irradiation += step_irr;

    float step_temp = (float) random_rand() / (float) RANDOM_RAND_MAX * 2.0;
    step_temp -= 1.0; // range [-1.0, 1.0]
    step_temp *= MAX_TEMP_DIFF;
    out_temperature += step_temp;

    float step_module_temp = (float) random_rand() / (float) RANDOM_RAND_MAX * 2.0;
    step_module_temp -= 1.0; // range [-1.0, 1.0]
    step_module_temp *= MAX_MODULE_TEMP_DIFF;
    module_temperature += step_module_temp;

    LOG_INFO("New weather values: Irradiation=%.2f, Out Temperature=%.2f, Module Temperature=%.2f\n",
             irradiation, out_temperature, module_temperature);
}

void weather_json_string(char* buffer)
{
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE,
            "{\"n\":\"weather\",\"irr\":%.2f,\"outTemp\":%.2f,\"modTemp\":%.2f}",
            irradiation, out_temperature, module_temperature);
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_weather,
                "title=\"Weather data (irr, outTemp, modTemp)\";rt=\"weather\";obs",
                res_get_handler,
                NULL,
                NULL,
                NULL,                
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    weather_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("Weather resource GET handler called\n");
}

static void res_event_handler(void)
{
    update_weather();
    coap_notify_observers(&res_weather);
    
    LOG_DBG("Weather resource event handler called\n");
}
