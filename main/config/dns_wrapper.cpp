#include <cstring>

#include <esp_log.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "lwip/netdb.h"

namespace {
constexpr const char* TAG = "DNS_WRAPPER";
}  // namespace

// Linker --wrap symbols must have C linkage
extern "C" {

// Declare the real function (which will be esp_getaddrinfo)
int __real_esp_getaddrinfo(const char *nodename, const char *servname,
                           const struct addrinfo *hints, struct addrinfo **res);

int __wrap_esp_getaddrinfo(const char *nodename, const char *servname,
                           const struct addrinfo *hints,
                           struct addrinfo **res) {
  if (nodename != nullptr) {
    constexpr const char* mdns_suffix = ".local";
    const size_t mdns_suffix_len = strlen(mdns_suffix);
    size_t len = strlen(nodename);
    // Check for .local suffix for mDNS
    if (len >= mdns_suffix_len &&
        strcasecmp(nodename + len - mdns_suffix_len, mdns_suffix) == 0) {
      ESP_LOGD(TAG, "Redirecting mDNS query for %s to lwip_getaddrinfo",
               nodename);
      return lwip_getaddrinfo(nodename, servname, hints, res);
    }
  }
  // Fallback to the standard ESP implementation (which handles IPv6/AF_UNSPEC
  // better)
  return __real_esp_getaddrinfo(nodename, servname, hints, res);
}

}  // extern "C"
