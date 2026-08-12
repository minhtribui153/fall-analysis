#pragma once

#if defined(TERVANIX_BOARD_NESSO_N1)
#define TERVANIX_HAS_IMU 1
#elif defined(TERVANIX_BOARD_ESP32_DEV)
#define TERVANIX_HAS_IMU 0
#else
#error "Select a supported PlatformIO environment"
#endif
