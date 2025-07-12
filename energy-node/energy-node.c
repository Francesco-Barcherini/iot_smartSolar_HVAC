#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "contiki.h"
#include "sys/log.h"

#include "os/dev/button-hal.h"
#include "os/dev/leds.h"

#include "coap-engine.h"

/* Log configuration */
#define LOG_MODULE "ENERGY"
#define LOG_LEVEL LOG_LEVEL_APP

// Publish intervals
#define LONG_INTERVAL CLOCK_SECOND * 10
#define SHORT_INTERVAL CLOCK_SECOND * 2
#define ANTIDUST_INTERVAL CLOCK_SECOND * 6

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

char* str(float value, char* output)
{
    int integer = (int) value;
    float fraction = value - integer;
    int fraction_int = (int)(fraction * 100);
    snprintf(output, 16, "%d.%d", integer, fraction_int);
    return output;
}

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
}

static void antidust_handler()
{
    energyNodeStatus = STATUS_ANTIDUST;

    update_antiDust(ANTIDUST_ON); // Enable anti-dust mode
    res_antiDust.trigger();

    res_gen_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_BATTERY, RELAY_HOME_GRID, 0.0, -1.0);
    res_relay.trigger();
}

static void end_antidust_handler()
{
    energyNodeStatus = STATUS_ON;

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

// Process
PROCESS(energy_node_process, "Energy Node Process");
AUTOSTART_PROCESSES(&energy_node_process);

PROCESS_THREAD(energy_node_process, ev, data) 
{
    static struct etimer weather_battery_timer;
    static struct etimer gen_power_timer;
    static struct etimer prediction_timer;

    static struct etimer end_antiDust_timer;

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
        }
        else if (ev == button_hal_periodic_event) {
            button_hal_button_t* btn = (button_hal_button_t*) data;
            if(btn->press_duration_seconds == 2) {
                // toggle defected
                defected = !defected;
                
                char mode[16] = defected ? "DUST" : "CLEAN";
                LOG_INFO("Toggled defected mode: %s\n", mode);
            } 
            // else if(btn->press_duration_seconds == 5) {
            //     // Trigger alarm
            //     energyNodeStatus = STATUS_ALARM;
            //     res_relay.trigger();
            //     LOG_INFO("Alarm triggered!\n");
            // }
        }
    }

    PROCESS_END();
}

