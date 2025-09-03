#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "contiki.h"
#include "sys/log.h"
#include "coap-callback-api.h"
#include "coap.h"

#include "os/dev/button-hal.h"
#include "os/dev/leds.h"

#include "coap-engine.h"

/* Log configuration */
#define LOG_MODULE "ENERGY"
#define LOG_LEVEL LOG_LEVEL_APP

/* COAP hvac-node URL */
#ifdef COOJA
    #define HVAC_NODE_EP "coap://[fd00::203:3:3:3]:5683"
#else /*NRF52840*/
    #define HVAC_NODE_EP "coap://[fd00::f6ce:3627:65f2:492f]:5683"
#endif

#define SETTINGS_URI "/settings"

// Publish intervals
#define LONG_INTERVAL CLOCK_SECOND * 15
#define SHORT_INTERVAL CLOCK_SECOND * 7
#define ANTIDUST_INTERVAL CLOCK_SECOND * 5
#define BLINK_INTERVAL CLOCK_SECOND * 0.1

// Power parameters
#define MAX_POWER 3000.0 // in W
#define MAX_OFFSET_PREDICTION 0.1 * MAX_POWER
#define WRONG_PREDICTIONS_THRESHOLD_DUST 2
#define WRONG_PREDICTIONS_THRESHOLD_ALARM 4

// Resources
extern coap_resource_t res_weather, res_battery, res_gen_power, res_relay, res_antiDust;

//extern variables and functions
enum antiDust_t {ANTIDUST_OFF, ANTIDUST_ON, ANTIDUST_ALARM};
enum relay_sp_t { RELAY_SP_HOME, RELAY_SP_BATTERY, RELAY_SP_GRID };
enum relay_home_t { RELAY_HOME_SP, RELAY_HOME_BATTERY, RELAY_HOME_GRID };
extern float gen_power; // Generated power in W
extern bool defected; // true if the solar panel is defected
float solar_power_predict();
void update_antiDust(enum antiDust_t newState);
void updateChargeRate(float rate);
void updateBatteryChargeRate();
void update_relay(enum relay_sp_t new_relay_sp, enum relay_home_t new_relay_home, float new_power_sp, float new_power_home);
extern float charge_rate;
extern enum relay_sp_t relay_sp;
extern float power_sp;

// Status
enum status_t {STATUS_ON, STATUS_ANTIDUST, STATUS_ALARM};
enum status_t energyNodeStatus = STATUS_ON;

static unsigned int nWrongPredictions = 0; // Counter for wrong predictions

struct etimer blink_timer;
struct etimer sleep_timer;

// CoAP observation
coap_endpoint_t hvac_node_endpoint;

char* str(float value, char* output)
{
    int integer = (int) value;
    float fraction = value - integer;
    fraction = fraction < 0 ? -fraction : fraction; // abs
    int fraction_int = (int)(fraction * 100);
    snprintf(output, 16, "%d.%d", integer, fraction_int);
    return output;
}

// Process
PROCESS(energy_node_process, "Energy Node Process");
AUTOSTART_PROCESSES(&energy_node_process);

static void alarm_handler()
{
    energyNodeStatus = STATUS_ALARM;
    update_antiDust(ANTIDUST_ALARM); // Disable anti-dust mode
    
    res_gen_power.trigger();

    // relay control
    update_relay(RELAY_SP_GRID, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();

    res_antiDust.trigger();
}

static void restart()
{
    energyNodeStatus = STATUS_ON;
    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode
    
    res_gen_power.trigger();

    // relay control
    update_relay(RELAY_SP_GRID, RELAY_HOME_GRID, gen_power, -1.0);
    res_relay.trigger();

    res_antiDust.trigger();
}

static void antidust_handler()
{
    energyNodeStatus = STATUS_ANTIDUST;
    update_antiDust(ANTIDUST_ON); // Enable anti-dust mode

#if PLATFORM_HAS_LEDS || LEDS_COUNT
    etimer_set(&blink_timer, BLINK_INTERVAL);
#endif

    res_gen_power.trigger();

    // relay control
    update_relay(RELAY_SP_BATTERY, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();

    res_antiDust.trigger();
}

static void end_antidust_handler()
{
    energyNodeStatus = STATUS_ON;
    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode

#if PLATFORM_HAS_LEDS || LEDS_COUNT
    etimer_stop(&blink_timer);
    if (defected)
        leds_single_on(LEDS_YELLOW); // Indicate defected mode
    else
        leds_single_off(LEDS_YELLOW); // Turn off yellow LED
#endif

    res_gen_power.trigger();

    // relay control
    update_relay(RELAY_SP_GRID, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();

    res_antiDust.trigger();
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

static coap_callback_request_state_t req_state;

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

    // Initialize resources
    coap_activate_resource(&res_weather, "sensors/weather");
    coap_activate_resource(&res_battery, "sensors/battery");
    coap_activate_resource(&res_gen_power, "sensors/power");
    coap_activate_resource(&res_relay, "relay");
    coap_activate_resource(&res_antiDust, "antiDust");

    // Initialize CoAP endpoint
    coap_endpoint_parse(HVAC_NODE_EP, strlen(HVAC_NODE_EP), &hvac_node_endpoint);

    // Wait connection
    while (!coap_endpoint_is_connected(&hvac_node_endpoint)) {
        LOG_DBG("Waiting for connection to HVAC node...\n");
        etimer_set(&sleep_timer, CLOCK_SECOND * 0.5);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&sleep_timer));
        #if PLATFORM_HAS_LEDS || LEDS_COUNT
            leds_toggle(LEDS_GREEN);
        #endif 
    }
    etimer_stop(&sleep_timer);
    LOG_DBG("Connected to HVAC node\n");
#if PLATFORM_HAS_LEDS || LEDS_COUNT
    leds_on(LEDS_GREEN);
#endif

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
                if (charge_rate != 0.0)
                    res_battery.trigger(); // Trigger battery only if charge rate is set
                etimer_reset(&weather_battery_timer);
            }
            else if (data == &gen_power_timer) {
                // Trigger power generation resource
                if (energyNodeStatus == STATUS_ON) {
                    res_gen_power.trigger();
                    updateBatteryChargeRate();
                    if (relay_sp == RELAY_SP_BATTERY || relay_sp == RELAY_SP_GRID)
                        power_sp = gen_power;
                }
                    
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
                        LOG_DBG("Anti-dust mode active, will end in %d seconds\n", (int) (ANTIDUST_INTERVAL / CLOCK_SECOND));
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
            if(btn->press_duration_seconds == 2) 
            {
                long_press = true;
                if (energyNodeStatus == STATUS_ALARM)
                {
                    LOG_WARN("Button pressed for 2s: RESTART\n");
                    restart();
                }
                else
                {
                    LOG_WARN("Button pressed for 2s: ALARM\n");
                    alarm_handler();
                }
            }
        }
#endif /* PLATFORM_HAS_BUTTON */
    }

    PROCESS_END();
}

