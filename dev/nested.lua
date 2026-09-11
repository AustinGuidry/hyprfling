-- A throwaway nested Hyprland for trying hyprfling builds without risking your
-- real session. From a terminal inside Hyprland:
--
--   Hyprland -c dev/nested.lua
--   export HYPRLAND_INSTANCE_SIGNATURE=<the new instance from `hyprctl instances`>
--   make load
--
-- Uses ALT instead of SUPER: this runs as a window inside your real session,
-- which grabs SUPER + drag before it gets here.

mainMod  = "ALT"
terminal = os.getenv("TERMINAL") or "kitty"

hl.config({
    general = { gaps_in = 5, gaps_out = 20, border_size = 3, layout = "dwindle" },
    misc    = { force_default_wallpaper = 0, disable_hyprland_logo = true },
    input   = { follow_mouse = 1 },
    debug   = { disable_logs = false }, -- plugin log lines show up in `hyprctl rollinglog`
})

-- Wrapped in functions so the lookup happens at keypress, after `make load`.
hl.bind(mainMod .. " + M", function() hl.plugin.fling.mode() end)
hl.bind(mainMod .. " + Y", function() hl.plugin.fling.yeet() end)

hl.bind(mainMod .. " + Return", hl.dsp.exec_cmd(terminal))
hl.bind(mainMod .. " + Q", hl.dsp.window.kill())
hl.bind(mainMod .. " + mouse:272", hl.dsp.window.drag(), { mouse = true })
hl.bind(mainMod .. " + mouse:273", hl.dsp.window.resize(), { mouse = true })

hl.on("hyprland.start", function()
    for _ = 1, 3 do
        hl.exec_cmd(terminal)
    end
end)
