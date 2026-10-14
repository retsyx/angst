#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The queue passed to service_start must have length 1 and this item size. */
#define SERVICE_MESSAGE_CAPACITY 1024
typedef struct {
    char text[SERVICE_MESSAGE_CAPACITY];
} service_message_t;

/*
 * Registers network listeners and starts the HTTP worker without waiting for
 * connectivity. Call after esp_netif_init() and default event-loop creation,
 * before starting Matter, which owns Wi-Fi initialization, commissioning and
 * reconnection. Shares the mDNS responder with Matter.
 */
esp_err_t service_start(QueueHandle_t messages);

/* The persisted fetch URL starts empty. Empty disables price requests; otherwise
 * a complete HTTP or HTTPS URL is required. Length excludes the terminator.
 * Call after service_start(). A successful update wakes the polling worker;
 * an in-flight response from the previous URL is discarded.
 */
#define SERVICE_URL_MAX_LENGTH 1024
esp_err_t service_get_url(char *buffer, size_t capacity);
esp_err_t service_set_url(const char *url);

/*
 * The GUI calls this after consuming a message and when its animation ends.
 * The worker clears idle when it publishes a message and waits for the GUI's
 * acknowledgement before the next request, including for animations >30s.
 */
void service_set_display_idle(bool idle);

#ifdef __cplusplus
}
#endif
