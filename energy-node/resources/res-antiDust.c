#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

#include "sys/log.h"
#define LOG_MODULE "DUST"
#define LOG_LEVEL LOG_LEVEL_APP

enum antiDust_t {ANTIDUST_OFF, ANTIDUST_ON, ANTIDUST_ALARM};
enum antiDust_t antiDustState = ANTIDUST_OFF; // AntiDust state for solar panel

void set_antidust_handler(enum antiDust_t oldState);

void update_antiDust(enum antiDust_t newState)
{
    antiDustState = newState;
    LOG_INFO("AntiDust state updated: %d\n", antiDustState);
}

void antiDust_json_string(char* buffer)
{
    // json of antiDust state
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE,
            "{\"n\":\"antiDust\",\"v\":%d}",
            antiDustState);
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_antiDust,
                "title=\"antiDust control (on|off|alarm)\";rt=\"Control\";obs",
                res_get_handler,
                res_post_put_handler,
                res_post_put_handler,
                NULL,                
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    antiDust_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("antiDust resource GET handler called\n");
}

static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    const char *req_antiDust = NULL;
    enum antiDust_t old_antiDust = antiDustState;
    enum antiDust_t new_antiDust = ANTIDUST_OFF;

    coap_get_post_variable(request, "antiDust", &req_antiDust);

    if (req_antiDust != NULL) {
        if (strcmp(req_antiDust, "on") == 0) {
            if (old_antiDust == ANTIDUST_ALARM) {
                LOG_ERR("Cannot set antiDust to ON from ALARM state\n");
                coap_set_status_code(response, BAD_REQUEST_4_00);
                return;
            }
            new_antiDust = ANTIDUST_ON;
        } else if (strcmp(req_antiDust, "off") == 0) {
            new_antiDust = ANTIDUST_OFF;
        } else {
            LOG_ERR("Invalid antiDust state: %s\n", req_antiDust);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
    }

    update_antiDust(new_antiDust);
    coap_set_status_code(response, CHANGED_2_04);

    set_antidust_handler(old_antiDust);
}

static void res_event_handler(void)
{
    coap_notify_observers(&res_antiDust);
    LOG_DBG("antiDust resource event handler called\n");
}
