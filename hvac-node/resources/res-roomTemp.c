#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

#include "sys/clock.h"

#include "sys/log.h"
#define LOG_MODULE "ROOMT"
#define LOG_LEVEL LOG_LEVEL_APP

// Conditioner parameters
#define MIN_POWER 0.0
#define MAX_POWER 1000.0 // W

// T_new = T_old + (deltaT * c1 - power * c2) * elapsed_time
// Temperature parameters
#define DELTAT_COEFF 0.05
#define POWER_COEFF 1 / MAX_POWER
#define MAX_RANDOM_OFFSET 0.2
#define MIN_TEMP 15.0
#define MAX_TEMP 40.0

// external resources
extern float conditioner_power; // Power of the conditioner in W
extern float outTemp;
enum status_t {STATUS_OFF, STATUS_VENT, STATUS_COOL, STATUS_HEAT, STATUS_ERROR};
extern enum status_t status = STATUS_OFF;

float roomTemp = 28.0;
float lastUpdateTime = 0.0;

char* str(float value, char* output);

static float roomTemp = 22.0;

static void update_roomTemp()
{
    if (lastUpdateTime == 0.0)
        lastUpdateTime = clock_seconds();
    unsigned long currentTime = clock_seconds();
    unsigned long elapsedTime = currentTime - lastUpdateTime;

    float outside_contribution = DELTAT_COEFF * (outTemp - roomTemp);
    float conditioner_contribution = 
        (status == STATUS_OFF || status == STATUS_VENT || status == STATUS_ERROR) ? 0.0 :
        (status == STATUS_COOL) ? -conditioner_power * POWER_COEFF :
        (status == STATUS_HEAT) ? conditioner_power * POWER_COEFF : 
        0.0;

    // Update room temperature
    roomTemp = roomTemp +
                (outside_contribution + conditioner_contribution) * elapsedTime;

    // Add a random offset
    float random_offset = (float) random_rand() / (float) RANDOM_RAND_MAX * 2.0 - 1.0;
    random_offset *= MAX_RANDOM_OFFSET;
    roomTemp += random_offset;

    // Ensure temperature stays within bounds
    if (roomTemp < MIN_TEMP) {
        roomTemp = MIN_TEMP;
    } else if (roomTemp > MAX_TEMP) {
        roomTemp = MAX_TEMP;
    }

    lastUpdateTime = currentTime;

    char roomTemp_str[16];
    LOG_INFO("New room temperature: roomTemp=%s°C\n",
            str(roomTemp, roomTemp_str));
}

void roomTemp_json_string(char* buffer)
{
    char buf1[16];
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE,
            "{\"n\":\"roomTemp\",\"v\":%s}",
            str(roomTemp, buf1));
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_roomTemp,
                "title=\"Room temperature\";rt=\"sensor\";obs",
                res_get_handler,
                NULL,
                NULL,
                NULL,                
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    roomTemp_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("Room temperature resource GET handler called\n");
}

static void res_event_handler(void)
{
    update_roomTemp();
    coap_notify_observers(&res_roomTemp);
    
    LOG_DBG("Room temperature resource event handler called\n");
}
