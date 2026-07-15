#include "ChordManager.hpp"
#include "globals.hpp"

#include <src/managers/KeybindManager.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/config/shared/actions/ConfigActions.hpp>
#include <src/SharedDefs.hpp>

#include <hyprutils/string/String.hpp>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <format>

constexpr const char* SUBMAP_PREFIX = "hc:";

static std::string    lower(std::string s) {
    std::ranges::transform(s, s.begin(), ::tolower);
    return s;
}

static void notify(const std::string& msg) {
    HyprlandAPI::addNotificationV2(PHANDLE, {{"text", "[hyprchords] " + msg}, {"time", uint64_t{10000}}, {"color", CHyprColor{0}}, {"icon", ICON_WARNING}});
}

static std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t                   pos = 0;
    while (true) {
        const auto NEXT = s.find(sep, pos);
        out.push_back(Hyprutils::String::trim(s.substr(pos, NEXT - pos)));
        if (NEXT == std::string::npos)
            break;
        pos = NEXT + 1;
    }
    return out;
}

static Hyprlang::CParseResult chordKeywordHandler(const char* COMMAND, const char* VALUE) {
    return g_chordManager.onChordKeyword(VALUE ? VALUE : "");
}

bool CChordManager::init(HANDLE handle) {
    m_timeout    = makeShared<Config::Values::CIntValue>("plugin:hyprchords:timeout", "pending chain timeout in ms, 0 disables the timeout", 3000,
                                                         Config::Values::SIntValueOptions{.min = 0});
    m_abortKey   = makeShared<Config::Values::CStringValue>("plugin:hyprchords:abort_key", "key that aborts a pending chain", "Escape");
    m_stickyMods = makeShared<Config::Values::CIntValue>("plugin:hyprchords:sticky_mods", "modless steps also match with the previous step's modifiers still held", 1,
                                                         Config::Values::SIntValueOptions{.min = 0, .max = 1});

    if (!HyprlandAPI::addConfigValueV2(handle, m_timeout) || !HyprlandAPI::addConfigValueV2(handle, m_abortKey) || !HyprlandAPI::addConfigValueV2(handle, m_stickyMods))
        return false;

    // no V2 equivalent exists for plugin config keywords yet
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (!HyprlandAPI::addConfigKeyword(handle, "plugin:hyprchords:chord", chordKeywordHandler, Hyprlang::SHandlerOptions{}))
        return false; // e.g. Lua config backend, where plugin keywords are unsupported
#pragma GCC diagnostic pop

    if (!HyprlandAPI::addDispatcherV2(handle, "hyprchords_enter", [](std::string arg) { return g_chordManager.enter(arg); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprchords_exec", [](std::string arg) { return g_chordManager.exec(arg); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprchords_abort", [](std::string arg) { return g_chordManager.abort(arg); }))
        return false;

    m_timer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { g_chordManager.onTimeout(); }, nullptr);
    g_pEventLoopManager->addTimer(m_timer);

    m_preReloadListener = Event::bus()->m_events.config.preReload.listen([this] { onPreReload(); });
    m_reloadedListener  = Event::bus()->m_events.config.reloaded.listen([this] { onReloaded(); });
    m_submapListener    = Event::bus()->m_events.keybinds.submap.listen([this](const std::string& submap) {
        if (!submap.starts_with(SUBMAP_PREFIX) && m_timer)
            m_timer->updateTimeout(std::nullopt);
    });

    return true;
}

void CChordManager::shutdown() {
    leaveOurSubmap();

    for (const auto& dk : m_displayKeys) {
        g_pKeybindManager->removeKeybind(dk);
    }
    m_displayKeys.clear();

    if (m_timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_timer);
    m_timer.reset();

    m_preReloadListener.reset();
    m_reloadedListener.reset();
    m_submapListener.reset();

    m_chords.clear();
    m_submaps.clear();
    m_bindOwners.clear();
    m_rootSteps.clear();
    m_warnings.clear();
}

Hyprlang::CParseResult CChordManager::onChordKeyword(const std::string& value) {
    Hyprlang::CParseResult result;

    auto                   parsed = parseChordLine(value);
    if (!parsed) {
        result.setError(("hyprchords: " + parsed.error()).c_str());
        return result;
    }

    if (const auto ERR = registerChord(*parsed)) {
        result.setError(("hyprchords: " + *ERR).c_str());
        return result;
    }

    return result;
}

std::expected<SChord, std::string> CChordManager::parseChordLine(const std::string& value) {
    // chord = STEP ; STEP ; ... , dispatcher , arg
    const auto FIRSTCOMMA = value.find(',');
    if (FIRSTCOMMA == std::string::npos)
        return std::unexpected("expected: chord = STEP ; STEP ; ... , dispatcher , arg");

    const auto  SEQ  = Hyprutils::String::trim(value.substr(0, FIRSTCOMMA));
    const auto  REST = Hyprutils::String::trim(value.substr(FIRSTCOMMA + 1));

    SChord      chord;
    const auto  SECONDCOMMA = REST.find(',');
    if (SECONDCOMMA == std::string::npos)
        chord.dispatcher = REST;
    else {
        chord.dispatcher = Hyprutils::String::trim(REST.substr(0, SECONDCOMMA));
        chord.arg        = Hyprutils::String::trim(REST.substr(SECONDCOMMA + 1));
    }

    if (SEQ.empty())
        return std::unexpected("empty chord sequence");
    if (chord.dispatcher.empty())
        return std::unexpected("missing dispatcher");
    if (chord.dispatcher.starts_with("hyprchords_"))
        return std::unexpected("cannot target hyprchords' own dispatchers");
    if (!g_pKeybindManager->m_dispatchers.contains(chord.dispatcher))
        return std::unexpected("no such dispatcher: " + chord.dispatcher);

    for (const auto& RAWSTEP : splitOn(SEQ, ';')) {
        if (RAWSTEP.empty())
            return std::unexpected("empty step in chord sequence");

        SChordStep step;
        auto       tokens = splitOn(RAWSTEP, '+');
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].empty())
                return std::unexpected("empty modifier in step '" + RAWSTEP + "'");
            const auto MASK = g_pKeybindManager->stringToModMask(tokens[i]);
            if (!MASK)
                return std::unexpected("unknown modifier: " + tokens[i]);
            step.modmask |= MASK;
            step.hasMods = true;
        }

        std::string keyTok = tokens.back();
        while (!keyTok.empty() && (keyTok.front() == '~' || keyTok.front() == '@')) {
            if (keyTok.front() == '~')
                step.passthrough = true;
            else
                step.release = true;
            keyTok.erase(0, 1);
        }

        if (keyTok.empty())
            return std::unexpected("missing key in step '" + RAWSTEP + "'");
        if (keyTok.contains(':') && !keyTok.starts_with("code:"))
            return std::unexpected("':' (locked chains) is not supported yet, use ';'");

        if (keyTok.starts_with("code:")) {
            try {
                step.keycode = std::stoul(keyTok.substr(5));
            } catch (...) { return std::unexpected("invalid keycode in step '" + RAWSTEP + "'"); }
            if (!step.keycode)
                return std::unexpected("invalid keycode in step '" + RAWSTEP + "'");
        } else {
            if (xkb_keysym_from_name(keyTok.c_str(), XKB_KEYSYM_NO_FLAGS) == XKB_KEY_NoSymbol &&
                xkb_keysym_from_name(keyTok.c_str(), XKB_KEYSYM_CASE_INSENSITIVE) == XKB_KEY_NoSymbol)
                return std::unexpected("unknown key: " + keyTok);
            step.key = keyTok;
        }

        // sequence identity: mods + key, lowercase, prefixes excluded
        std::string repr;
        for (size_t i = 0; i + 1 < tokens.size(); ++i)
            repr += lower(tokens[i]) + "+";
        repr += lower(keyTok);
        step.repr = repr;

        chord.steps.push_back(step);
    }

    if (chord.steps.empty())
        return std::unexpected("empty chord sequence");

    std::string repr;
    for (const auto& s : chord.steps)
        repr += (repr.empty() ? "" : " ; ") + s.repr;
    chord.repr = repr;

    return chord;
}

std::optional<std::string> CChordManager::registerChord(const SChord& chord) {
    const size_t IDX    = m_chords.size();
    const bool   STICKY = m_stickyMods && m_stickyMods->value();

    struct SPending {
        SKeybind    bind;
        std::string identity;
        std::string owner;
        std::string stepKeyLower;
    };
    std::vector<SPending>    pending;
    std::vector<std::string> submapsToCreate;

    std::string              prefix;
    uint32_t                 stickyMask = 0;

    for (size_t i = 0; i < chord.steps.size(); ++i) {
        const auto&       STEP  = chord.steps[i];
        const bool        FINAL = i + 1 == chord.steps.size();
        const std::string BINDSUBMAP = i == 0 ? "" : SUBMAP_PREFIX + prefix;

        prefix = prefix.empty() ? STEP.repr : prefix + ";" + STEP.repr;
        const std::string NEXTSUBMAP = SUBMAP_PREFIX + prefix;

        const std::string HANDLER = FINAL ? "hyprchords_exec" : "hyprchords_enter";
        const std::string ARG     = FINAL ? std::to_string(IDX) : NEXTSUBMAP;
        const std::string OWNER   = HANDLER + "|" + ARG;

        std::set<uint32_t> masks = {STEP.hasMods ? STEP.modmask : 0};
        if (!STEP.hasMods && STICKY && stickyMask)
            masks.insert(stickyMask);
        if (STEP.hasMods)
            stickyMask = STEP.modmask;

        for (const auto MASK : masks) {
            const auto IDENTITY = std::format("{}|{}|{}|{}|{}", BINDSUBMAP, MASK, lower(STEP.key), STEP.keycode, STEP.release);

            if (const auto IT = m_bindOwners.find(IDENTITY); IT != m_bindOwners.end()) {
                if (IT->second == OWNER)
                    continue; // shared prefix with an already registered chord
                return std::format("chord '{}' is ambiguous at step {} ('{}'): the same key already continues or finishes another chord in this position "
                                   "(a chord cannot be a prefix of another chord)",
                                   chord.repr, i + 1, STEP.repr);
            }

            SKeybind bind;
            bind.key            = STEP.key;
            bind.keycode        = STEP.keycode;
            bind.modmask        = MASK;
            bind.handler        = HANDLER;
            bind.arg            = ARG;
            bind.submap         = SSubmap{.name = BINDSUBMAP, .reset = ""};
            bind.release        = STEP.release;
            bind.nonConsuming   = STEP.passthrough;
            bind.description    = FINAL ? std::format("hyprchords: {} -> {} {}", chord.repr, chord.dispatcher, chord.arg) : std::format("hyprchords: chain {} ...", prefix);
            bind.hasDescription = true;
            bind.displayKey     = std::format("hyprchords:{}:{}:{}", IDX, i, MASK);

            pending.push_back(SPending{.bind = bind, .identity = IDENTITY, .owner = OWNER, .stepKeyLower = lower(STEP.key)});
        }

        if (!FINAL)
            submapsToCreate.push_back(NEXTSUBMAP);
    }

    // no errors -> commit
    const auto ABORTKEY = lower(m_abortKey ? std::string{m_abortKey->value()} : "Escape");
    for (const auto& P : pending) {
        addBind(P.bind);
        m_bindOwners[P.identity] = P.owner;

        if (!P.bind.submap.name.empty()) {
            m_submaps[P.bind.submap.name].stepKeys.insert(P.stepKeyLower);
            if (!P.stepKeyLower.empty() && P.stepKeyLower == ABORTKEY)
                m_warnings.push_back(std::format("step '{}' of chord '{}' uses the abort key ({}); aborting from that position won't work", P.stepKeyLower, chord.repr,
                                                 m_abortKey->value()));
        }
    }

    for (const auto& S : submapsToCreate)
        ensureSubmapMachinery(S);

    m_rootSteps.push_back(chord.steps.front());
    m_chords.push_back(chord);

    return std::nullopt;
}

void CChordManager::ensureSubmapMachinery(const std::string& submapName) {
    if (m_submaps.contains(submapName) && m_submaps[submapName].hasMachinery)
        return;

    m_submaps[submapName].hasMachinery = true;

    SKeybind abortBind;
    abortBind.key            = m_abortKey ? std::string{m_abortKey->value()} : "Escape";
    abortBind.ignoreMods     = true;
    abortBind.handler        = "hyprchords_abort";
    abortBind.arg            = "key";
    abortBind.submap         = SSubmap{.name = submapName, .reset = ""};
    abortBind.description    = "hyprchords: abort chain";
    abortBind.hasDescription = true;
    abortBind.displayKey     = "hyprchords:abort:" + submapName;
    addBind(abortBind);

    SKeybind catchallBind;
    catchallBind.catchAll       = true;
    catchallBind.ignoreMods     = true;
    catchallBind.handler        = "hyprchords_abort";
    catchallBind.arg            = "catchall";
    catchallBind.submap         = SSubmap{.name = submapName, .reset = ""};
    catchallBind.description    = "hyprchords: abort chain (unmatched key)";
    catchallBind.hasDescription = true;
    catchallBind.displayKey     = "hyprchords:catchall:" + submapName;
    addBind(catchallBind);
}

SP<SKeybind> CChordManager::addBind(SKeybind bind) {
    auto sp = g_pKeybindManager->addKeybind(std::move(bind));
    if (sp)
        m_displayKeys.push_back(sp->displayKey);
    return sp;
}

SDispatchResult CChordManager::enter(const std::string& submapName) {
    auto res = Config::Actions::setSubmap(submapName);
    if (!res)
        return SDispatchResult{.success = false, .error = "hyprchords: " + res.error().message};

    armEventGuard();

    const auto TIMEOUT = m_timeout ? m_timeout->value() : 0;
    if (m_timer) {
        if (TIMEOUT > 0)
            m_timer->updateTimeout(std::chrono::milliseconds(TIMEOUT));
        else
            m_timer->updateTimeout(std::nullopt);
    }

    return {};
}

SDispatchResult CChordManager::exec(const std::string& idxStr) {
    size_t idx = 0;
    try {
        idx = std::stoul(idxStr);
    } catch (...) { return SDispatchResult{.success = false, .error = "hyprchords: invalid chord index"}; }

    if (idx >= m_chords.size())
        return SDispatchResult{.success = false, .error = "hyprchords: stale chord index"};

    armEventGuard();

    // reset before dispatching, so a dispatcher that sets its own submap wins
    if (inOurSubmap())
        Config::Actions::setSubmap("reset");
    if (m_timer)
        m_timer->updateTimeout(std::nullopt);

    const auto& CHORD      = m_chords[idx];
    const auto  DISPATCHER = g_pKeybindManager->m_dispatchers.find(CHORD.dispatcher);
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end())
        return SDispatchResult{.success = false, .error = "hyprchords: no such dispatcher: " + CHORD.dispatcher};

    return DISPATCHER->second(CHORD.arg);
}

SDispatchResult CChordManager::abort(const std::string& mode) {
    if (!inOurSubmap())
        return {};

    if (mode == "catchall") {
        // the same key event already advanced or finished a chain: not a mismatch
        if (eventGuardMatches())
            return {};
        // a bare modifier press (e.g. holding SUPER for the next step) must not abort
        if (g_pKeybindManager->keycodeToModifier(Config::Actions::state()->m_lastCode))
            return {};
    }

    leaveOurSubmap();
    return {};
}

void CChordManager::onTimeout() {
    leaveOurSubmap();
}

void CChordManager::onPreReload() {
    // resetHLConfig() wipes all keybinds right after this; our keyword handler
    // re-adds them during the parse. Only our bookkeeping needs clearing.
    leaveOurSubmap();

    m_chords.clear();
    m_submaps.clear();
    m_bindOwners.clear();
    m_displayKeys.clear();
    m_rootSteps.clear();
    m_warnings.clear();
    m_eventGuard.valid = false;
}

void CChordManager::onReloaded() {
    // warn about chord prefixes that collide with regular root binds: both would fire
    for (const auto& STEP : m_rootSteps) {
        if (STEP.key.empty())
            continue;
        const auto SYM = xkb_keysym_from_name(STEP.key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        if (SYM == XKB_KEY_NoSymbol)
            continue;

        for (const auto& K : g_pKeybindManager->m_keybinds) {
            if (!K->enabled || K->mouse || !K->submap.name.empty() || K->modmask != STEP.modmask || K->key.empty() || K->handler.starts_with("hyprchords_"))
                continue;
            const auto KSYM = xkb_keysym_from_name(K->key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
            if (KSYM != XKB_KEY_NoSymbol && KSYM == SYM)
                m_warnings.push_back(std::format("chord prefix '{}' collides with an existing bind ({} {}); both will trigger", STEP.repr, K->handler, K->arg));
        }
    }

    constexpr size_t MAXNOTIFS = 3;
    for (size_t i = 0; i < m_warnings.size() && i < MAXNOTIFS; ++i)
        notify(m_warnings[i]);
    if (m_warnings.size() > MAXNOTIFS)
        notify(std::format("...and {} more warnings", m_warnings.size() - MAXNOTIFS));
    m_warnings.clear();
}

bool CChordManager::inOurSubmap() const {
    return g_pKeybindManager->getCurrentSubmap().name.starts_with(SUBMAP_PREFIX);
}

void CChordManager::leaveOurSubmap() {
    if (inOurSubmap())
        Config::Actions::setSubmap("reset");
    if (m_timer)
        m_timer->updateTimeout(std::nullopt);
}

void CChordManager::armEventGuard() {
    m_eventGuard = {.timeMs = Config::Actions::state()->m_timeLastMs, .code = Config::Actions::state()->m_lastCode, .valid = true};
}

bool CChordManager::eventGuardMatches() const {
    return m_eventGuard.valid && m_eventGuard.timeMs == Config::Actions::state()->m_timeLastMs && m_eventGuard.code == Config::Actions::state()->m_lastCode;
}
