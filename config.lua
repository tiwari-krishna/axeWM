print("SUPER =", SUPER)
print("CONTROL =", CONTROL)
print("key('j') =", key("j"))

-- this should NOT work if the sandbox is tight -- os should not exist
if os then
    print("DANGER: os is visible!")
else
    print("good: os is nil, sandboxed correctly")
end

menu = "rofi -show drun -i -p"
tmux = "foot -e sh -c \"tmux attach || tmux_session\""--"tmux attach || ~/.local/bin/tmux_session"

-- real keybinds table: { mods, keysym, "action_name", arg }
keybinds = {
    {SUPER|SHIFT,         key("z"),      "test_focus"},
    {SUPER,         key("k"),      "focus_prev"},
    {SUPER,         key("h"),      "setmfact",   -0.05},
    {SUPER,         key("l"),      "setmfact",    0.05},
    {SUPER,         key("q"),      "totally_fake_action"},  -- should be rejected, not crash
    {SUPER|SHIFT,         key("r"),       "spawn", menu},
    {SUPER,         key("return"),      "spawn", tmux}
}
