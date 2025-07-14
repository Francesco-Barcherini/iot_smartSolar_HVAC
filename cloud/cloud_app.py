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
from coapthon.utils import parse_uri
from flask import Flask, request, jsonify

import mysql.connector

from modules.db_manager import HVAC_DB
from modules.mqtt_manager import get_mqtt_client
import config.app_config as conf

VENT_POWER = 50.0
DELTAT_COEFF = 0.05
POWER_COEFF = 1 / 1000.0
SECONDS = 60.0
BATTERY_INTERVAL = 10.0
DC_AC_COEFF = 10.0
BATTERY_CAPACITY = 10000.0  # in Wh

energy_antiDust = 0 #off, on, alarm

hvac_status = 0 # off, vent, cool, heat, error
hvac_mode = 0 # normal, green

target_temp = 22.0 # default target temperature

def remote_control_logic(component):
    if component in ["battery", "gen_power", "roomTemp"]:
        if energy_antiDust == 0 and hvac_status not in [0,4] and hvac_mode == 0:
            needed_power = VENT_POWER
            if hvac_status != 1:
                roomTemp = HVAC_DB.get_last_sensor_entries("roomTemp", 1)[0][2]
                outTemp = HVAC_DB.get_last_sensor_entries("outTemp", 1)[0][2]
                needed_power = (0.2 * (target_temp - roomTemp) / SECONDS) - (outTemp - roomTemp) * DELTAT_COEFF
                needed_power /= POWER_COEFF
                needed_power = -needed_power if hvac_status == 2 else needed_power
                needed_power = max(needed_power, 0.0)
            print(f"needed_power: {needed_power} W")

            gen_power = HVAC_DB.get_last_sensor_entries("gen_power", 1)[0][2]
            battery_power = HVAC_DB.get_last_sensor_entries("battery", 1)[0][2]
            if needed_power <= gen_power:
                print("Using solar power")
                HVAC_DB.insert_relay_data(0, 0, needed_power, needed_power)
                HVAC_DB.insert_hvac_data(needed_power, hvac_status, hvac_mode, target_temp)
                energy_payload = f'r_sp=0&r_h=0&p_sp={needed_power}&p_h={needed_power}'
                hvac_payload = f'pw=0&status=same&mode=normal&targetTemp=-1.0'
                client_energy.put(conf.RELAY_URL, energy_payload)
                client_hvac.put(conf.SETTINGS_URL, hvac_payload)
            else:
                # try battery
                dc_needed_power = needed_power * DC_AC_COEFF
                rel_sp = 1 if battery_power < 0.9 * BATTERY_CAPACITY else 2
                if dc_needed_power * BATTERY_INTERVAL <= battery_power:
                    print("Using battery power")
                    HVAC_DB.insert_relay_data(rel_sp, 1, gen_power, dc_needed_power)
                    HVAC_DB.insert_hvac_data(needed_power, hvac_status, hvac_mode, target_temp)
                    energy_payload = f'r_sp={rel_sp}&r_h=1&p_sp={gen_power}&p_h={dc_needed_power}'
                    hvac_payload = f'pw=0&status=same&mode=normal&targetTemp=-1.0'
                    client_energy.put(conf.RELAY_URL, energy_payload)
                    client_hvac.put(conf.SETTINGS_URL, hvac_payload)
                else:
                    # attach to grid
                    print("Using grid power")
                    HVAC_DB.insert_relay_data(rel_sp, 2, gen_power, needed_power)
                    HVAC_DB.insert_hvac_data(needed_power, hvac_status, hvac_mode, target_temp)
                    energy_payload = f'r_sp={rel_sp}&r_h=2&p_sp={gen_power}&p_h={needed_power}'
                    hvac_payload = f'pw=0&status=same&mode=normal&targetTemp=-1.0'
                    client_energy.put(conf.RELAY_URL, energy_payload)
                    client_hvac.put(conf.SETTINGS_URL, hvac_payload)
        # if error or green -> do nothing
    elif component == "antiDust":
        if energy_antiDust == 1 or energy_antiDust == 2:
            if hvac_status == 0 or hvac_status == 4:
                print("AntiDust during inactivity")
                return
            # try the battery
            battery_power = HVAC_DB.get_last_sensor_entries("battery", 1)[0][2]
            cons_pw = HVAC_DB.get_last_entries("HVAC", 1)[0][2]
            dc_needed_power = cons_pw * DC_AC_COEFF
            if dc_needed_power * BATTERY_INTERVAL <= battery_power:
                print("Using battery power for AntiDust")
                HVAC_DB.insert_rely_data(0, 1, 0.0, dc_needed_power)
                HVAC_DB.insert_hvac_data(cons_pw, hvac_status, 0, target_temp)
                energy_payload = f'r_sp=0&r_h=1&p_sp=0.0&p_h={dc_needed_power}'
                hvac_payload = f'pw=0&status=same&mode=normal&targetTemp=-1.0'
                client_energy.put(conf.RELAY_URL, energy_payload)
                client_hvac.put(conf.SETTINGS_URL, hvac_payload)
            else:
                # attach to grid
                print("Using grid power for AntiDust")
                HVAC_DB.insert_relay_data(0, 2, 0.0, cons_pw)
                HVAC_DB.insert_hvac_data(cons_pw, hvac_status, 0, target_temp)
                energy_payload = f'r_sp=0&r_h=2&p_sp=0.0&p_h={cons_pw}'
                hvac_payload = f'pw=0&status=same&mode=normal&targetTemp=-1.0'
                client_energy.put(conf.RELAY_URL, energy_payload)
                client_hvac.put(conf.SETTINGS_URL, hvac_payload)
    elif component == "settings":
        if hvac_status == 4:
            print("HVAC in error mode, cannot control")
            # null relay_h
            battery = HVAC_DB.get_last_sensor_entries("battery", 1)[0][2]
            p_sp = HVAC_DB.get_last_entries("Relay", 1)[0][3]
            rel_sp = 1 if battery < 0.9 * BATTERY_CAPACITY else 2
            energy_payload = f'r_sp={rel_sp}&r_h=0&p_sp=-1.0&p_h=0.0'
            HVAC_DB.insert_relay_data(rel_sp, 0, p_sp, 0.0)
            client_energy.put(conf.RELAY_URL, energy_payload)


def notification_callback(url, response):
    #print(f"Notification received from {url}")
    if response is None:
        print(f"No response received for {url}")
        return
    payload_raw = None
    try:
        # CoAPthon's response.payload can be bytes, decode it to string for JSON parsing
        payload_raw = response.payload.decode('utf-8') if isinstance(response.payload, bytes) else response.payload
        #print(f"Raw payload: '{payload_raw}'")

        if not payload_raw:
            print(f"Empty payload received. Skipping processing.")
            return

        payload = json.loads(payload_raw)
        print(f"Parsed payload: {payload}")

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
                global energy_antiDust
                energy_antiDust = int(payload["v"])
            else:
                raise ValueError("Invalid anti-dust data format")
            remote_control_logic("antiDust")

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
                    mq_client.publish("hvac", "alarm")
                global hvac_status, hvac_mode, target_temp
                hvac_status = int(payload["status"])
                hvac_mode = int(payload["mode"])
                target_temp = float(payload["targetTemp"])
            else:
                raise ValueError("Invalid HVAC data format")
            remote_control_logic("settings")
        else:
            raise ValueError(f"Unknown URL: {url}")

    except mysql.connector.Error as e:
        print(f"Database error: {e}")
        HVAC_DB.connect()  # Reconnect to the database
                
    except Exception as e:
        print(f"Error processing notification: {e}")

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
        client.stop_observing(url)
        print(f"Stopped observation on {url}")
    except Exception as e:
        print(f"Error stopping observation on {url}: {e}")

def start_all_observations():
    print("Starting observations for all sensors...")
    # Start observations
    start_observation(client_energy, conf.WEATHER_URL)
    start_observation(client_energy, conf.BATTERY_URL)
    start_observation(client_energy, conf.GEN_POWER_URL)
    start_observation(client_energy, conf.RELAY_URL)
    start_observation(client_energy, conf.ANTI_DUST_URL)
    start_observation(client_hvac, conf.ROOM_TEMP_URL)
    start_observation(client_hvac, conf.SETTINGS_URL)
    print("All observations started successfully.")

def stop_all_observations():
    print("Stopping observations for all sensors...")
    # Stop observations
    stop_observation(client_energy, conf.WEATHER_URL)
    stop_observation(client_energy, conf.BATTERY_URL)
    stop_observation(client_energy, conf.GEN_POWER_URL)
    stop_observation(client_energy, conf.RELAY_URL)
    stop_observation(client_energy, conf.ANTI_DUST_URL)
    stop_observation(client_hvac, conf.ROOM_TEMP_URL)
    stop_observation(client_hvac, conf.SETTINGS_URL)
    print("All observations stopped successfully.")

app = Flask(__name__)

@app.route("/all", methods=["GET"])
def get_all_data():
    try:
        data = {}
        # Get weather data from CoAP GET request
        weather_response = client_energy.get(conf.WEATHER_URL)
        weather_data = json.loads(weather_response.payload) if weather_response.payload else {}
        data["weather"] = weather_data
        # Get battery data from CoAP GET request
        battery_response = client_energy.get(conf.BATTERY_URL)
        battery_data = json.loads(battery_response.payload) if battery_response.payload else {}
        data["battery"] = battery_data
        # Get gen_power data from CoAP GET request
        gen_power_response = client_energy.get(conf.GEN_POWER_URL)
        gen_power_data = json.loads(gen_power_response.payload) if gen_power_response.payload else {}
        data["gen_power"] = gen_power_data
        # Get relay data from CoAP GET request
        relay_response = client_energy.get(conf.RELAY_URL)
        relay_data = json.loads(relay_response.payload) if relay_response.payload else {}
        data["relay"] = relay_data
        # Get antiDust data from CoAP GET request
        anti_dust_response = client_energy.get(conf.ANTI_DUST_URL)
        anti_dust_data = json.loads(anti_dust_response.payload) if anti_dust_response.payload else {}
        data["antiDust"] = anti_dust_data
        # Get roomTemp data from CoAP GET request
        room_temp_response = client_hvac.get(conf.ROOM_TEMP_URL)
        room_temp_data = json.loads(room_temp_response.payload) if room_temp_response.payload else {}
        data["roomTemp"] = room_temp_data
        # Get settings data from CoAP GET request
        settings_response = client_hvac.get(conf.SETTINGS_URL)
        settings_data = json.loads(settings_response.payload) if settings_response.payload else {}
        data["settings"] = settings_data

        try:
            # Get stats from the DB
            # Total HVAC power consumption of the last hour
            total_power = HVAC_DB.get_total_hvac_power_consumption(3600)
            data["HVAC consumption (1h)"] = total_power

            # Net balance of the last hour of energy sent to the grid
            net_balance = HVAC_DB.get_net_balance(3600)
            data["Grid power balance"] = net_balance

            # Last antiDust operation time
            last_anti_dust = HVAC_DB.get_last_anti_dust_operation_time()
            print(f"Last antiDust operation time: {last_anti_dust}")
            data["Last antiDust operation"] = last_anti_dust
        except mysql.connector.Error as e:
            print(f"Database error: {e}")
            HVAC_DB.connect()

        return jsonify(data), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/relay", methods=["POST"])
def set_relay():
    payload = request.get_json()
    try:
        # send the request to coap in key=value format
        coap_payload = f'r_sp={payload["r_sp"]}&r_h={payload["r_h"]}&p_sp={payload["p_sp"]}&p_h={payload["p_h"]}'
        client_energy.put(conf.RELAY_URL, coap_payload)
        HVAC_DB.insert_relay_data(
            payload["r_sp"], 
            payload["r_h"], 
            payload["p_sp"], 
            payload["p_h"]
        )
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
    global energy_antiDust
    # if different update and store
    if energy_antiDust != int(payload["v"]):
        HVAC_DB.insert_anti_dust_data(int(payload["v"]))
        energy_antiDust = int(payload["v"])
        mq_client.publish("antiDust", energy_antiDust)
    return "AntiDust command accepted", 200

STATUS_MAP = {
    0: "off",
    1: "vent",
    2: "cool",
    3: "heat",
    4: "error"
}

MODE_MAP = {
    0: "normal",
    1: "green"
}

@app.route("/settings", methods=["POST"])
def set_settings():
    payload = request.get_json()
    try:
        coap_payload = f'pw={payload["pw"]}&status={payload["status"]}&mode={payload["mode"]}&targetTemp={payload["targetTemp"]}'
        client_hvac.put(conf.SETTINGS_URL, coap_payload)
    except Exception as e:
        return f"Error: {e}", 400

    global hvac_status, hvac_mode, target_temp
    hvac_status = STATUS_MAP.get(payload["status"], 0)
    hvac_mode = MODE_MAP.get(payload["mode"], 0)
    target_temp = float(payload["targetTemp"])
    HVAC_DB.insert_hvac_data(
        payload["pw"], 
        hvac_status, 
        hvac_mode, 
        target_temp
    )
    if hvac_status == 4:
        mq_client.publish("hvac", "alarm")
    return "Settings command accepted", 200
    
def flask_app():
    app.run(host=conf.HTTP_HOST, port=conf.HTTP_PORT, debug=True)

# Start the CoAP client wrt the sensors
print('Starting CoAP client...')
if '--cooja' in sys.argv:
    client_energy = HelperClient((conf.COOJA_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_hvac = HelperClient((conf.COOJA_HVAC_IP, conf.COAP_PORT), None, None, None)
else:
    client_energy = HelperClient((conf.DONGLE_ENERGY_IP, conf.COAP_PORT), None, None, None)
    client_hvac = HelperClient((conf.DONGLE_HVAC_IP, conf.COAP_PORT), None, None, None)

if __name__ == "__main__":
    # arguments: --new-db --cooja
    if '--new-db' in sys.argv:
        user_input = ''
        while (user_input != 'y' and user_input != 'n'):
            user_input = input('Are you sure you want to create a new DB?[y/n] ')
        
        if user_input == 'y':
            HVAC_DB.reset_db()

    # start a mosquitto broker
    print('Starting Mosquitto broker...')
    os.system('sudo systemctl restart mosquitto.service')
    print('Mosquitto broker restarted')
    mq_client = get_mqtt_client()
    
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
        sys.exit(0)