// config.h - build-time configuration, debug log macro, TFT colour constants.
// Every value here can be overridden from platformio.ini `build_flags`.
#pragma once
#include <Arduino.h>

// ---- serial / bus ---------------------------------------------------------
#ifndef BRIDGE_BAUD
#define BRIDGE_BAUD 9600            // mount link + BT bridge (iOptron legacy set)
#endif
#ifndef RS232_RX_PIN
#define RS232_RX_PIN 16             // ESP32 UART2 RX  <- MAX3232 R1OUT
#endif
#ifndef RS232_TX_PIN
#define RS232_TX_PIN 17             // ESP32 UART2 TX  -> MAX3232 T1IN
#endif
#ifndef GPS_RX_PIN
#define GPS_RX_PIN 32               // ESP32 UART1 RX  <- NEO-6M TX (receive only)
#endif
#ifndef GPS_BAUD
#define GPS_BAUD 9600
#endif

// ---- GPIO ---------------------------------------------------------------
#ifndef ACT_LED_PIN
#define ACT_LED_PIN 2               // LED2 - lit while the mount link is up
#endif
#ifndef BTN_SYNC_PIN
#define BTN_SYNC_PIN 39             // BTN1, active-low, external pull-up
#endif
#ifndef SD_CS_PIN
#define SD_CS_PIN 33                // parked HIGH at boot (shared SPI, unused)
#endif
#ifndef TOUCH_CS_PIN
#define TOUCH_CS_PIN 13             // parked HIGH at boot (shared SPI, unused)
#endif

// ---- TFT (ST7789, hardware SPI / VSPI) ---------------------------------
#ifndef TFT_CS
#define TFT_CS 25
#endif
#ifndef TFT_DC
#define TFT_DC 26
#endif
#ifndef TFT_RST
#define TFT_RST 27
#endif
#ifndef TFT_HZ
#define TFT_HZ 24000000            // SPI clock; drop to 16-20 MHz on flaky wiring
#endif
#ifndef TFT_INVERT
#define TFT_INVERT 0               // 0/1 - flip if colours look wrong
#endif

// ---- Bluetooth / misc -------------------------------------------------
#ifndef BT_NAME
#define BT_NAME "SmartEQ-RJ9"
#endif
#ifndef DEBUG_BAUD
#define DEBUG_BAUD 115200
#endif
#ifndef POLL_PERIOD_MS
#define POLL_PERIOD_MS 400         // gap between mount self-poll queries when idle
#endif

// ---- debug log ------------------------------------------------------
#if defined(DEBUG_LOG) && DEBUG_LOG
#define LOG(...)  do { Serial.printf(__VA_ARGS__); } while (0)
#else
#define LOG(...)  do {} while (0)
#endif

// ---- TFT colours (RGB565) -----------------------------------------
#define BG      0x0000
#define C_HEAD  0x07FF
#define C_LBL   0x8410
#define C_VAL   0xFFFF
#define C_OK    0x07E0
#define C_WARN  0xFFE0
#define C_BAD   0xF800
#define C_SEP   0x2124
