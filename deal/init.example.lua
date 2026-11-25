-- Copy this file to ~/.config/deal/init.lua to customize deal.
return {
    window = {
        title = "deal",
        dynamic_title = true,
        padding = 8, -- Or { horizontal = 10, vertical = 6 }.
        opacity = 1.0,
        decorations = true,
        columns = 80,
        rows = 24,
    },
    font = {
        family = "monospace",
        size = 14,
        letter_spacing = 0,
        line_spacing = 0,
    },
    cursor = {
        style = "underline", -- block, underline, or bar
        blink = false,
        blink_interval = 500,
        thickness = 2,
    },
    colors = {
        -- foreground = "#dce5ed",
        -- background = "#10161e",
        -- cursor = "#dce5ed",
        -- palette = { [2] = "#bf616a" }, -- Lua indices 1 through 16.
    },
    selection = {
        foreground = "#10161e",
        background = "#8ac1de",
    },
    terminal = {
        -- shell = "/bin/bash",
        term = "xterm-color", -- "deal" requires the installed terminfo.
    },
    scrollback = { capacity = 10000, scroll_on_input = true },
    mouse = { reporting = true, selection_override = "Shift" },
    hyperlinks = { enabled = true, modifier = "Ctrl" },
    bell = { audible = false, visual = false, urgency = false },
    shortcuts = { zoom_in = "Ctrl+Shift+=", zoom_out = "Ctrl+Shift+-" },
    keybinds = { reload_config = "Ctrl+Shift+R", fullscreen = "F11" },
}
