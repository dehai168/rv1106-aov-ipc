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

/* Startup: re-apply saved config if the OEM boot scripts clobbered it
 * (e.g. their udhcpc flushed a static address). No-op when already correct. */
int network_ensure_boot(void);

/* AOV: bring iface down without changing saved config. */
int network_set_link_down(const char *iface);

/*
 * AOV deep-sleep prep (Rockchip AOV guide tip):
 * Before echo mem, thoroughly cut internal FE-PHY power steal —
 * disable eth wakeup, link down, unbind rk_gmac-dwmac so driver
 * runs rk_gmac_powerdown() → RV1106 FEPHY SHUTDOWN.
 */
int network_prepare_aov_suspend(void);
/* After each wake in AOV loop: keep FE-PHY down (no DHCP). */
int network_resume_from_aov(void);
/* On leave / abort: rebind gmac and network_apply(). */
int network_restore_after_aov(void);

#ifdef __cplusplus
}
#endif

#endif /* IPC_SYSTEM_NETWORK_SERVICE_H */
