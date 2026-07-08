#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "paho-mqtt",
#     "pyjson",
# ]
# ///

import paho.mqtt.client as mqtt
import json

def on_message(client, userdata, message):
    # print(json.loads(message.payload.decode()))
    print(message.payload)

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_message = on_message
client.connect("localhost", 1883, 60)
client.subscribe("test")
client.loop_forever()
