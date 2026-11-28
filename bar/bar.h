#ifndef BAR_H
#define BAR_H

#include <stdbool.h>
#include <stdint.h>

enum {
    BAR_MAX_WORKSPACES = 32,
    BAR_LABEL_SIZE = 64,
    BAR_FONT_FAMILY_SIZE = 128,
    BAR_CLOCK_FORMAT_SIZE = 256,
    BAR_CLOCK_TEXT_SIZE = 512,
    BAR_NETWORK_SEPARATOR_SIZE = 64,
    BAR_NETWORK_TEXT_SIZE = 64,
    BAR_NETWORK_COMMAND_SIZE = 1024,
    BAR_VOLUME_TEXT_SIZE = 64,
    BAR_BLUETOOTH_TEXT_SIZE = 64,
};

/* How the active workspace stands out besides its color. */
enum bar_active_style {
    BAR_ACTIVE_BOX,       /* Opaque box in the workspace color, label in the background color. */
    BAR_ACTIVE_UNDERLINE, /* Two-pixel line under the label. */
    BAR_ACTIVE_NONE,
};

struct bar_config {
    int height, padding, font_size, spacing;
    bool bottom, hide_empty, wallpaper_theme;
    enum bar_active_style active_style;
    uint32_t background, foreground;
    uint32_t active_color, occupied_color, inactive_color, urgent_color;
    double opacity, scale;
    unsigned label_count;
    char labels[BAR_MAX_WORKSPACES][BAR_LABEL_SIZE];
    char family[BAR_FONT_FAMILY_SIZE];
    char clock_format[BAR_CLOCK_FORMAT_SIZE];
    bool network_enabled, network_show_ssid, network_show_strength;
    bool network_click_enabled;
    char network_separator[BAR_NETWORK_SEPARATOR_SIZE];
    char network_custom_text[BAR_NETWORK_TEXT_SIZE];
    char network_command[BAR_NETWORK_COMMAND_SIZE];
    bool volume_enabled;
    uint32_t volume_muted_color;
    char volume_custom_text[BAR_VOLUME_TEXT_SIZE];
    /* follow prints a line per change; the others run once per pointer action. */
    char volume_command[BAR_NETWORK_COMMAND_SIZE];
    char volume_click[BAR_NETWORK_COMMAND_SIZE];
    char volume_middle_click[BAR_NETWORK_COMMAND_SIZE];
    char volume_scroll_up[BAR_NETWORK_COMMAND_SIZE];
    char volume_scroll_down[BAR_NETWORK_COMMAND_SIZE];
    bool bluetooth_enabled, bluetooth_show_device, bluetooth_show_battery;
    bool bluetooth_click_enabled;
    uint32_t bluetooth_off_color;
    char bluetooth_custom_text[BAR_BLUETOOTH_TEXT_SIZE];
    char bluetooth_command[BAR_NETWORK_COMMAND_SIZE];
};

static const struct bar_config bar_defaults = {
    .height = 24,
    .padding = 8,
    .font_size = 14,
    .spacing = 10,
    .background = 0x353535u,
    .opacity = 0.78,
    .scale = 1.0,
    .foreground = 0xc5ad81u,
    .active_color = 0xead4aau,
    .occupied_color = 0xc5ad81u,
    .inactive_color = 0x887d69u,
    .urgent_color = 0xcf7777u,
    .label_count = 9,
    .labels = {"1", "2", "3", "4", "5", "6", "7", "8", "9"},
    .family = "monospace",
    .clock_format = "%a, %b %e  %l:%M%P",
    .network_enabled = true,
    .network_show_strength = true,
    .network_click_enabled = true,
    .network_separator = " | ",
    .network_custom_text = "WiFi",
    .network_command = "deal --app-id net --columns 84 --rows 26 -e net",
    .volume_enabled = true,
    .volume_muted_color = 0x887d69u,
    .volume_custom_text = "Vol",
    .volume_command = "vol --follow",
    .volume_click = "vol mixer",
    .volume_middle_click = "vol mute",
    .volume_scroll_up = "vol up",
    .volume_scroll_down = "vol down",
    .bluetooth_enabled = true,
    .bluetooth_show_device = true,
    .bluetooth_show_battery = true,
    .bluetooth_click_enabled = true,
    .bluetooth_off_color = 0x887d69u,
    .bluetooth_custom_text = "BT",
    .bluetooth_command = "deal --app-id blue --columns 84 --rows 26 -e blue",
};

#endif
