import threading
import requests
import json
import time
import paho.mqtt.client as mqtt

import config.app_config as conf
from modules.mqtt_manager import get_mqtt_client
from modules.colors import *

SP = {
    0: "HVAC",
    1: "Battery",
    2: "Grid"
}

H = {
    0: "Solar Panel",
    1: "Battery",
    2: "Grid"
}

BACKCOLOR_HVAC = {
    "off": BACKGROUND_BLACK,
    "vent": BACKGROUND_BRIGHT_CYAN,
    "cool": BACKGROUND_BRIGHT_BLUE,
    "heat": BACKGROUND_BRIGHT_MAGENTA,
    "error": BACKGROUND_BRIGHT_RED,
}

status_map = {
    0: "off",
    1: "vent",
    2: "cool",
    3: "heat",
    4: "error",
    5: "same"
}

mode_map = {
    0: "normal",
    1: "green"
}

lastRoomTemp = 27.0

def print_get_all(data):
    # clear
    print("\033[H\033[J", end='')

    print("STATS:")
    print(f"  Grid power balance: {data['Grid power balance']}Wh")
    print(f"  HVAC consumption (1h): {data['HVAC consumption (1h)']}Wh")
    print(f"  Last antidust operation: {data['Last antiDust operation']}")

    status = status_map[data['settings']['status']]
    mode = mode_map[data['settings']['mode']]
    BACK = BACKCOLOR_HVAC[status]
    green = f"{BRIGHT_GREEN} green{RESET}" if mode == "green" and status != "off" and status != "error" else ""
    targetTemp = data['settings']['targetTemp']
    hvacPower = data['settings']['pw']
    backPower = RED if hvacPower > 0 else RESET
    outTemp = data['weather']['outTemp']
    print(f"{BACK}HVAC {status}{RESET} {green}")
    print(f"  Power consumption: {backPower}{hvacPower}W{RESET}")
    print(f"  Target Temperature: {targetTemp}°C \t Outside Temperature: {outTemp}°C")

    roomTemp = data['roomTemp']['v']
    global lastRoomTemp
    colorRoomTemp = BRIGHT_MAGENTA if roomTemp > lastRoomTemp + 0.1 else \
                    BRIGHT_CYAN if roomTemp < lastRoomTemp - 0.1 else \
                    RESET
    lastRoomTemp = roomTemp if abs(roomTemp - lastRoomTemp) > 0.1 else lastRoomTemp
    print(f"ROOM:    {colorRoomTemp}{roomTemp}°C{RESET}")

    antidust = f"{GREEN} antidust on{RESET}" if data['antiDust']['v'] == 1 else ""
    modTemp = data['weather']['modTemp']
    irradiation = data['weather']['irr']
    genPower = data['gen_power']['v']
    print(f"SOLAR PANEL {antidust}")
    print(f"  Module Temperature: {modTemp}°C \t Irradiation: {irradiation}kW/m²")
    print(f"  Generated Power: {genPower}W")

    relay_sp = data['relay']['r_sp']
    relay_h = data['relay']['r_h']
    power_sp = data['relay']['p_sp']
    power_h = data['relay']['p_h']
    batteryValue = data['battery']['v']
    batteryColor = BRIGHT_GREEN if relay_sp == 1 else \
                    BRIGHT_MAGENTA if relay_h == 1 else \
                    RESET
    print("POWER SUPPLY:")
    print(f"  Solar Panel--{power_sp}W-->{SP[relay_sp]}")
    print(f"  HVAC<--{power_h}W--{H[relay_h]}")
    print(f"  Battery: {batteryColor}{batteryValue}Wh{RESET}")

    print(f"OUTSIDE: {outTemp}°C")



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
        time.sleep(5)

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
        print("0. Help - print this menu")
        print("1. Set HVAC status")
        print("2. Set mode (normal|green)")
        print("3. Set target temperature")
        print("4. Set relays state")
        print("5. Set antidust state")
        print("6. Set relay settings [CRUD]")
        print("7. Set HVAC settings [CRUD]")
        print("8. Dump system state [CRUD]")
        print("9. Exit")
        choice = input("> ")

        if choice == "0":
            continue
        elif choice == "1":
            stop_get_all = True
            try:
                status = input("Status (off|vent|cool|heat|error): ")
                payload = {"n": "settings", "pw": -1, "status": status, "mode": "same", "targetTemp": -1}
                send_post("settings", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "2":
            stop_get_all = True
            try:
                mode = input("Mode (normal|green): ")
                payload = {"n": "settings", "pw": -1, "status": "same", "mode": mode, "targetTemp": -1}
                send_post("settings", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "3":
            stop_get_all = True
            try:
                target_temp = float(input("Target Temperature: "))
                payload = {"n": "settings", "pw": -1, "status": "same", "mode": "same", "targetTemp": target_temp}
                send_post("settings", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "4":
            stop_get_all = True
            try:
                r_sp = int(input("Relay SP (0|1|2): "))
                r_h = int(input("Relay H (0|1|2): "))
                payload = {"n": "relay", "r_sp": r_sp, "r_h": r_h, "p_sp": -1, "p_h": -1}
                send_post("relay", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "5":
            stop_get_all = True
            try:
                v = input("antiDust value (on|off): ")
                send_post("antiDust", {"n": "antiDust", "v": v})
            except ValueError:
                print("Invalid input.")
        elif choice == "6":
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
        elif choice == "7":
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
        elif choice == "8":
            stop_get_all = True
            try:
                r = requests.get(f"{conf.HTTP_SERVER}/all")
                print("[MANUAL /all] Response:")
                print(json.dumps(r.json(), indent=2))
            except Exception as e:
                print(f"[ERROR] Could not GET /all: {e}")
        elif choice == "9":
            stop_get_all = True
            print("Exiting...")
            break
        else:
            print("Invalid choice.")

# ========== MAIN ==========

def handle_mqtt_message(client, userdata, message):
    try:
        payload = json.loads(message.payload.decode())
        print(f"{BACKGROUND_BRIGHT_RED}\n[SIGNAL RECEIVED] {message.topic}={payload}{RESET}")
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
