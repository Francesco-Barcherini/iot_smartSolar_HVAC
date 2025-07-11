#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contiki.h"
#include "sys/log.h"

#include "os/dev/button-hal.h"
#include "os/dev/leds.h"

#include "coap-engine.h"

#include "solar-power-model.h" // model weights

/* Log configuration */
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_APP

// Publish intervals
#define LONG_INTERVAL CLOCK_SECOND * 15
#define SHORT_INTERVAL CLOCK_SECOND * 1
#define ANTIDUST_INTERVAL CLOCK_SECOND * 6

// Power parameters
#define MAX_POWER 1500.0 // in W
#define MAX_OFFSET_PREDICTION 0.2 * MAX_POWER
#define WRONG_PREDICTIONS_THRESHOLD_DUST 5
#define WRONG_PREDICTIONS_THRESHOLD_ALARM 10

// Resources
extern coap_resource_t res_weather, res_battery, res_power, res_relay, res_antiDust;

//extern variables and functions
enum antiDust_t {ANTIDUST_OFF, ANTIDUST_ON};
enum relay_sp_t { RELAY_SP_HOME, RELAY_SP_BATTERY, RELAY_SP_GRID };
enum relay_home_t { RELAY_HOME_SP, RELAY_HOME_BATTERY, RELAY_HOME_GRID };
extern float gen_power; // Generated power in W
float solar_power_prediction();
void update_antiDust(antiDust_t newState);
void updateChargeRate(float rate);
void update_relay(relay_sp_t new_relay_sp, relay_home_t new_relay_home, float new_power_sp, float new_power_home);

// Status
enum status_t {STATUS_ON, STATUS_ANTIDUST, STATUS_ALARM};
status_t energyNodeStatus = STATUS_ON;

static unsigned int nWrongPredictions = 0; // Counter for wrong predictions

static void alarm_handler()
{
    energyNodeStatus = STATUS_ALARM;

    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode
    res_antiDust.trigger();
    
    res_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_BATTERY, RELAY_HOME_GRID, 0.0, NULL);
    res_relay.trigger();
}

static void antidust_handler()
{
    energyNodeStatus = STATUS_ANTIDUST;

    update_antiDust(ANTIDUST_ON); // Enable anti-dust mode
    res_antiDust.trigger();

    res_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_BATTERY, RELAY_HOME_GRID, 0.0, NULL);
    res_relay.trigger();
}

static void end_antidust_handler()
{
    energyNodeStatus = STATUS_ON;

    update_antiDust(ANTIDUST_OFF); // Disable anti-dust mode
    res_antiDust.trigger();

    res_power.trigger();

    updateChargeRate(0.0); // Stop charging

    // relay control
    update_relay(RELAY_SP_GRID, RELAY_HOME_GRID, 0.0, NULL);
    res_relay.trigger();
}

static void analyze_prediction(float prediction)
{
    if (gen_power >= prediction - MAX_OFFSET_PREDICTION)
    {
        nWrongPredictions = 0;
        return; // No change in status
    }

    LOG_WARN("Generated power is low: %.2f W, prediction: %.2f W\n", gen_power, prediction);
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

    // Initialize resources
    coap_activate_resource(&res_weather, "sensors/weather");
    coap_activate_resource(&res_battery, "sensors/battery");
    coap_activate_resource(&res_power, "sensors/power");
    coap_activate_resource(&res_relay, "relay");
    coap_activate_resource(&res_antiDust, "antiDust");

    // Initialize timers
    etimer_set(&weather_battery_timer, LONG_INTERVAL);
    etimer_set(&gen_power_timer, SHORT_INTERVAL);
    etimer_set(&prediction_timer, SHORT_INTERVAL);

    while(1) {
        PROCESS_WAIT_EVENT();

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
                    res_power.trigger();
                etimer_reset(&gen_power_timer);
            }
            else if (data == &prediction_timer) {
                if (energyNodeStatus == STATUS_ON)
                {
                    // Trigger prediction logic
                    float prediction = solar_power_predict();
                    LOG_DBG("Predicted solar power: %.2f W\n", prediction);
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
        // else if (ev == button_hal_periodic_event) {
        //     button_hal_button_t* btn = (button_hal_button_t*) data;
        //     if(btn->press_duration_seconds == 2) {
        //         // Toggle anti-dust mode
        //         energyNodeStatus = (energyNodeStatus == STATUS_ON) ? STATUS_ANTIDUST : STATUS_ON;
        //         res_antiDust.trigger();
        //         LOG_INFO("Toggled anti-dust mode: %s\n", energyNodeStatus == STATUS_ANTIDUST ? "STATUS_ON" : "OFF");
        //     } 
        //     else if(btn->press_duration_seconds == 5) {
        //         // Trigger alarm
        //         energyNodeStatus = STATUS_ALARM;
        //         res_relay.trigger();
        //         LOG_INFO("Alarm triggered!\n");
        //     }
        // }
    }

    PROCESS_END();
}

