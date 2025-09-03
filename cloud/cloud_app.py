'''
Code for the cloud app that contains all the modules.

The cloud app:
- answers to user_app commands (he is a server HTTP)
- intercept CoAP observations from the sensors and store them in the DB -> use coapthon
- sets the actuator reading from data in the DB
- sends data and alarms into an MQTT broker self hosted
- provides a REST API to the user_app to get data from the DB and to interact with the system
'''

import sys
import os
import json
import threading
from coapthon.client.helperclient import HelperClient
from flask import Flask, request, jsonify
from datetime import datetime, timedelta

import mysql.connector

from modules.db_manager import HVAC_DB
from modules.mqtt_manager import get_mqtt_client
import config.app_config as conf
from modules.colors import *

VENT_POWER = 50.0
DELTAT_COEFF = 0.0005
POWER_COEFF = 0.00005
SECONDS = 60.0
BATTERY_INTERVAL = 10.0/3600.0 # hours
DC_AC_COEFF = 10.0
BATTERY_CAPACITY = 10000.0  # in Wh

GET_TIME = 3 # seconds

last_response = {}

mq_client = None

def normal_feedback_logic():
    print("Normal feedback logic")
    settings = HVAC_DB.get_last_entries("HVAC", 1)[0]
    #hvac_pw = settings[1]
    hvac_status = settings[2]
    hvac_mode = settings[3]
    target_temp = settings[4]
    
    needed_power = VENT_POWER
    if hvac_status != 1:
        roomTemp = HVAC_DB.get_last_sensor_entries("roomTemp", 1)[0][2]
        outTemp = HVAC_DB.get_last_sensor_entries("outTemp", 1)[0][2]
        if hvac_status == 2 and roomTemp <= target_temp \
            or hvac_status == 3 and roomTemp >= target_temp:
            print("Target temperature reached, HVAC suspended")
            needed_power = 0.0
        else:
            needed_power = (0.3 * (target_temp - roomTemp) / SECONDS) - (outTemp - roomTemp) * DELTAT_COEFF
            needed_power /= POWER_COEFF
            needed_power = -needed_power if hvac_status == 2 else needed_power
            needed_power = max(needed_power, 0.0)
            needed_power = round(needed_power, 2) # round to 10e-2
    print(f"needed_power: {needed_power} W")

    gen_power = HVAC_DB.get_last_sensor_entries("gen_power", 1)[0][2]
    battery_power = HVAC_DB.get_last_sensor_entries("battery", 1)[0][2]
    print(f"gen_power: {gen_power} W, battery_power: {battery_power} Wh")
    if needed_power > 0.0 and needed_power <= gen_power:
        print("Using solar power")
        HVAC_DB.insert_relay_data(0, 0, needed_power, needed_power)
        HVAC_DB.insert_hvac_data(needed_power, hvac_status, hvac_mode, target_temp)
        energy_payload = f'r_sp=0&r_h=0&p_sp={needed_power}&p_h={needed_power}'
        hvac_payload = f'pw={needed_power}&status=same&mode=same&targetTemp=-1.0'
        client_energy.put(conf.RELAY_URL, energy_payload)
        client_hvac.put(conf.SETTINGS_URL, hvac_payload)
    else:
        # try battery
        dc_needed_power = needed_power * DC_AC_COEFF
        rel_sp = 1 if battery_power < 0.9 * BATTERY_CAPACITY else 2
        if needed_power == 0.0 or dc_needed_power * BATTERY_INTERVAL <= battery_power - 20.0:
            print("Using battery power")
            HVAC_DB.insert_relay_data(rel_sp, 1, gen_power, needed_power)
            HVAC_DB.insert_hvac_data(needed_power, hvac_status, hvac_mode, target_temp)
            energy_payload = f'r_sp={rel_sp}&r_h=1&p_sp={gen_power}&p_h={needed_power}'
            hvac_payload = f'pw={needed_power}&status=same&mode=normal&targetTemp=-1.0'
            client_energy.put(conf.RELAY_URL, energy_payload)
            client_hvac.put(conf.SETTINGS_URL, hvac_payload)
        else:
            # attach to grid
            print("Using grid power")
            HVAC_DB.insert_relay_data(rel_sp, 2, gen_power, needed_power)
            HVAC_DB.insert_hvac_data(needed_power, hvac_status, hvac_mode, target_temp)
            energy_payload = f'r_sp={rel_sp}&r_h=2&p_sp={gen_power}&p_h={needed_power}'
            hvac_payload = f'pw={needed_power}&status=same&mode=normal&targetTemp=-1.0'
            client_energy.put(conf.RELAY_URL, energy_payload)
            client_hvac.put(conf.SETTINGS_URL, hvac_payload)

def handle_energy_with_hvac_down(is_gen_power):
    relays = HVAC_DB.get_last_entries("Relay", 1)[0]
    rel_sp = relays[1]
    rel_h = relays[2]
    p_h = float(relays[4])
    gen_power = HVAC_DB.get_last_sensor_entries("gen_power", 1)[0][2]
    battery_power = HVAC_DB.get_last_sensor_entries("battery", 1)[0][2]
    new_rel_sp = 1 if battery_power < 0.9 * BATTERY_CAPACITY else 2
    if not is_gen_power and rel_sp == new_rel_sp and rel_h == 2 and p_h == 0.0:
        print("No change in relay state needed")
        return
    rel_sp = new_rel_sp 
    HVAC_DB.insert_relay_data(rel_sp, 2, gen_power, 0.0)
    energy_payload = f'r_sp={rel_sp}&r_h=2&p_sp={gen_power}&p_h=0.0'
    client_energy.put(conf.RELAY_URL, energy_payload)

def remote_control_logic(component):
    settings = HVAC_DB.get_last_entries("HVAC", 1)[0]
    #hvac_pw = settings[1]
    hvac_status = settings[2]
    hvac_mode = settings[3]
    target_temp = settings[4]
    energy_antiDust = HVAC_DB.get_last_entries("AntiDust", 1)[0][1]
    if component in ["battery", "gen_power", "roomTemp", "settings", "relay"]:
        if component != "roomTemp" and (hvac_status == 0 or hvac_status == 4): # hvac off
            handle_energy_with_hvac_down(component == "gen_power")
            return
        if hvac_status not in [0,4] and hvac_mode == 0: # hvac on and normal mode
            normal_feedback_logic()
        # if error or green -> do nothing

def notification_callback(url, response):
    #print(f"Notification received from {url}")
    if response is None:
        print(f"No response received for {url}")
        return
    global last_response
    last_response[url] = response
    payload_raw = None
    try:
        global mq_client
        # CoAPthon's response.payload can be bytes, decode it to string for JSON parsing

        payload_raw = response.payload.decode('utf-8') if isinstance(response.payload, bytes) else response.payload
        #print(f"Raw payload: '{payload_raw}'")
        if not payload_raw:
            print(f"Empty payload received from {url}. Skipping processing.")
            return

        payload = json.loads(payload_raw)

        # Determine the actual type of data based on the 'n' field in the payload
        data_type = payload.get("n")
        if not data_type:
            raise ValueError("Payload missing 'n' field (data type identifier). Cannot process.")

        if data_type == "weather":
            # Process weather data: "{\"n\":\"weather\",\"irr\":%s,\"outTemp\":%s,\"modTemp\":%s}"
            if "n" in payload and payload["n"] == "weather" \
            and "irr" in payload and "outTemp" in payload and "modTemp" in payload:
                    for key in ["irr", "outTemp", "modTemp"]:
                        if not isinstance(payload[key], (int, float)):
                            raise ValueError(f"Invalid value for {key}: {payload[key]}")
                        HVAC_DB.insert_sensor_data(key, payload[key])
            else:
                raise ValueError("Invalid weather data format")
            remote_control_logic(data_type)
        elif data_type in ["battery", "gen_power", "roomTemp"]:
            # Process other sensor data
            if "n" in payload and payload["n"] == data_type and "v" in payload:
                HVAC_DB.insert_sensor_data(data_type, payload["v"])
            else:
                raise ValueError(f"Invalid data format for {data_type}")
            remote_control_logic(data_type)
                
        elif data_type == "relay":
            # Process relay data: "{\"n\":\"relay\",\"r_sp\":%d,\"r_h\":%d,\"p_sp\":%s,\"p_h\":%s}"
            if "n" in payload and payload["n"] == "relay" \
            and "r_sp" in payload and "r_h" in payload \
            and "p_sp" in payload and "p_h" in payload:
                HVAC_DB.insert_relay_data(
                    payload["r_sp"], 
                    payload["r_h"], 
                    payload["p_sp"], 
                    payload["p_h"]
                )
            else:
                raise ValueError("Invalid relay data format")  
            # nothing to control    
            
        elif data_type == "antiDust":
            # Process anti-dust data: "{\"n\":\"antiDust\",\"v\":%d}",
            if "n" in payload and payload["n"] == "antiDust" and "v" in payload:
                HVAC_DB.insert_anti_dust_data(payload["v"])
                mq_client.publish("antiDust", payload["v"])
            else:
                raise ValueError("Invalid anti-dust data format")

        elif data_type == "settings":
            # Process HVAC data: "{\"n\":\"settings\",\"pw\":%s,\"status\":%d,\"mode\":%d,\"targetTemp\":%s}",
            if "n" in payload and payload["n"] == "settings" \
            and "pw" in payload and "status" in payload \
            and "mode" in payload and "targetTemp" in payload:
                HVAC_DB.insert_hvac_data(
                    payload["pw"], 
                    payload["status"], 
                    payload["mode"], 
                    payload["targetTemp"]
                )
                if payload["status"] == 4:
                    mq_client.publish("hvac", "error")
            else:
                raise ValueError("Invalid HVAC data format")
            remote_control_logic("settings")
        else:
            raise ValueError(f"Unknown URL: {url}")

    except mysql.connector.Error as e:
        print(f"Database error: {e}")
        #HVAC_DB.connect()  # Reconnect to the database
                
    except Exception as e:
        print(f"Error processing notification: {url}, {e}")

def start_observation(client, url):
    try:
        client.observe(
            url, 
            callback=lambda resp: notification_callback(url, resp))
        print(f"Started observation on {url}")
    except Exception as e:
        print(f"Error starting observation on {url}: {e}")

def stop_observation(client, url):
    try:
        global last_response
        if url in last_response and last_response[url]:
            client.cancel_observing(last_response[url], False)
        print(f"Stopped observation on {url}")
    except Exception as e:
        print(f"Error stopping observation on {url}: {e}")

def start_all_observations():
    print("Starting observations for all sensors...")
    # Start observations
    start_observation(client_energy_WEATHER, conf.WEATHER_URL)
    start_observation(client_energy_BATTERY, conf.BATTERY_URL)
    start_observation(client_energy_GEN_POWER, conf.GEN_POWER_URL)
    start_observation(client_energy_RELAY, conf.RELAY_URL)
    start_observation(client_energy_ANTI_DUST, conf.ANTI_DUST_URL)
    start_observation(client_hvac_ROOM_TEMP, conf.ROOM_TEMP_URL)
    start_observation(client_hvac_SETTINGS, conf.SETTINGS_URL)
    print("All observations started successfully.")

def stop_all_observations():
    print("Stopping observations for all sensors...")
    # Stop observations
    stop_observation(client_energy_WEATHER, conf.WEATHER_URL)
    stop_observation(client_energy_BATTERY, conf.BATTERY_URL)
    stop_observation(client_energy_GEN_POWER, conf.GEN_POWER_URL)
    stop_observation(client_energy_RELAY, conf.RELAY_URL)
    stop_observation(client_energy_ANTI_DUST, conf.ANTI_DUST_URL)
    stop_observation(client_hvac_ROOM_TEMP, conf.ROOM_TEMP_URL)
    stop_observation(client_hvac_SETTINGS, conf.SETTINGS_URL)
    print("All observations stopped successfully.")

app = Flask(__name__)

def get_weather():
    irr = HVAC_DB.get_last_sensor_entries("irr", 1)[0]
    outTemp = HVAC_DB.get_last_sensor_entries("outTemp", 1)[0]
    modTemp = HVAC_DB.get_last_sensor_entries("modTemp", 1)[0]
    return {
        "irr": irr[2],
        "outTemp": outTemp[2],
        "modTemp": modTemp[2]
    }

def get_v(key):
    data = HVAC_DB.get_last_sensor_entries(key, 1)[0] if key != "antiDust" else HVAC_DB.get_last_entries("AntiDust", 1)[0]
    time_idx = 3 if key != "antiDust" else 2
    if key == "battery" and datetime.now() - data[time_idx] > timedelta(seconds=GET_TIME):
        url_map = {
            "battery": conf.BATTERY_URL,
            "gen_power": conf.GEN_POWER_URL,
            "roomTemp": conf.ROOM_TEMP_URL,
            "antiDust": conf.ANTI_DUST_URL
        }
        response = client_energy.get(url_map[key]) if key != "roomTemp" else client_hvac.get(url_map[key])
        data = json.loads(response.payload) if response.payload else {}
        return data
    return {"v": data[2]} if key != "antiDust" else {"v": data[1]}

def get_relay():
    relay = HVAC_DB.get_last_entries("Relay", 1)[0]
    return {
        "r_sp": relay[1],
        "r_h": relay[2],
        "p_sp": relay[3],
        "p_h": relay[4],
        "timestamp": relay[5]
    }

def get_settings():
    settings = HVAC_DB.get_last_entries("HVAC", 1)[0]
    return {
        "pw": settings[1],
        "status": settings[2],
        "mode": settings[3],
        "targetTemp": settings[4],
        "timestamp": settings[5]
    }


@app.route("/all", methods=["GET"])
def get_all_data():
    try:
        data = {}
        # Get weather data from DB / CoAP GET request        
        data["weather"] = get_weather()    
        # Get battery data from DB / CoAP GET request
        data["battery"] = get_v("battery")    
        # Get gen_power data from DB / CoAP GET request
        data["gen_power"] = get_v("gen_power")    
        # Get relay data from DB / CoAP GET request
        data["relay"] = get_relay()    
        # Get antiDust data from DB / CoAP GET request
        data["antiDust"] = get_v("antiDust")    
        # Get roomTemp data from DB / CoAP GET request
        data["roomTemp"] = get_v("roomTemp")    
        # Get settings data from DB / CoAP GET request
        data["settings"] = get_settings()    

        try:
            # Get stats from the DB
            # Total HVAC power consumption of the last hour
            total_power = HVAC_DB.get_total_hvac_power_consumption(3600)
            #add actual power contribution
            actual_power = float(data["settings"]["pw"])
            total_power += actual_power * (datetime.now() - data["settings"]["timestamp"]).total_seconds() / 3600.0
            data["HVAC consumption (1h)"] = total_power        

            # Net balance of the last hour of energy sent to the grid
            net_balance = HVAC_DB.get_net_balance(3600)
            actual_relay = data["relay"]
            actual_rate = float(actual_relay["p_sp"]) if actual_relay["r_sp"] == 2 else 0.0
            actual_rate -= float(actual_relay["p_h"]) if actual_relay["r_h"] == 2 else 0.0
            net_balance += actual_rate * (datetime.now() - actual_relay["timestamp"]).total_seconds() / 3600.0
            data["Grid power balance"] = net_balance        

            # Last antiDust operation time
            last_anti_dust = HVAC_DB.get_last_anti_dust_operation_time()
            data["Last antiDust operation"] = last_anti_dust        
        except mysql.connector.Error as e:
            print(f"Database error: {e}")
            HVAC_DB.connect()

        return jsonify(data), 200
    except Exception as e:
        print(f"Error in /all endpoint: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/relay", methods=["POST"])
def set_relay():
    payload = request.get_json()
    try:
        # send the request to coap in key=value format
        coap_payload = f'r_sp={payload["r_sp"]}&r_h={payload["r_h"]}&p_sp={payload["p_sp"]}&p_h={payload["p_h"]}'
        client_energy.put(conf.RELAY_URL, coap_payload)
        last_relay_db = HVAC_DB.get_last_entries("Relay", 1)[0]
        HVAC_DB.insert_relay_data(
            payload["r_sp"],
            payload["r_h"],
            payload["p_sp"] if payload["p_sp"] != -1 else last_relay_db[3], 
            payload["p_h"] if payload["p_h"] != -1 else last_relay_db[4]
        )
        remote_control_logic("relay")
        return "Relay command accepted", 200

    except Exception as e:
        return f"Error: {e}", 400

@app.route("/antiDust", methods=["POST"])
def set_anti_dust():
    payload = request.get_json()
    try:
        coap_payload = f'antiDust={payload["v"]}'
        client_energy.put(conf.ANTI_DUST_URL, coap_payload)
    except Exception as e:
        return f"Error: {e}", 400
    
    energy_antiDust = HVAC_DB.get_last_entries("AntiDust", 1)[0][2]
    # if different update and store
    if energy_antiDust != int(payload["v"]):
        HVAC_DB.insert_anti_dust_data(int(payload["v"]))
        energy_antiDust = int(payload["v"])
        global mq_client
        mq_client.publish("antiDust", energy_antiDust)
    return "AntiDust command accepted", 200

STATUS_MAP = {
    "off": 0,
    "vent": 1,
    "cool": 2,
    "heat": 3,
    "error": 4
}

MODE_MAP = {
    "normal": 0,
    "green": 1
}

@app.route("/settings", methods=["POST"])
def set_settings():
    payload = request.get_json()
    try:
        payload["pw"] = 0.0 if payload["status"] == "off" else payload["pw"]
        coap_payload = f'pw={payload["pw"]}&status={payload["status"]}&mode={payload["mode"]}&targetTemp={payload["targetTemp"]}'
        client_hvac.put(conf.SETTINGS_URL, coap_payload)
    except Exception as e:
        return f"Error: {e}", 400

    settings = HVAC_DB.get_last_entries("HVAC", 1)[0]
    hvac_pw = settings[1]
    hvac_status = settings[2]
    hvac_mode = settings[3]
    target_temp = settings[4]

    hvac_status = hvac_status if payload["status"] == "same" else STATUS_MAP.get(payload["status"], 0)
    hvac_mode = hvac_mode if payload["mode"] == "same" else MODE_MAP.get(payload["mode"], 0)
    target_temp = target_temp if payload["targetTemp"] == -1 else float(payload["targetTemp"])
    hvac_pw = hvac_pw if payload["pw"] == -1 else float(payload["pw"])
    HVAC_DB.insert_hvac_data(
        hvac_pw,
        hvac_status, 
        hvac_mode, 
        target_temp
    )
    if hvac_status == 4:
        global mq_client
        mq_client.publish("hvac", "error")
    remote_control_logic("settings")
    return "Settings command accepted", 200
    
def flask_app():
    app.run(host=conf.HTTP_HOST, port=conf.HTTP_PORT, debug=True)

# Start the CoAP client wrt the sensors
print('Starting CoAP client...')
if '--cooja' in sys.argv:
    client_energy = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_hvac = HelperClient((conf.COOJA_HVAC_IP, conf.COAP_PORT), None, None, None)
    client_energy_WEATHER = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_BATTERY = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_GEN_POWER = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_RELAY = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_ANTI_DUST = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_hvac_ROOM_TEMP = HelperClient((conf.COOJA_HVAC_IP, conf.COAP_PORT), None, None, None)
    client_hvac_SETTINGS = HelperClient((conf.COOJA_HVAC_IP, conf.COAP_PORT), None, None, None)
else:
    client_energy = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_hvac = HelperClient((conf.DONGLE_HVAC_IP, conf.COAP_PORT), None, None, None)
    client_energy_WEATHER = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_BATTERY = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_GEN_POWER = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_RELAY = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_energy_ANTI_DUST = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_hvac_ROOM_TEMP = HelperClient((conf.DONGLE_HVAC_IP, conf.COAP_PORT), None, None, None)
    client_hvac_SETTINGS = HelperClient((conf.DONGLE_HVAC_IP, conf.COAP_PORT), None, None, None)

if __name__ == "__main__":
    # arguments: --new-db --cooja
    if '--new-db' in sys.argv:
        user_input = ''
        while (user_input != 'y' and user_input != 'n'):
            user_input = input('Are you sure you want to create a new DB?[y/n] ')
        
        if user_input == 'y':
            HVAC_DB.reset_db()
    
    if '--default' in sys.argv:
        HVAC_DB.insert_default()

# start a mosquitto broker
print('Checking Mosquitto broker status...')
status = os.system('systemctl is-active --quiet mosquitto.service')
if status != 0:
    print('Mosquitto broker is not active. Restarting...')
    os.system('sudo systemctl restart mosquitto.service')
else:
    print('Mosquitto broker is already active.')
mq_client = get_mqtt_client()
print('Mosquitto broker setup completed')

if __name__ == "__main__":    
    threading.Thread(target=start_all_observations, daemon=True).start()
    print('CoAP client started')

    # Wait for the user to stop the script
    try:
        while True:
            pass
    except KeyboardInterrupt:
        print("Stopping observations...")
        stop_all_observations()
        client_energy.stop()
        client_hvac.stop()
        print("CoAP client stopped")
        mq_client.disconnect()
        print("MQTT client disconnected")
        HVAC_DB.close()  # Close the database connection
        print("Database connection closed")
        print("Exiting...")