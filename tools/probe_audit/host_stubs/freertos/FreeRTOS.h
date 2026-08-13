#pragma once
// Host stub: wifi_density.c #includes freertos/FreeRTOS.h for the portMUX_TYPE spinlock it uses to
// guard state shared with the real firmware's Wi-Fi promiscuous-mode RX callback (a concern that
// doesn't exist on the host -- this build is single-threaded). No-op stand-ins so the file compiles
// unmodified; mirrors the existing esp_random.h/nvs.h stub pattern in this directory.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))
