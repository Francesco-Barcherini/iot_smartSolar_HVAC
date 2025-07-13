#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "contiki.h"
#include "sys/log.h"
#include "coap-callback-api.h"
#include "coap-observe-client.h"

#include "os/dev/button-hal.h"
#include "os/dev/leds.h"
#include "jsonparse.h"

#include "coap-engine.h"

/* Log configuration */
#define LOG_MODULE "HVAC"
#define LOG_LEVEL LOG_LEVEL_APP

/* COAP energy-node URL */
#ifdef COOJA
    #define ENERGY_NODE_EP "coap://[fd00::202:2:2:2]:5683"
#else /*NRF52840*/
    #define ENERGY_NODE_EP "coap://[fd00::f6ce:3640:d05a:3662]:5683"
#endif

#define WEATHER_URI "/sensors/weather"
#define BATTERY_URI "/sensors/battery"
#define GEN_POWER_URI "/sensors/power"
#define RELAY_URI "/relay"
// #define ANTIDUST_URI "/antiDust"

// Publish intervals
#define LONG_INTERVAL CLOCK_SECOND * 15
#define SHORT_INTERVAL CLOCK_SECOND * 7
#define BLINK_INTERVAL CLOCK_SECOND * 0.2
#define GREEN_INTERVAL CLOCK_SECOND * 10

// Power parameters
#define VENT_POWER 50.0
#define DELTAT_COEFF 0.05
#define POWER_COEFF 1 / 1000.0 // Assuming max power is 1000 W
#define SECONDS 60.0
#define DC_AC_COEFF 10.0

//external resources
enum status_t {STATUS_OFF, STATUS_VENT, STATUS_COOL, STATUS_HEAT, STATUS_ERROR};
enum cond_mode_t {MODE_NORMAL, MODE_GREEN};

enum relay_sp_t { RELAY_SP_HOME, RELAY_SP_BATTERY, RELAY_SP_GRID };
enum relay_home_t { RELAY_HOME_SP, RELAY_HOME_BATTERY, RELAY_HOME_GRID };

extern float roomTemp;
extern float conditioner_power;
extern enum status_t status;
extern enum cond_mode_t cond_mode;
extern float target_temp;

// data from energy node
float outTemp = 27.5;
float gen_power = 0.0;
float battery_level = 5000.0;


static struct etimer green_timer;

// Resources
extern coap_resource_t res_roomTemp, res_settings;

// Custom events
static process_event_t green_start_event;
static process_event_t restart_obs;

// Process
PROCESS(hvac_node_process, "HVAC Node Process");
AUTOSTART_PROCESSES(&hvac_node_process);

char* str(float value, char* output)
{
    int integer = (int) value;
    float fraction = value - integer;
    int fraction_int = (int)(fraction * 100);
    snprintf(output, 16, "%d.%d", integer, fraction_int);
    return output;
}

// #if PLATFORM_HAS_LEDS || LEDS_COUNT
//     etimer_stop(&blink_timer);
//     if (defected)
//         leds_single_on(LEDS_YELLOW); // Indicate defected mode
//     else
//         leds_single_off(LEDS_YELLOW); // Turn off yellow LED
// #endif

void stop_observation_battery();
void stop_observation_gen_power();

void green_stop()
{
    cond_mode = MODE_NORMAL; // Reset mode
    etimer_stop(&green_timer); // Stop green mode timer
    stop_observation_battery(); // Stop battery observation
    stop_observation_gen_power(); // Stop gen power observation
}

void handle_stop()
{
    conditioner_power = 0.0; // Reset power

    if (cond_mode == MODE_GREEN)
        green_stop();
}

void handle_settings(float old_power, enum status_t old_status, enum cond_mode_t old_mode, float old_target_temp)
{
    // working -> off|error
    if (old_status != STATUS_OFF && old_status != STATUS_ERROR
        && (status == STATUS_OFF || status == STATUS_ERROR))
    {
        handle_stop();
        return;
    }

    // print old and new
    char old_power_str[16], old_target_temp_str[16];
    LOG_INFO("Old settings: power=%s, status=%d, mode=%d, targetTemp=%s\n",
             str(old_power, old_power_str), old_status, old_mode, str(old_target_temp, old_target_temp_str));
    char new_power_str[16], new_target_temp_str[16];
    LOG_INFO("New settings: power=%s, status=%d, mode=%d, targetTemp=%s\n",
             str(conditioner_power, new_power_str), status, cond_mode, str(target_temp, new_target_temp_str));

    // normal -> green
    if (old_mode == MODE_NORMAL && cond_mode == MODE_GREEN)
    {
        if (status == STATUS_OFF || status == STATUS_ERROR)
        {
            LOG_ERR("Cannot switch to green mode when HVAC is off or in error state.\n");
            cond_mode = MODE_NORMAL;
            return;
        }

        LOG_INFO("Switching to green mode.\n");
        process_post(&hvac_node_process, green_start_event, NULL);
        return;
    }

    // green -> normal
    if (old_mode == MODE_GREEN && cond_mode == MODE_NORMAL)
    {
        LOG_INFO("Switching to normal mode.\n");
        green_stop();
        return;
    } 
}

// CoAP observation
static coap_endpoint_t energy_node_endpoint;

static coap_observee_t* weather_obs;
static coap_observee_t* battery_obs;
static coap_observee_t* gen_power_obs;

void get_value_from_json(const uint8_t *json, int len)
{
    struct jsonparse_state state;
    char key[16];
    char value_str[16];

    int next;
    jsonparse_setup(&state, (const char *)json, len);

    char name_buf[16] = {0};
    char value_buf[16] = {0};
    bool found_name = false;

    while((next = jsonparse_next(&state)) != JSON_TYPE_ERROR) {
        if(next == JSON_TYPE_PAIR_NAME) {
            jsonparse_copy_value(&state, key, sizeof(key));

            if(strcmp(key, "n") == 0) {
                if(jsonparse_next(&state) == JSON_TYPE_STRING) {
                    jsonparse_copy_value(&state, name_buf, sizeof(name_buf));
                    found_name = true;
                }
            } else if(strcmp(key, "v") == 0 && found_name) {
                if(jsonparse_next(&state) == JSON_TYPE_STRING || jsonparse_get_type(&state) == JSON_TYPE_NUMBER) {
                    jsonparse_copy_value(&state, value_buf, sizeof(value_buf));
                    float v = atof(value_buf);

                    if(strcmp(name_buf, "battery") == 0) {
                        battery_level = v;
                        LOG_INFO("Battery updated: %s\n", value_buf);
                    } else if(strcmp(name_buf, "gen_power") == 0) {
                        gen_power = v;
                        LOG_INFO("Gen Power updated: %s\n", value_buf);
                    }
                }
            } else if(strcmp(key, "outTemp") == 0) {
                if(jsonparse_next(&state) == JSON_TYPE_STRING || jsonparse_get_type(&state) == JSON_TYPE_NUMBER) {
                    jsonparse_copy_value(&state, value_str, sizeof(value_str));
                    outTemp = atof(value_str);
                    LOG_INFO("Weather outTemp updated: %s\n", value_str);
                }
            }
        }
    }
}

static bool observing[3] = {false, false, false}; // Weather, Battery, Gen Power

/* COAP Notification handler*/
static void notification_callback(coap_observee_t* obs, void* notification, coap_notification_flag_t flag)
{
    int len = 0;
    const uint8_t* payload = NULL;

    if (notification) {
        len = coap_get_payload(notification, &payload);
    }
    switch(flag) {
        case NOTIFICATION_OK:
            LOG_DBG("Received %s\n", (char *)payload);
            get_value_from_json(payload, len);
            if (strcmp(obs->url, WEATHER_URI) == 0)
                observing[0] = true;
            else if (strcmp(obs->url, BATTERY_URI) == 0)
                observing[1] = true;
            else if (strcmp(obs->url, GEN_POWER_URI) == 0)
                observing[2] = true;
            break;
        case OBSERVE_OK: /* server accepeted observation request */
            LOG_INFO("%s accepted observe request\n", obs->url);
            break;
        case OBSERVE_NOT_SUPPORTED:
            LOG_WARN("%s does not support observation\n", obs->url);
            status = STATUS_ERROR;
            handle_stop();
            LOG_ERR("HVAC system in error state.\n");
            break;
        case ERROR_RESPONSE_CODE:
            LOG_WARN("%s sent error code: %*s\n", obs->url, len, (char *)payload);
            status = STATUS_ERROR;
            handle_stop();
            LOG_ERR("HVAC system in error state.\n");
            break;
        case NO_REPLY_FROM_SERVER:
            LOG_WARN("%s did not reply: "
                    "removing observe registration with token %x%x\n",
                    obs->url, obs->token[0], obs->token[1]);
            process_post(&hvac_node_process, restart_obs, obs);
            if (strcmp(obs->url, WEATHER_URI) == 0)
                observing[0] = false;
            else if (strcmp(obs->url, BATTERY_URI) == 0)
                observing[1] = false;
            else if (strcmp(obs->url, GEN_POWER_URI) == 0)
                observing[2] = false;
            break;
    }
}


void start_observation_weather()
{
    LOG_INFO("Starting weather observation\n");
    coap_obs_remove_observee(weather_obs);
    weather_obs = coap_obs_request_registration(
        &energy_node_endpoint, WEATHER_URI, notification_callback, NULL
    );
}

void start_observation_battery()
{
    LOG_INFO("Starting battery observation\n");
    coap_obs_remove_observee(battery_obs);
    battery_obs = coap_obs_request_registration(
        &energy_node_endpoint, BATTERY_URI, notification_callback, NULL
    );
}

void start_observation_gen_power()
{
    LOG_INFO("Starting gen power observation\n");
    coap_obs_remove_observee(gen_power_obs);
    gen_power_obs = coap_obs_request_registration(
        &energy_node_endpoint, GEN_POWER_URI, notification_callback, NULL
    );
}

void stop_observation_battery()
{
    LOG_INFO("Stopping battery observation\n");
    coap_obs_remove_observee(battery_obs);
    observing[1] = false;
}

void stop_observation_gen_power()
{
    LOG_INFO("Stopping gen power observation\n");
    coap_obs_remove_observee(gen_power_obs);
    observing[2] = false;
}

void
client_chunk_handler(coap_callback_request_state_t *state)
{

  coap_message_t *response = state->state.response;

  const uint8_t *chunk;

  if(response == NULL) {
    puts("Request timed out");
    return;
  }

//   int len = coap_get_payload(response, &chunk);

//   printf("|%.*s", len, (char *)chunk);
}

static coap_callback_request_state_t req_state;

PROCESS_THREAD(hvac_node_process, ev, data) 
{
    static struct etimer rootTemp_timer;

    //static bool long_press;

    PROCESS_BEGIN();

#if PLATFORM_HAS_LEDS || LEDS_COUNT
    leds_on(LEDS_RED); // Indicate hvac node is starting
#endif

    LOG_DBG("Starting hvac node\n");

    //setlocale(LC_NUMERIC, "C");

    // Initialize resources
    coap_activate_resource(&res_roomTemp, "roomTemp");
    coap_activate_resource(&res_settings, "settings");

    // Initialize CoAP endpoint
    coap_endpoint_parse(ENERGY_NODE_EP, strlen(ENERGY_NODE_EP), &energy_node_endpoint);

    // Initialize events
    green_start_event = process_alloc_event();
    restart_obs = process_alloc_event();

    // Initialize observations
    start_observation_weather();

    // Initialize timers
    etimer_set(&rootTemp_timer, SHORT_INTERVAL);

    while(1) {
        PROCESS_WAIT_EVENT();

        printf("\n");

        if (ev == PROCESS_EVENT_TIMER)
        {
            if (data == &rootTemp_timer) {
                // Trigger rootTemp resources
                res_roomTemp.trigger();
                etimer_reset(&rootTemp_timer);
            }
            // else if (data == &blink_timer) {
            //     Blink yellow LED in anti-dust mode
            //     if (energyNodeStatus == STATUS_ANTIDUST) {
            //         leds_single_toggle(LEDS_YELLOW);
            //         etimer_reset(&blink_timer);
            //     }
            // }
            else if (data == &green_timer) {
                if (status == STATUS_OFF || status == STATUS_ERROR || 
                    cond_mode != MODE_GREEN) {
                    LOG_ERR("Cannot start green mode, HVAC is off or in error state.\n");
                    handle_stop();
                    continue;
                }

                if (!observing[0] || !observing[1] || !observing[2]) {
                    LOG_DBG("Green mode: waiting observe registration complete\n");
                    LOG_DBG("Observing: %d, %d, %d\n",
                        observing[0], observing[1], observing[2]);
                    etimer_reset(&green_timer);
                    continue;
                }

                float needed_power;

                if (status == STATUS_VENT)
                    needed_power = VENT_POWER;
                else
                {
                    needed_power = (0.2 * (target_temp - roomTemp) / SECONDS) - (outTemp - roomTemp) * DELTAT_COEFF;
                    needed_power /= POWER_COEFF;
                    if (status == STATUS_COOL)
                        needed_power = -needed_power; // Cool mode uses negative power

                    if (needed_power < 0.0)
                        needed_power = 0.0; // No need for power if cooling is not needed
                }

                char needed_power_str[16], gen_power_str[16], battery_level_str[16];
                LOG_DBG("Green mode: needed power = %s W, gen power = %s W, battery level = %s Wh\n",
                         str(needed_power, needed_power_str), str(gen_power, gen_power_str), str(battery_level, battery_level_str));

                coap_message_t request[1];
                coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
                coap_set_header_uri_path(request, RELAY_URI);

                char payload[COAP_MAX_CHUNK_SIZE];
                char buf[16], buf2[16];
                // compare with gen_power
                if (needed_power <= gen_power) 
                {
                    snprintf(payload, // Ask needed power to energy node
                        COAP_MAX_CHUNK_SIZE,
                        "n=relay&r_sp=%d&r_h=%d&p_sp=%s&p_h=%s",
                        (int) RELAY_SP_HOME, (int) RELAY_HOME_SP, str(needed_power, buf), str(needed_power, buf2));
                    conditioner_power = needed_power; // Update conditioner power
                }
                else
                {
                    // try with the battery
                    float dc_needed_power = needed_power * DC_AC_COEFF;
                    if (dc_needed_power * GREEN_INTERVAL <= battery_level) // battery is enough
                    {
                        snprintf(payload, // Ask needed power to energy node
                            COAP_MAX_CHUNK_SIZE,
                            "n=relay&r_sp=%d&r_h=%d&p_sp=%s&p_h=%s",
                            (int) RELAY_SP_BATTERY, (int) RELAY_HOME_BATTERY, str(gen_power, buf), str(dc_needed_power, buf2));
                        conditioner_power = needed_power;
                    }
                    else // not enough power
                    {
                        // if cool, try vent
                        if (status == STATUS_COOL)
                        {
                            // gen_power is enough
                            if (VENT_POWER <= gen_power)
                            {
                                snprintf(payload, // Ask vent power to energy node
                                    COAP_MAX_CHUNK_SIZE,
                                    "n=relay&r_sp=%d&r_h=%d&p_sp=%s&p_h=%s",
                                    (int) RELAY_SP_HOME, (int) RELAY_HOME_SP, str(VENT_POWER, buf), str(VENT_POWER, buf2));
                                conditioner_power = VENT_POWER;
                            }
                            else
                            {
                                // try with the battery
                                float dc_needed_power = VENT_POWER * DC_AC_COEFF;
                                if (dc_needed_power * GREEN_INTERVAL/CLOCK_SECOND <= battery_level) // battery is enough
                                {
                                    snprintf(payload, // Ask vent power to energy node
                                        COAP_MAX_CHUNK_SIZE,
                                        "n=relay&r_sp=%d&r_h=%d&p_sp=%s&p_h=%s",
                                        (int) RELAY_SP_BATTERY, (int) RELAY_HOME_BATTERY, str(gen_power, buf), str(dc_needed_power, buf2));
                                        conditioner_power = VENT_POWER;
                                }
                                else // not enough in any case
                                {
                                    LOG_WARN("Not enough power for green mode, stopping the hvac.\n");
                                    status = STATUS_OFF;
                                    handle_stop();
                                    res_settings.trigger();
                                    continue;
                                }
                            }
                        }
                        else // not enough in any case
                        {
                            LOG_WARN("Not enough power for green mode, stopping the hvac.\n");
                            status = STATUS_OFF;
                            handle_stop();
                            res_settings.trigger();
                            continue;
                        }
                    }
                }
                char power_str[16], target_temp_str[16];
                LOG_INFO("New settings: power=%s, status=%d, mode=%d, targetTemp=%s\n",
                str(conditioner_power, power_str), status, cond_mode, str(target_temp, target_temp_str));

                coap_set_payload(request, (uint8_t *) payload, strlen(payload));
                coap_send_request(&req_state, &energy_node_endpoint, request, client_chunk_handler);
                LOG_DBG("Green mode request sent: %s\n", payload);

                // Reset the timer for the next green mode check
                etimer_reset(&green_timer);

                // TODO: Trigger the new settings?
            }
        }
        // Handle green mode
        else if (ev == green_start_event)
        {
            // start observations
            start_observation_battery();
            start_observation_gen_power();

            etimer_set(&green_timer, GREEN_INTERVAL);
        }
        // restart failed observation
        else if (ev == restart_obs)
        {
            // data is the observee that failed
            coap_observee_t* obs = (coap_observee_t*) data;
            LOG_DBG("Restarting observation for %s\n", obs->url);
            if (strcmp(obs->url, WEATHER_URI) == 0) {
                start_observation_weather();
            } else if (strcmp(obs->url, BATTERY_URI) == 0) {
                start_observation_battery();
            } else if (strcmp(obs->url, GEN_POWER_URI) == 0) {
                start_observation_gen_power();
            } else {
                LOG_ERR("Unknown observee URL: %s\n", obs->url);
            }
        }
#if PLATFORM_HAS_BUTTON
        else if (ev == button_hal_periodic_event) {
            button_hal_button_t* btn = (button_hal_button_t*) data;
            if(btn->press_duration_seconds == 3) 
            {
                LOG_DBG("Button pressed for 3 seconds, toggling ERROR status.\n");
                //long_press = true;
                // toggle ERROR status
                if (status == STATUS_ERROR) {
                    conditioner_power = 0.0; // Reset power
                    cond_mode = MODE_NORMAL; // Reset mode
                    status = STATUS_OFF;
                    LOG_INFO("HVAC system recovered from error state.\n");
                }
                else 
                {
                    status = STATUS_ERROR;
                    handle_stop();
                    LOG_ERR("HVAC system in error state.\n");
                }
                res_settings.trigger();
            }
        }
#endif /* PLATFORM_HAS_BUTTON */
    }

    PROCESS_END();
}

