import threading
import requests
import json
import time
import paho.mqtt.client as mqtt

import config.app_config as conf
from modules.mqtt_manager import get_mqtt_client

SP = {
    0: "SP->HVAC",
    1: "SP->BATT",
    2: "SP->GRID"
}

H = {
    0: "HVAC<-SP",
    1: "HVAC<-BATT",
    2: "HVAC<-GRID"
}

def print_get_all(data):

    # Print the key and if has "v" its value
    for key, val in data.items():
        if isinstance(val, dict) and 'v' in val:
            print(f"  {key}: {val['v']}")
        else:
            if key == "relay":
                print(f"  {key}: {SP[val['r_sp']]} {val['p_sp']}W \t {H[val['r_h']]} {val['p_h']}W")
            elif key == "settings":
                status_map = {0: "off", 1: "vent", 2: "cool", 3: "heat", 4: "error", 5: "same"}
                mode_map = {0: "normal", 1: "green"}
                print(f"  {key}: {val['targetTemp']}°C \t {status_map[val['status']]} \t mode={mode_map[val['mode']]} \t "
                      f"pw={val['pw']}W")
            elif key == "weather":
                print(f"  {key}: {val['outTemp']}°C \t {val['modTemp']}°C \t {val['irr']}kW/m²")
            else:
                print(f"  {key}: {val}")



# ========== PERIODIC GET /all ==========
stop_get_all = False
def periodic_get_all():
    while True:
        global stop_get_all
        if stop_get_all:
            time.sleep(2)
            continue
        try:
            r = requests.get(f"{conf.HTTP_SERVER}/all")
            if r.status_code == 200:
                data = r.json()
                print("\n[DATA UPDATE] Current system status:")
                print_get_all(data)
                print("> ", end='', flush=True)
        except Exception as e:
            print(f"[ERROR] Could not fetch /all: {e}")
        time.sleep(5)  # every 15 seconds

# ========== CLI MENU ==========

def send_post(endpoint, json_data):
    try:
        r = requests.post(f"{conf.HTTP_SERVER}/{endpoint}", json=json_data)
        print(f"[RESPONSE] Status: {r.status_code}, Body: {r.text}")
    except Exception as e:
        print(f"[ERROR] POST to /{endpoint}: {e}")

def menu():
    global stop_get_all
    while True:
        stop_get_all = False
        print("\nChoose an operation:")
        print("0. help")
        print("1. Send /relay settings")
        print("2. Send /antiDust signal")
        print("3. Send /settings")
        print("4. Manually GET /all")
        print("5. Exit")
        choice = input("> ")

        if choice == "0":
            continue
        elif choice == "1":
            stop_get_all = True
            try:
                r_sp = int(input("Relay SP (0|1|2): "))
                r_h = int(input("Relay H (0|1|2): "))
                p_sp = float(input("Power SP: "))
                p_h = float(input("Power H: "))
                payload = {"n": "relay", "r_sp": r_sp, "r_h": r_h, "p_sp": p_sp, "p_h": p_h}
                send_post("relay", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "2":
            stop_get_all = True
            try:
                v = int(input("antiDust value (on|off): "))
                send_post("antiDust", {"n": "antiDust", "v": v})
            except ValueError:
                print("Invalid input.")
        elif choice == "3":
            stop_get_all = True
            try:
                pw = float(input("Consumed power: "))
                status = input("Status (off|vent|cool|heat|error|same): ")
                mode = input("Mode (normal|green): ")
                target_temp = float(input("Target Temperature: "))
                payload = {"n": "settings", "pw": pw, "status": status, "mode": mode, "targetTemp": target_temp}
                send_post("settings", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "4":
            stop_get_all = True
            try:
                r = requests.get(f"{conf.HTTP_SERVER}/all")
                print("[MANUAL /all] Response:")
                print(json.dumps(r.json(), indent=2))
            except Exception as e:
                print(f"[ERROR] Could not GET /all: {e}")
        elif choice == "5":
            stop_get_all = True
            print("Exiting...")
            break
        else:
            print("Invalid choice.")

# ========== MAIN ==========

def handle_mqtt_message(client, userdata, message):
    try:
        payload = json.loads(message.payload.decode())
        print(f"\n[MESSAGE RECEIVED] Topic: {message.topic}, Payload: {payload}")
    except Exception as e:
        print(f"[ERROR] Failed to process MQTT message: {e}")
        return
        

    except json.JSONDecodeError as e:
        print(f"[ERROR] Failed to decode JSON from MQTT message: {e}")

if __name__ == "__main__":
    print("Starting user_app...")
    
    # Start MQTT client
    mqtt_client = get_mqtt_client()
    mqtt_client.on_message = handle_mqtt_message
    mqtt_client.subscribe("antiDust")
    mqtt_client.subscribe("hvac")


    # Start background /all updater
    threading.Thread(target=periodic_get_all, daemon=True).start()

    # Start command menu
    menu()
