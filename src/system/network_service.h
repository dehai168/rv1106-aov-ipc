#ifndef IPC_SYSTEM_NETWORK_SERVICE_H
#define IPC_SYSTEM_NETWORK_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char iface[32];
  char mode[16]; /* dhcp | static */
  char ip[64];
  char netmask[64];
  char gateway[64];
  char dns1[64];
  char dns2[64];
  /* runtime */
  char link[16]; /* up | down | unknown */
  char current_ip[64];
  char current_netmask[64];
  char current_gateway[64];
  char usb0_ip[64];
} NetworkConfig;

/* Load desired config from config_service into out (+ runtime status). */
int network_get(NetworkConfig *out);

/* Validate + save to config_service (+ interfaces/resolv). If apply!=0, apply now. */
int network_set(const NetworkConfig *in, int apply);

/* Apply currently saved config to iface (eth0). */
int network_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* IPC_SYSTEM_NETWORK_SERVICE_H */
