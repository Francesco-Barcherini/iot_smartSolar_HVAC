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
colorRoomTemp = RESET
stop_get_all = False

def print_get_all(data):
    global stop_get_all
    if stop_get_all:
        return
    print("\033[H\033[J", end='') # clear

    grid_balance = float(data['Grid power balance'])
    grid_balance_eur = 0.15 * grid_balance
    grid_color = BRIGHT_GREEN if grid_balance > 0 else BRIGHT_RED if grid_balance < 0 else RESET
    print("STATS:")
    print(f"  Grid power balance (1h): {round(grid_balance,2)}Wh ({grid_color}{round(grid_balance_eur,2)}€{RESET})")
    print(f"  HVAC consumption (1h): {round(float(data['HVAC consumption (1h)']),2)}Wh")
    print(f"  Last antidust operation: {data['Last antiDust operation']}")

    status = status_map[data['settings']['status']]
    mode = mode_map[data['settings']['mode']]
    BACK = BACKCOLOR_HVAC[status]
    green = f"{BRIGHT_GREEN} green{RESET}" if mode == "green" and status != "off" and status != "error" else ""
    targetTemp = round(float(data['settings']['targetTemp']), 2)
    hvacPower = round(float(data['settings']['pw']), 2)
    backPower = RED if hvacPower > 0 else RESET
    outTemp = round(float(data['weather']['outTemp']), 2)
    print(f"{BACK}HVAC {status}{RESET} {green}")
    print(f"  Power consumption: {backPower}{hvacPower}W{RESET}")
    print(f"  Target Temperature: {targetTemp}°C \t Outside Temperature: {outTemp}°C")

    roomTemp = round(float(data['roomTemp']['v']), 2)
    global lastRoomTemp
    global colorRoomTemp
    if abs(roomTemp - lastRoomTemp) > 0.0:
        colorRoomTemp = BRIGHT_MAGENTA if roomTemp > lastRoomTemp else BRIGHT_CYAN
        lastRoomTemp = roomTemp
    print(f"ROOM:    {colorRoomTemp}{roomTemp}°C{RESET}")

    antidust = f"{GREEN}antidust on{RESET}" if data['antiDust']['v'] == 1 else \
                f"{BACKGROUND_BRIGHT_RED}antidust alarm{RESET}" if data['antiDust']['v'] == 2 else ""
    modTemp = round(float(data['weather']['modTemp']), 2)
    irradiation = round(float(data['weather']['irr']), 2)
    genPower = round(float(data['gen_power']['v']), 2)
    print(f"SOLAR PANEL {antidust}")
    print(f"  Module Temperature: {modTemp}°C \t Irradiation: {irradiation}kW/m²")
    print(f"  Generated Power: {genPower}W")

    relay_sp = data['relay']['r_sp']
    relay_h = data['relay']['r_h']
    power_sp = round(float(data['relay']['p_sp']), 2)
    power_h = round(float(data['relay']['p_h']), 2)
    batteryValue = round(float(data['battery']['v']), 2)
    batteryRate = power_sp * (1 if relay_sp == 1 else 0) - power_h * (1 if relay_h == 1 else 0)
    batteryColor = GREEN if batteryRate > 0 else \
                    RED if batteryRate < 0 else \
                    RESET
    print("POWER SUPPLY:")
    print(f"  Solar Panel--{power_sp}W-->{SP[relay_sp]}")
    print(f"  HVAC<--{power_h}W--{H[relay_h]}")
    print(f"  Battery: {batteryColor}{batteryValue}Wh{RESET}")


# ========== PERIODIC GET /all ==========
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
        time.sleep(3)

# ========== CLI MENU ==========
def send_post(endpoint, json_data):
    try:
        r = requests.post(f"{conf.HTTP_SERVER}/{endpoint}", json=json_data)
        print(f"[RESPONSE] Status: {r.status_code}, {r.text}")
    except Exception as e:
        print(f"[ERROR] POST to /{endpoint}: {e}")

def menu():
    global stop_get_all
    while True:
        if not stop_get_all: # no print after command execution
            print("\nChoose an operation:")
            print("0. Help - print this menu")
            print("1. Set HVAC status")
            print("2. Set mode (normal|green)")
            print("3. Set target temperature")
            print("4. Set antidust state")
            print("5. Set HVAC settings [CRUD]")
            print("6. Dump system state [CRUD]")
            print("7. Exit")
        stop_get_all = False
        choice = input("> ")

        if choice == "0":
            continue
        elif choice == "1":
            stop_get_all = True
            try:
                status = input("Status (off|vent|cool|heat): ")
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
                v = input("antiDust value (on|off): ")
                if v not in ["on", "off"]:
                    raise ValueError("Invalid input")
                send_post("antiDust", {"n": "antiDust", "v": v})
            except ValueError:
                print("Invalid input.")
        elif choice == "5":
            stop_get_all = True
            try:
                pw = float(input("Consumed power: "))
                status = input("Status (off|vent|cool|heat): ")
                mode = input("Mode (normal|green): ")
                target_temp = float(input("Target Temperature: "))
                payload = {"n": "settings", "pw": pw, "status": status, "mode": mode, "targetTemp": target_temp}
                send_post("settings", payload)
            except ValueError:
                print("Invalid input.")
        elif choice == "6":
            stop_get_all = True
            try:
                r = requests.get(f"{conf.HTTP_SERVER}/all")
                print("[MANUAL /all] Response:")
                print(json.dumps(r.json(), indent=2))
            except Exception as e:
                print(f"[ERROR] Could not GET /all: {e}")
        elif choice == "7":
            stop_get_all = True
            print("Exiting...")
            break
        else:
            print("Invalid choice.")

# ========== MAIN ==========

def handle_mqtt_message(client, userdata, message):
    try:
        payload = message.payload.decode()
        print(f"{BACKGROUND_BRIGHT_RED}\n[SIGNAL RECEIVED] {message.topic}={payload}{RESET}")
    except json.JSONDecodeError as e:
        print(f"[ERROR] Failed to decode JSON from MQTT message: {e}")
    except Exception as e:
        print(f"[ERROR] Failed to process MQTT message: {e}")
        return

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
