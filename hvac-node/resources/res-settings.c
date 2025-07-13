#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "random.h"

#include "sys/log.h"
#define LOG_MODULE "AIRCOND"
#define LOG_LEVEL LOG_LEVEL_APP

// Conditioner parameters
#define MIN_POWER 0.0
#define MAX_POWER 1000.0 // W

// external resources
enum status_t {STATUS_OFF, STATUS_VENT, STATUS_COOL, STATUS_HEAT, STATUS_ERROR};
enum cond_mode_t {MODE_NORMAL, MODE_GREEN};
char* str(float value, char* output);
void handle_settings(float old_power, enum status_t old_status, enum cond_mode_t old_mode, float old_target_temp);

float conditioner_power = 0.0; // Power of the conditioner in W
enum status_t status = STATUS_OFF;
enum cond_mode_t cond_mode = MODE_GREEN;
float target_temp = 27.5;

// void update_settings(float new_power, enum status_t new_status, enum cond_mode_t new_mode)
// {
//     if (new_power < MIN_POWER || new_power > MAX_POWER) {
//         LOG_ERR("Invalid power value: %f\n", new_power);
//         return;
//     }

//     conditioner_power = new_power == -1.0 ? conditioner_power : new_power; // -1.0 means no change
//     status = new_status;
//     cond_mode = new_mode;

//     char power_str[16];
//     LOG_INFO("HVAC settings updated: power=%s, status=%d, mode=%d\n",
//              str(conditioner_power, power_str), status, cond_mode);
// }

void settings_json_string(char* buffer)
{
    // json of settings
    char buf1[16], buf2[16];
    snprintf(buffer, 
            COAP_MAX_CHUNK_SIZE,
            "{\"n\":\"settings\",\"pw\":%s,\"status\":%d,\"mode\":%d,\"targetTemp\":%s}",
            str(conditioner_power, buf1), status, cond_mode, str(target_temp, buf2));
}

// RESOURCE definition
static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void res_event_handler(void);

EVENT_RESOURCE(res_settings,
                "title=\"HVAC power, status (off|vent|cool|heat|error), mode (normal|green)\";rt=\"control\";obs",
                res_get_handler,
                res_post_put_handler,
                res_post_put_handler,
                NULL,                
                res_event_handler);

static void res_get_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    settings_json_string((char *)buffer);

    coap_set_header_content_format(response, APPLICATION_JSON);
    coap_set_payload(response, buffer, strlen((char *)buffer));

    LOG_DBG("settings resource GET handler called\n");
}

static void res_post_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
    LOG_DBG("Received POST request for settings!\n");
    const char *buffer_var = NULL;
    char req_pw[16], req_status[16], req_mode[16];
    char req_target_temp[16];

    float new_power = conditioner_power;
    enum status_t new_status = status;
    enum cond_mode_t new_mode = cond_mode;
    float new_target_temp = -1.0; // -1.0 means no change

    int len;

    len = coap_get_post_variable(request, "pw", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_pw, buffer_var, len);
        req_pw[len] = '\0'; // Null-terminate the string
    }
    len = coap_get_post_variable(request, "status", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_status, buffer_var, len);
        req_status[len] = '\0'; // Null-terminate the string
    }
    len = coap_get_post_variable(request, "mode", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_mode, buffer_var, len);
        req_mode[len] = '\0'; // Null-terminate the string
    }
    len = coap_get_post_variable(request, "targetTemp", &buffer_var);
    if (len > 0 && len < 15)
    {
        memcpy(req_target_temp, buffer_var, len);
        req_target_temp[len] = '\0'; // Null-terminate the string
    }

    if (req_pw != NULL) {
        new_power = atof(req_pw);
        if (new_power != -1.0 && (new_power < MIN_POWER || new_power > MAX_POWER)) {
            LOG_ERR("Invalid power value: %s\n", req_pw);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
        LOG_DBG("New power: %s\n", req_pw);
    }

    if (req_status != NULL) {
        if (strcmp(req_status, "off") == 0) {
            new_status = STATUS_OFF;
        } else if (strcmp(req_status, "vent") == 0) {
            new_status = STATUS_VENT;
        } else if (strcmp(req_status, "cool") == 0) {
            new_status = STATUS_COOL;
        } else if (strcmp(req_status, "heat") == 0) {
            new_status = STATUS_HEAT;
        } else if (strcmp(req_status, "error") == 0) {
            new_status = STATUS_ERROR;
        } else if (strcmp(req_status, "same") == 0) {
            new_status = status; // No change
        } else {
            LOG_ERR("Invalid status: %s\n", req_status);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
        LOG_DBG("New status: %d\n", new_status);
    }

    if (req_mode != NULL) {
        if (strcmp(req_mode, "normal") == 0) {
            new_mode = MODE_NORMAL;
        } else if (strcmp(req_mode, "green") == 0) {
            new_mode = MODE_GREEN;
        } else {
            LOG_ERR("Invalid mode: %s\n", req_mode);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
        LOG_DBG("New mode: %d\n", new_mode);
    }

    if (req_target_temp != NULL) {
        new_target_temp = atof(req_target_temp);
        if (new_target_temp != -1.0 && (new_target_temp < 16.0 || new_target_temp > 35.0)) {
            LOG_ERR("Invalid target temperature: %s\n", req_target_temp);
            coap_set_status_code(response, BAD_REQUEST_4_00);
            return;
        }
        LOG_DBG("New target temperature: %f\n", new_target_temp);
    }

    float old_power = conditioner_power;
    enum status_t old_status = status;
    enum cond_mode_t old_mode = cond_mode;
    float old_target_temp = target_temp;

    // Update
    status = new_status;
    cond_mode = new_mode;
    conditioner_power = (status == STATUS_OFF || status == STATUS_ERROR) ? 0.0 :
                        (cond_mode == MODE_GREEN) ? conditioner_power :
                        (new_power == -1.0) ? conditioner_power : new_power;
    target_temp = (new_target_temp == -1.0) ? target_temp : new_target_temp;
    
    char power_str[16], target_temp_str[16];
    LOG_INFO("Air conditioning updated: power=%s, status=%d, mode=%d, targetTemp=%s\n",
             str(conditioner_power, power_str), status, cond_mode, str(target_temp, target_temp_str));

    coap_set_status_code(response, CHANGED_2_04);

    handle_settings(old_power, old_status, old_mode, old_target_temp);
}

static void res_event_handler(void)
{
    coap_notify_observers(&res_settings);
    LOG_DBG("settings resource event handler called\n");
}
