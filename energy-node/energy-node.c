#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "contiki.h"
#include "sys/log.h"
#include "coap-blocking-api.h"
#include "coap.h"

#include "os/dev/button-hal.h"
#include "os/dev/leds.h"

#include "coap-engine.h"

/* Log configuration */
#define LOG_MODULE "ENERGY"
#define LOG_LEVEL LOG_LEVEL_APP

/* COAP energy-node URL */
#ifdef COOJA
    #define HVAC_NODE_EP "coap://[fd00::203:3:3:3]:5683"
#else /*NRF52840*/
    #define HVAC_NODE_EP "coap://[fd00::f6ce:3627:65f2:492f]:5683"
#endif

#define SETTINGS_URI "/settings"

// Publish intervals
#define LONG_INTERVAL CLOCK_SECOND * 10
#define SHORT_INTERVAL CLOCK_SECOND * 2
#define ANTIDUST_INTERVAL CLOCK_SECOND * 6
#define BLINK_INTERVAL CLOCK_SECOND * 0.2

// Power parameters
#define MAX_POWER 1500.0 // in W
#define MAX_OFFSET_PREDICTION 0.2 * MAX_POWER
#define WRONG_PREDICTIONS_THRESHOLD_DUST 5
#define WRONG_PREDICTIONS_THRESHOLD_ALARM 10

// Resources
extern coap_resource_t res_weather, res_battery, res_gen_power, res_relay, res_antiDust;

//extern variables and functions
enum antiDust_t {ANTIDUST_OFF, ANTIDUST_ON};
enum relay_sp_t { RELAY_SP_HOME, RELAY_SP_BATTERY, RELAY_SP_GRID };
enum relay_home_t { RELAY_HOME_SP, RELAY_HOME_BATTERY, RELAY_HOME_GRID };
extern float gen_power; // Generated power in W
extern bool defected; // true if the solar panel is defected
float solar_power_predict();
void update_antiDust(enum antiDust_t newState);
void updateChargeRate(float rate);
void update_relay(enum relay_sp_t new_relay_sp, enum relay_home_t new_relay_home, float new_power_sp, float new_power_home);

// Status
enum status_t {STATUS_ON, STATUS_ANTIDUST, STATUS_ALARM};
enum status_t energyNodeStatus = STATUS_ON;

static unsigned int nWrongPredictions = 0; // Counter for wrong predictions

struct etimer blink_timer;

// CoAP observation
static coap_endpoint_t hvac_node_endpoint;

char* str(float value, char* output)
{
    int integer = (int) value;
    float fraction = value - integer;
    int fraction_int = (int)(fraction * 100);
    snprintf(output, 16, "%d.%d", integer, fraction_int);
    return output;
}

// Process
PROCESS(energy_node_process, "Energy Node Process");
AUTOSTART_PROCESSES(&energy_node_process);

process_event_t send_green_setting_event;

static void alarm_handler()
{
    energyNodeStatus = STATUS_ALARM;

    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode
    res_antiDust.trigger();
    
    res_gen_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_BATTERY, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();

    process_post(&energy_node_process, send_green_setting_event, NULL);
}

static void restart()
{
    energyNodeStatus = STATUS_ON;

    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode
    res_antiDust.trigger();
    
    res_gen_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_GRID, RELAY_HOME_GRID, gen_power, -1.0);
    res_relay.trigger();
}

static void antidust_handler()
{
    energyNodeStatus = STATUS_ANTIDUST;

#if PLATFORM_HAS_LEDS || LEDS_COUNT
    etimer_set(&blink_timer, BLINK_INTERVAL);
#endif

    update_antiDust(ANTIDUST_ON); // Enable anti-dust mode
    res_antiDust.trigger();

    res_gen_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_BATTERY, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();

    process_post(&energy_node_process, send_green_setting_event, NULL);
}

static void end_antidust_handler()
{
    energyNodeStatus = STATUS_ON;

#if PLATFORM_HAS_LEDS || LEDS_COUNT
    etimer_stop(&blink_timer);
    if (defected)
        leds_single_on(LEDS_YELLOW); // Indicate defected mode
    else
        leds_single_off(LEDS_YELLOW); // Turn off yellow LED
#endif
    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode
    res_antiDust.trigger();

    res_gen_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_GRID, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();
}

static void analyze_prediction(float prediction)
{
    if (gen_power >= prediction - MAX_OFFSET_PREDICTION)
    {
        nWrongPredictions = 0;
        return; // No change in status
    }

    char gen_power_str[16], prediction_str[16];
    LOG_WARN("Generated power is low: %s W, prediction: %s W\n", str(gen_power, gen_power_str), str(prediction, prediction_str));
    nWrongPredictions++;

    if (nWrongPredictions == WRONG_PREDICTIONS_THRESHOLD_ALARM)
    {
        LOG_ERR("Too many wrong predictions, triggering alarm!\n");
        alarm_handler();
    } 
    else if (nWrongPredictions == WRONG_PREDICTIONS_THRESHOLD_DUST)
    {
        LOG_WARN("Too many wrong predictions, switching to anti-dust mode!\n");
        antidust_handler();
    }
}

PROCESS_THREAD(energy_node_process, ev, data) 
{
    static struct etimer weather_battery_timer;
    static struct etimer gen_power_timer;
    static struct etimer prediction_timer;

    static struct etimer end_antiDust_timer;

    static bool long_press = false;

    PROCESS_BEGIN();

#if PLATFORM_HAS_LEDS || LEDS_COUNT
    leds_on(LEDS_GREEN); // Indicate energy node is starting
#endif

    LOG_DBG("Starting energy node\n");

    //setlocale(LC_NUMERIC, "C");
    send_green_setting_event = process_alloc_event();

    // Initialize resources
    coap_activate_resource(&res_weather, "sensors/weather");
    coap_activate_resource(&res_battery, "sensors/battery");
    coap_activate_resource(&res_gen_power, "sensors/power");
    coap_activate_resource(&res_relay, "relay");
    coap_activate_resource(&res_antiDust, "antiDust");

    // Initialize CoAP endpoint
    coap_endpoint_parse(HVAC_NODE_EP, strlen(HVAC_NODE_EP), &hvac_node_endpoint);

    // Initialize timers
    etimer_set(&weather_battery_timer, LONG_INTERVAL);
    etimer_set(&gen_power_timer, SHORT_INTERVAL);
    etimer_set(&prediction_timer, SHORT_INTERVAL);

    while(1) {
        PROCESS_WAIT_EVENT();

        printf("\n");

        if (ev == PROCESS_EVENT_TIMER)
        {
            if (data == &weather_battery_timer) {
                // Trigger weather and battery resources
                res_weather.trigger();
                res_battery.trigger();
                etimer_reset(&weather_battery_timer);
            }
            else if (data == &gen_power_timer) {
                // Trigger power generation resource
                if (energyNodeStatus == STATUS_ON)
                    res_gen_power.trigger();
                etimer_reset(&gen_power_timer);
            }
            else if (data == &prediction_timer) {
                if (energyNodeStatus == STATUS_ON)
                {
                    // Trigger prediction logic
                    float prediction = solar_power_predict();
                    char pred[16];
                    LOG_INFO("Solar power prediction: %s W\n", str(prediction, pred));
                    analyze_prediction(prediction);

                    if (energyNodeStatus == STATUS_ANTIDUST)
                    {
                        etimer_set(&end_antiDust_timer, ANTIDUST_INTERVAL);
                        LOG_DBG("Anti-dust mode active, will end in %d seconds\n", ANTIDUST_INTERVAL / CLOCK_SECOND);
                    }
                }
                etimer_reset(&prediction_timer);
            }
            else if (data == &end_antiDust_timer) {
                if (energyNodeStatus == STATUS_ANTIDUST) {
                    LOG_DBG("Ending anti-dust mode\n");
                    end_antidust_handler();
                }
                etimer_stop(&end_antiDust_timer);
            }
            else if (data == &blink_timer) {
                // Blink yellow LED in anti-dust mode
                if (energyNodeStatus == STATUS_ANTIDUST) {
                    leds_single_toggle(LEDS_YELLOW);
                    etimer_reset(&blink_timer);
                }
            }
        }
        else if (ev == send_green_setting_event)
        {
            coap_message_t request[1];
            coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
            coap_set_header_uri_path(request, SETTINGS_URI);

            // Prepare payload
            char payload[COAP_MAX_CHUNK_SIZE], buf1[16], buf2[16];
            snprintf(payload, COAP_MAX_CHUNK_SIZE, 
                "pw=%s&status=%s&mode=%s&targetTemp=%s",
                "0.0", "same", "green", "-1.0");
            //coap_set_header_content_format(request, TEXT_PLAIN);
            
            coap_set_payload(request, (uint8_t *) payload, strlen(payload));
            
            // Send request
            LOG_DBG("Sending green mode: %s\n", payload);
            COAP_BLOCKING_REQUEST(&hvac_node_endpoint, request, NULL);
        }

        // Handle button events
#if PLATFORM_HAS_BUTTON
        else if (ev == button_hal_release_event) 
        {
            if (long_press)
            {
                long_press = false; // Reset long press flag
                LOG_DBG("Button released after long press\n");
                continue; // Skip further processing
            }
            // toggle defected
            defected = !defected;

    #if PLATFORM_HAS_LEDS || LEDS_COUNT
            leds_single_toggle(LEDS_YELLOW);
    #endif /* PLATFORM_HAS_LEDS || LEDS_COUNT */
                
            char mode[16];
            strcpy(mode, defected ? "DUST" : "CLEAN");
            LOG_INFO("Toggled defected mode: %s\n", mode);
        } 
        else if (ev == button_hal_periodic_event) {
            button_hal_button_t* btn = (button_hal_button_t*) data;
            if(btn->press_duration_seconds == 3) 
            {
                long_press = true;
                if (energyNodeStatus == STATUS_ALARM)
                {
                    LOG_WARN("Button pressed for 3s: RESTART\n");
                    restart();
                }
                else
                {
                    LOG_WARN("Button pressed for 3s: ALARM\n");
                    alarm_handler();
                }
            }
        }
#endif /* PLATFORM_HAS_BUTTON */
    }

    PROCESS_END();
}

