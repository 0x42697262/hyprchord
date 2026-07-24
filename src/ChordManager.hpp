#pragma once

#include <src/plugins/PluginAPI.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/config/values/types/IntValue.hpp>
#include <src/config/values/types/StringValue.hpp>

#include <hyprutils/signal/Signal.hpp>

#include <lua.hpp>

#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct SKeybind;

// one chord in a chain: [MODS +] [~][@]KEY
struct SChordStep {
    uint32_t    modmask     = 0;
    std::string key         = ""; // key name (or mouse:NNN), empty when keycode is used
    uint32_t    keycode     = 0;  // for code:NN steps
    bool        release     = false; // @ prefix
    bool        passthrough = false; // ~ prefix
    bool        anyMods     = false; // "any" modifier -> matches every modifier state
    bool        lockChain   = false; // preceded by ':' -> matching engages locked mode
    bool        hasMods     = false;
    std::string repr        = ""; // normalized "super+x"
};

// a full chain: steps + the dispatcher it fires
struct SChord {
    std::vector<SChordStep>                          steps;
    std::string                                      dispatcher;
    std::string                                      arg;
    int                                              luaRef = LUA_NOREF; // registry ref of a lua action; when set, dispatcher/arg are unused
    std::vector<std::pair<std::string, std::string>> cycle; // sxhkd cycling: command variants, advanced per fire
    size_t                                           cyclePos   = 0;
    bool                                             lockOnFire = false; // final step engages locked mode
    std::string                                      repr;
    std::string                                      description; // optional user label for the final bind (else auto)
};

class CChordManager {
  public:
    bool                   init(HANDLE handle);
    void                   shutdown();

    Hyprlang::CParseResult onChordKeyword(const std::string& value, const std::string& description = "");
    Hyprlang::CParseResult onChordLua(const std::string& steps, lua_State* L, int ref, const std::string& description = "");
    Hyprlang::CParseResult onSxhkdSource(const std::string& value);

    // fire a registered chord's action by index (hl.plugin.hyprchords.fire),
    // same path as a completed key chain
    SDispatchResult        fire(const std::string& idxStr) { return exec(idxStr); }

  private:
    struct SSubmapInfo {
        std::set<std::string> stepKeys;             // lowercase key names bound as steps in this submap
        bool                  hasMachinery = false; // abort-key (+ catchall) binds added
    };

    std::expected<SChord, std::string> parseChordLine(const std::string& value, bool luaAction = false);
    std::optional<std::string>         addChordLine(const std::string& value, int luaRef = LUA_NOREF, const std::string& description = "");
    std::optional<std::string>         registerChord(const SChord& chord);
    void                               ensureSubmapMachinery(const std::string& submapName);
    SP<SKeybind>                       addBind(SKeybind bind);

    SDispatchResult                    enter(const std::string& arg); // arg: [+]submap, '+' engages locked mode
    SDispatchResult                    exec(const std::string& idxStr);
    SDispatchResult                    abort(const std::string& mode);
    SDispatchResult                    toggle(const std::string& arg);
    SDispatchResult                    importSxhkd(const std::string& arg);

    void                               onTimeout();
    void                               onPreReload();
    void                               onReloaded();
    void                               leaveOurSubmap();
    bool                               inOurSubmap() const;

    // identifies the key/mouse event that last entered/fired a chord, so the
    // catchall abort bind firing on the same event can be ignored
    void                               armEventGuard();
    bool                               eventGuardMatches() const;

    std::vector<SChord>                          m_chords;
    std::map<std::string, SSubmapInfo>           m_submaps;
    std::map<std::string, std::string>           m_bindOwners; // bind identity -> "handler|arg", for dedupe/ambiguity
    std::vector<std::string>                     m_displayKeys;
    std::vector<WP<SKeybind>>                    m_binds; // for hyprchords_toggle
    std::vector<SChordStep>                      m_rootSteps; // first chords, for conflict warnings
    std::vector<std::string>                     m_warnings;

    bool                                         m_locked  = false; // a ':' chord matched; only the abort key exits
    bool                                         m_enabled = true;  // hyprchords_toggle state

    // state of the most recent lua chord registration. Chords (and their refs) are dropped
    // on config.preReload, so refs never outlive this state; refs are deliberately never
    // luaL_unref'd — reload order vs state teardown isn't guaranteed, and registry entries
    // die with the state anyway
    lua_State*                                   m_luaState = nullptr;

    SP<CEventLoopTimer>                          m_timer;
    SP<Config::Values::CIntValue>                m_timeout;
    SP<Config::Values::CStringValue>             m_abortKey;
    SP<Config::Values::CIntValue>                m_stickyMods;
    SP<Config::Values::CIntValue>                m_swallow;

    Hyprutils::Signal::CHyprSignalListener       m_preReloadListener;
    Hyprutils::Signal::CHyprSignalListener       m_reloadedListener;
    Hyprutils::Signal::CHyprSignalListener       m_submapListener;

    struct {
        uint32_t timeMs    = 0;
        uint32_t code      = 0;
        uint32_t mouseCode = 0;
        bool     valid     = false;
    } m_eventGuard;
};

inline CChordManager g_chordManager;
