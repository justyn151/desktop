#ifndef VOL_H
#define VOL_H

#define VOL_VERSION "0.1.0"

/* Exit statuses. */
enum {
    EXIT_OK = 0,
    EXIT_FAILED = 1, /* The sound server can't be reached, or has no default device. */
    EXIT_USAGE = 2,
};

/* Percent of normal volume. Above 100 is software amplification. */
enum {
    VOL_STEP_DEFAULT = 5,
    VOL_MAX_DEFAULT = 100,
    VOL_MAX_LIMIT = 150,
};

/* How long a one-shot command waits for the server before giving up. */
#define VOL_TIMEOUT_MS 5000
/* How long --follow waits before connecting again after losing the server. */
#define VOL_RECONNECT_MS 1000

#endif
