#include <src/plugins/PluginAPI.hpp>

#include <stdexcept>

#include "globals.hpp"
#include "ChordManager.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (!g_chordManager.init(handle)) {
        HyprlandAPI::addNotificationV2(
            handle, {{"text", std::string{"[hyprchords] failed to initialize (Lua config backends are not supported yet)"}}, {"time", uint64_t{10000}}, {"color", CHyprColor{0}}, {"icon", ICON_ERROR}});
        throw std::runtime_error("hyprchords: initialization failed");
    }

    return {"hyprchords", "sxhkd-style key chords/chains (SUPER+X ; K)", "chicken", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_chordManager.shutdown();
}
