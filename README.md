## This is a small example for home automation with decent security, running on ESP8266.

### It does the following:

1. Use WiFi EAP-TLS (certificate-based enterprise authentication) to connect to a wireless network.
2. Connect securely via TLS to an MQTT broker using client-cert based authentication
3. Subscribes to topic "esp8266/update" for triggering OTA updates.
4. Subscribes to topic "esp8266/debug" to enable debugging
5. Subscribes to topic "esp8266/nodebug" to disable debugging
6. Publishes changes on GPIO to topic "esp8266/gpio" (not yet implemented)

It also serves as an example for my [esp8266-rtos-syslog](https://github.com/felfert/esp8266-rtos-syslog) component.
This is WIP

### Prerequisites:
- You need a local PKI infrastructure to secure all components.
  - A local WiFi network using radius-based EAP-TLS for authentication
  - A local MQTT server which uses TLS and client certificate based athentication
  - A local HTTP server for providing OTA update functionality
- Espressif's [ESP8266_RTOS_SDK](https://github.com/espressif/ESP8266_RTOS_SDK)
- Set it up according to https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/get-started/index.html

### Steps to build/flash/run this app:
1. Clone this repository with `git clone --recursive`
1. Run `make menuconfig`, change setup according to your environment.
2. From your PKI, copy the CA's certificate (PEM-encoded text) to `main/ca.crt` 
3. From your PKI, copy the client certificate (PEM-encoded text) to `main/client.crt`
4. From your PKI, copy the client key (PEM-encoded text) to `main/client.key`
5. Connect your target board via USB
6. Run make flash monitor

### Note:
There are **A LOT** of "HOWTOs" and instructions on the Internet which use the Arduino IDE and an **ancient** NON-OSS SDK.

**DO NOT USE** these. The ancient NON-OSS SDK is **OUTDATED**, **UNMAINTAINED**  and **DANGEROUS**. For example it
uses TLS v1.1 for EAP-TLS which is considered insecure and therefore is **NOT** supported by any decent Radius anymore.
This is BTW the reason, WHY those other examples do NOT work. It was also the motivation for creating this project.

The newer [ESP8266-RTOS-SDK](https://github.com/espressif/ESP8266_RTOS_SDK) mentioned above is actively maintained, uses decent libs
and provides support for TLS v1.2 and given that, WiFi EAP-TLS actually works.
