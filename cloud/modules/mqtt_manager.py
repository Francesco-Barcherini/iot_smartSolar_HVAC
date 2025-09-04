# create a mosquitto client
import paho.mqtt.client as mqtt
import config.app_config as conf
import os

def on_connect(client, userdata, flags, rc):
    print("Connected to mosquitto with result code " + str(rc))

def on_subscribe(client, userdata, mid, granted_qos):
    print("Subscribed with mid: " + str(mid))

def on_publish(client, userdata, mid):
    print("Message published with mid: " + str(mid))

def check_mosquitto_status():
    print('Checking Mosquitto broker status...')
    status = os.system('systemctl is-active --quiet mosquitto.service')
    if status != 0:
        print('Mosquitto broker is not active. Restarting...')
        os.system('sudo systemctl restart mosquitto.service')
    else:
        print('Mosquitto broker is already active.')
    print('Mosquitto broker is running.')

def get_mqtt_client():
    check_mosquitto_status()
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_publish = on_publish
    client.connect(conf.MQTT_BROKER_IP, conf.MQTT_BROKER_PORT, conf.MQTT_KEEPALIVE)
    client.loop_start()  # Start the loop to process network traffic and callbacks
    return client