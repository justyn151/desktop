#ifndef BLUE_H
#define BLUE_H

#define BLUE_VERSION "0.3.0"

#define BLUEZ "org.bluez"
#define BLUEZ_ADAPTER BLUEZ ".Adapter1"
#define BLUEZ_DEVICE BLUEZ ".Device1"
#define BLUEZ_BATTERY BLUEZ ".Battery1"
#define BLUEZ_AGENT BLUEZ ".Agent1"
#define BLUEZ_AGENT_MANAGER BLUEZ ".AgentManager1"
#define BLUEZ_PATH "/org/bluez"
/* Where blue answers BlueZ's pairing questions while it runs. */
#define AGENT_PATH "/org/blue/agent"
#define BLUEZ_ERROR BLUEZ ".Error."
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define DBUS "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"

enum {
    /* Connecting to a device that is off ends in a page timeout after about
       5 seconds, but profiles can take much longer to come up. */
    CONNECT_TIMEOUT_US = 45 * 1000000ull,
    CALL_TIMEOUT_US = 25 * 1000000ull,
    /* Pairing waits for someone to compare or type a code. */
    PAIR_TIMEOUT_US = 90 * 1000000ull,
    /* How long Escape waits for the rest of an escape sequence. */
    ESCAPE_TIMEOUT_US = 50000,
    MAX_ADAPTERS = 8,
    NOTICE_SIZE = 192,
    /* A name typed in rename: BlueZ takes up to 248 bytes. */
    PROMPT_TEXT_SIZE = 249,
};

#endif
