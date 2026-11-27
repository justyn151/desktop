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
};

struct bar_config {
    int height, padding, font_size, spacing;
    bool bottom, hide_empty, underline_active, wallpaper_theme;
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
    .family = "DejaVu Sans Mono",
    .clock_format = "%a, %b %e  %l:%M%P",
    .network_enabled = true,
    .network_show_strength = true,
    .network_click_enabled = true,
    .network_separator = " | ",
    .network_custom_text = "WiFi",
    .network_command = "deal --app-id net --columns 84 --rows 26 -e net",
};

#endif
