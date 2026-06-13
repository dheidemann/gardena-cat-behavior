Does your mower turn into a cat when it rains? **Fear no more**: This script got you covered! Introducing, the

# Gardena Cat Behavior

This script will periodically check [Open-Meteo](https://open-meteo.com/) weather data and sends your loyal mower home when it rains. This is written for and tested to run on an `ESP8266` with WiFi, however with a few tweaks should work on any device with internet access.

Note that this script communicates with the [GARDENA smart system API](https://docs.developer.husqvarnagroup.cloud/gardena-smart-system-api/iapi-v2.yml) and will select the first mower it finds. It is *NOT* intended to be used for a mower available under the *Automower Connect API*. Check which API your mower uses first!

## Setup

1. Register your device at [Husqvarna MyPages](https://mypages.husqvarna.com)
2. Under *My Services*, enable the *Developer Portal*
3. In the *Developer Portal* create a new application with the default settings
4. Under connected APIs, connect the *Authentication API* and the *GARDENA smart system API*
5. At the top of the [maeeeeher.ino](./maeeeeher.ino) fill all variables
6. Flash onto your device
