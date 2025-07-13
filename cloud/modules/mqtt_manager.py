# create a mosquitto client
import paho.mqtt.client as mqtt
import config.app_config as conf

def on_connect(client, userdata, flags, rc):
    print("Connected to mosquitto with result code " + str(rc))

def on_subscribe(client, userdata, mid, granted_qos):
    print("Subscribed with mid: " + str(mid) + ", granted QoS: " + str(granted_qos))

def on_publish(client, userdata, mid):
    print("Message published with mid: " + str(mid))

def get_mqtt_client():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_publish = on_publish
    client.connect(conf.MQTT_BROKER_IP, conf.MQTT_BROKER_PORT, conf.MQTT_KEEPALIVE)
    client.loop_start()  # Start the loop to process network traffic and callbacks
    return client   