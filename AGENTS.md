# AGENTS

This project is a TFT screen emulator with web output. The ESP32 board wires into a device's SDA/SCL and emulates a slave device.
It hosts a web interface either on an access point or connected to a wifi network, then displays the video output from there.

The device should initially, or if it cannot find its WiFi network, or if the BOOT button is pressed to interrupt the connection,
host an access point. The index.html should have the display view itself, and settings for the emulated display (resolution, orientation, etc.),
and the WiFi connection (connect to an existing network, disconnect and go in AP mode, set AP SSID and password, etc.)

## DOCUMENTATION

ESP-IDF docs are available at E:\esp\.espressif\v6.1\esp-idf\docs\en in .rst format.