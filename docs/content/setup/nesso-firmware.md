# Nesso N1 firmware setup

## Active project

The PlatformIO project is:

```text
tervanix-n1/platformio.ini
```

Run commands from `tervanix-n1`, not the repository root.

## Build

```powershell
cd tervanix-n1
pio run -e nesso_n1 -t clean
pio run -e nesso_n1
```

The active CMake source list does not compile `wifi_manager.cpp` or `mqtt_publisher.cpp`.

## Upload

Close Arduino/PlatformIO serial monitors that may hold the COM port, then:

```powershell
pio run -e nesso_n1 -t upload
```

Specify a port when auto-detection is wrong:

```powershell
pio run -e nesso_n1 -t upload --upload-port COM9
```

## Monitor

```powershell
pio device monitor -e nesso_n1
```

Expected sequence:

```text
Tervanix BLE telemetry firmware starting
Scanning for TVX-REG
Connected
MTU=64
New UUID assigned / Existing UUID accepted
BLE registration confirmed; telemetry can start
BMI270 ready
```

## Verified hardware configuration

- BMI270 I²C address: `0x68`
- BMI270 chip ID: `0x24`
- SDA: GPIO 10
- SCL: GPIO 8
- I²C: 400 kHz

## No menuconfig network settings

The Nesso is BLE-only. Wi-Fi and MQTT Kconfig options were intentionally removed. Configure MQTT on the Python gateway instead.
