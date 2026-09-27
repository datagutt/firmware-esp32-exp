#pragma once

#include <stdbool.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the config portal: register its HTTP handlers and start the
 *        captive DNS server.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ap_start(void);

/**
 * @brief Stop the Access Point services (DNS only; HTTP server is shared)
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ap_stop(void);

/**
 * @brief Register the catch-all wildcard URI handler.
 *
 * Must be called after all other URI handlers (e.g. STA API) have been
 * registered so that specific paths are matched before the wildcard.
 */
void ap_register_wildcard(void);

/**
 * @brief Initialize the AP network interface
 */
void ap_init_netif(void);

/**
 * @brief Configure the Access Point settings
 */
void ap_configure(void);

/**
 * @brief Allow the config portal to close automatically.
 *
 * The portal shuts down 2 minutes after the STA has an IP address. The
 * countdown is cancelled if the STA drops before it expires, and the portal
 * stays open for as long as the STA is not connected.
 */
void ap_enable_auto_shutdown(void);

/**
 * @brief Reopen the config portal (APSTA mode + captive DNS) after the STA
 *        gave up connecting. No-op if already open or AP mode was not
 *        configured at boot.
 */
void ap_open_portal(void);

/**
 * @brief Whether the config portal (soft AP + captive DNS) is currently open.
 */
bool ap_portal_active(void);

#ifdef __cplusplus
}
#endif
