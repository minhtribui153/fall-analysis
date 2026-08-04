# Prerequisites

## Hardware

- Nesso N1 with BMI270
- USB cable capable of data transfer
- Windows laptop with working Bluetooth LE
- Raspberry Pi 5 server
- shared private LAN or hotspot

## Windows laptop software

- Python and the existing working virtual environment
- PlatformIO CLI
- Git or a repository copy
- Bluetooth enabled

The gateway dependencies are listed in root `requirements.txt`.

## Raspberry Pi software

- 64-bit Linux
- Docker Engine
- Docker Compose plugin
- GNU Make
- Python 3 and `venv`
- `curl`

## Required repository inputs

Before setup, obtain:

- a valid root `.env` based on `.env.example`;
- the missing base PostgreSQL schema or a database backup that already contains it;
- a Mosquitto configuration under `${DIR_DATA}/instances/mosquitto/config/mosquitto.conf`;
- the Windows Bless compatibility modifications used by the working laptop environment.

## Network checks

The laptop must be able to reach the Pi on at least:

```text
1883  MQTT
5432  PostgreSQL
```

For administration, also expose 5050, 8080 and 8083 as needed.
