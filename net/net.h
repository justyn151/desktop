#ifndef NET_H
#define NET_H

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_DEVICE NM ".Device"
#define NM_WIRELESS NM_DEVICE ".Wireless"
#define NM_AP NM ".AccessPoint"
#define NM_ACTIVE NM ".Connection.Active"
#define NM_IP4 NM ".IP4Config"
#define NM_IP6 NM ".IP6Config"
#define NM_SETTINGS NM ".Settings"
#define NM_SETTINGS_PATH NM_PATH "/Settings"
#define NM_PROFILE NM_SETTINGS ".Connection"
#define NM_AGENTS NM ".AgentManager"
#define NM_AGENTS_PATH NM_PATH "/AgentManager"
#define NM_AGENT NM ".SecretAgent"
#define NM_AGENT_PATH NM_PATH "/SecretAgent"
#define WIFI_SETTING "802-11-wireless"
#define WIFI_SECURITY WIFI_SETTING "-security"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define DBUS "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"

/* NetworkManager's D-Bus enumeration values. */
enum {
    NM_DEVICE_WIFI = 2,
    NM_DEVICE_DISCONNECTED = 30,
    NM_DEVICE_NEED_AUTH = 60,
    NM_DEVICE_IP_CONFIG = 70,
    NM_DEVICE_SECONDARIES = 90,
    NM_DEVICE_ACTIVATED = 100,
    NM_DEVICE_FAILED = 120,
    NM_REASON_NO_SECRETS = 7,
    NM_REASON_SUPPLICANT_DISCONNECT = 8,
    NM_REASON_SUPPLICANT_TIMEOUT = 11,
    NM_REASON_SSID_NOT_FOUND = 53,
    NM_ACTIVE_ACTIVATED = 2,
    NM_ACTIVE_DEACTIVATED = 4,
    NM_ACTIVE_REASON_IP_CONFIG = 5,
    NM_ACTIVE_REASON_NO_SECRETS = 9,
    NM_ACTIVE_REASON_LOGIN_FAILED = 10,
    NM_PROFILE_VOLATILE = 0x4,
    NM_PROFILE_EXTERNAL = 0x8,
    NM_UPDATE_TO_DISK = 0x1,
    NM_UPDATE_NO_REAPPLY = 0x40,
    NM_MODE_INFRA = 2,
    NM_MODE_AP = 3,
    NM_AP_PRIVACY = 0x0001,
    NM_SEC_PSK = 0x0100,
    NM_SEC_EAP = 0x0200,
    NM_SEC_SAE = 0x0400,
    NM_SEC_OWE = 0x0800,
    NM_SEC_OWE_TRANSITION = 0x1000,
    NM_SEC_SUITE_B = 0x2000,
    NM_SECRET_ALLOW_INTERACTION = 0x1,
    NM_SECRET_REQUEST_NEW = 0x2,
    SSID_MAX = 32,
    UUID_SIZE = 37,
    PASSWORD_MAX = 255,
};

#endif
