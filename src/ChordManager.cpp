#include "ChordManager.hpp"
#include "SxhkdConverter.hpp"
#include "globals.hpp"

#include <src/managers/KeybindManager.hpp>
#include <src/managers/EventManager.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/config/shared/actions/ConfigActions.hpp>
#include <src/event/EventBus.hpp>
#include <src/SharedDefs.hpp>

#include <hyprutils/string/String.hpp>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

constexpr const char* SUBMAP_PREFIX = "hc:";

static std::string    lower(std::string s) {
    std::ranges::transform(s, s.begin(), ::tolower);
    return s;
}

static std::string upper(std::string s) {
    std::ranges::transform(s, s.begin(), ::toupper);
    return s;
}

static void notify(const std::string& msg) {
    HyprlandAPI::addNotificationV2(PHANDLE, {{"text", "[hyprchords] " + msg}, {"time", uint64_t{10000}}, {"color", CHyprColor{0}}, {"icon", ICON_WARNING}});
}

// status events on the IPC socket (socket2), sxhkd's status fifo equivalent:
// hyprchords>>fire,<chord>,<dispatcher>,<arg> | abort | timeout | lock,<submap> | enabled | disabled
static void ipcEvent(const std::string& data) {
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprchords", .data = data});
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

static Hyprlang::CParseResult sxhkdSourceHandler(const char* COMMAND, const char* VALUE) {
    return g_chordManager.onSxhkdSource(VALUE ? VALUE : "");
}

// ---- sxhkd-style sequence expansion: {a,b,c} groups, `_` = empty, X-Y ranges, \{ \} escapes ----

struct SExpansionPart {
    std::vector<std::string>              literals; // always groups.size() + 1
    std::vector<std::vector<std::string>> groups;
};

struct SExpandedChord {
    std::vector<std::string> lines;
    bool                     cycle = false; // command-only groups: variants cycle per press
};

static bool isEscapedBrace(const std::string& s, size_t i) {
    return s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '{' || s[i + 1] == '}');
}

static size_t findTopLevelComma(const std::string& s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (isEscapedBrace(s, i)) {
            i++;
            continue;
        }
        if (s[i] == '{')
            depth++;
        else if (s[i] == '}')
            depth = std::max(0, depth - 1);
        else if (s[i] == ',' && depth == 0)
            return i;
    }
    return std::string::npos;
}

static void pushElement(std::vector<std::string>& elems, const std::string& raw) {
    const auto E = Hyprutils::String::trim(raw);
    if (E == "_") {
        elems.push_back("");
        return;
    }
    // single-char alnum range, e.g. 1-9 or a-e
    if (E.size() == 3 && E[1] == '-' && E[0] <= E[2] && ((isdigit(E[0]) && isdigit(E[2])) || (isalpha(E[0]) && isalpha(E[2])))) {
        for (char c = E[0]; c <= E[2]; ++c)
            elems.emplace_back(1, c);
        return;
    }
    elems.push_back(E);
}

static std::expected<SExpansionPart, std::string> parseExpansionPart(const std::string& s) {
    SExpansionPart part;
    std::string    cur;

    for (size_t i = 0; i < s.size(); ++i) {
        if (isEscapedBrace(s, i)) {
            cur += s[i + 1];
            i++;
            continue;
        }
        if (s[i] == '{') {
            std::vector<std::string> elems;
            std::string              elem;
            size_t                   close = std::string::npos;

            for (size_t j = i + 1; j < s.size(); ++j) {
                if (isEscapedBrace(s, j)) {
                    elem += s[j + 1];
                    j++;
                    continue;
                }
                if (s[j] == '{')
                    return std::unexpected("nested '{'");
                if (s[j] == '}') {
                    close = j;
                    break;
                }
                if (s[j] == ',') {
                    pushElement(elems, elem);
                    elem.clear();
                    continue;
                }
                elem += s[j];
            }

            if (close == std::string::npos)
                return std::unexpected("unmatched '{'");
            pushElement(elems, elem);
            if (elems.empty())
                return std::unexpected("empty {} group");

            part.literals.push_back(cur);
            cur.clear();
            part.groups.push_back(elems);
            i = close;
            continue;
        }
        if (s[i] == '}')
            return std::unexpected("unmatched '}'");
        cur += s[i];
    }

    part.literals.push_back(cur);
    return part;
}

static std::string buildFromPart(const SExpansionPart& part, const std::vector<size_t>& idx) {
    std::string out = part.literals[0];
    for (size_t j = 0; j < part.groups.size(); ++j) {
        if (!idx.empty())
            out += part.groups[j][idx[j]];
        out += part.literals[j + 1];
    }
    return out;
}

// expands one chord line into its sequence-expanded variants. Group i of the key
// sequence pairs with group i of the command part; the product across groups is taken.
// Groups only in the command part make a cycling chord (variants alternate per press).
static std::expected<SExpandedChord, std::string> expandChordLine(const std::string& value) {
    constexpr size_t MAXEXPANSIONS = 512;

    const auto       COMMA = findTopLevelComma(value);
    if (COMMA == std::string::npos)
        return std::unexpected("expected: chord = STEP ; STEP ; ... , dispatcher , arg");

    auto seqPart = parseExpansionPart(value.substr(0, COMMA));
    if (!seqPart)
        return std::unexpected(seqPart.error() + " in the key sequence");
    auto restPart = parseExpansionPart(value.substr(COMMA + 1));
    if (!restPart)
        return std::unexpected(restPart.error() + " in the command");

    const size_t              K = seqPart->groups.size();
    const size_t              M = restPart->groups.size();
    const std::vector<size_t> NOIDX;

    if (K == 0) {
        if (M == 0)
            return SExpandedChord{.lines = {buildFromPart(*seqPart, NOIDX) + "," + buildFromPart(*restPart, NOIDX)}};

        // command-only groups: sxhkd cycling — all groups advance together, one step per press
        const size_t N = restPart->groups[0].size();
        for (const auto& G : restPart->groups) {
            if (G.size() != N)
                return std::unexpected("cycling command {} groups must all have the same number of elements");
        }
        if (N > MAXEXPANSIONS)
            return std::unexpected(std::format("cycle has more than {} variants", MAXEXPANSIONS));

        SExpandedChord out{.lines = {}, .cycle = N > 1};
        const auto     SEQSTR = buildFromPart(*seqPart, NOIDX);
        for (size_t t = 0; t < N; ++t)
            out.lines.push_back(SEQSTR + "," + buildFromPart(*restPart, std::vector<size_t>(M, t)));
        return out;
    }

    if (M != 0 && M != K)
        return std::unexpected(std::format("mismatched {{}} group counts: {} in the key sequence but {} in the command", K, M));
    for (size_t j = 0; j < M; ++j) {
        if (restPart->groups[j].size() != seqPart->groups[j].size())
            return std::unexpected(std::format("{{}} group {} has {} elements in the key sequence but {} in the command", j + 1, seqPart->groups[j].size(),
                                               restPart->groups[j].size()));
    }

    size_t total = 1;
    for (const auto& G : seqPart->groups) {
        total *= G.size();
        if (total > MAXEXPANSIONS)
            return std::unexpected(std::format("expansion produces more than {} chords", MAXEXPANSIONS));
    }

    SExpandedChord      out;
    std::vector<size_t> idx(K, 0);
    while (true) {
        out.lines.push_back(buildFromPart(*seqPart, idx) + "," + buildFromPart(*restPart, M ? idx : NOIDX));

        size_t j = 0;
        for (; j < K; ++j) {
            if (++idx[j] < seqPart->groups[j].size())
                break;
            idx[j] = 0;
        }
        if (j == K)
            break;
    }

    return out;
}

// ---- step parsing helpers ----

// splits the key sequence on ';' and ':' separators. bool = the separator BEFORE the
// step was ':' (locked chain). ':' right after "code"/"mouse" is part of the token.
static std::vector<std::pair<std::string, bool>> splitSteps(const std::string& seq) {
    std::vector<std::pair<std::string, bool>> out;
    size_t                                    start    = 0;
    bool                                      lockNext = false;

    for (size_t i = 0; i < seq.size(); ++i) {
        const char C = seq[i];
        if (C != ';' && C != ':')
            continue;

        if (C == ':') {
            std::string word;
            for (size_t j = i; j > start;) {
                --j;
                if (isalpha(static_cast<unsigned char>(seq[j])))
                    word.insert(word.begin(), seq[j]);
                else
                    break;
            }
            if (word == "code" || word == "mouse")
                continue;
        }

        out.emplace_back(Hyprutils::String::trim(seq.substr(start, i - start)), lockNext);
        lockNext = C == ':';
        start    = i + 1;
    }

    out.emplace_back(Hyprutils::String::trim(seq.substr(start)), lockNext);
    return out;
}

// sxhkd modifier names Hyprland's stringToModMask doesn't know
static uint32_t modTokenToMask(const std::string& token) {
    const auto T = upper(token);
    if (T == "LOCK")
        return g_pKeybindManager->stringToModMask("CAPS");
    if (T == "HYPER")
        return g_pKeybindManager->stringToModMask("MOD3");
    if (T == "MODE_SWITCH")
        return g_pKeybindManager->stringToModMask("MOD5");
    return g_pKeybindManager->stringToModMask(T);
}

// sxhkd button1..button24 -> Hyprland mouse:NNN (linux BTN_* codes)
static std::expected<std::string, std::string> buttonToMouseKey(const std::string& token) {
    int n = 0;
    try {
        n = std::stoi(token.substr(6));
    } catch (...) { return std::unexpected("invalid button: " + token); }

    switch (n) {
        case 1: return "mouse:272"; // left
        case 2: return "mouse:274"; // middle
        case 3: return "mouse:273"; // right
        case 8: return "mouse:275"; // back / side
        case 9: return "mouse:276"; // forward / extra
        case 4:
        case 5:
        case 6:
        case 7: return std::unexpected(token + ": scroll wheel events cannot be chord steps");
        default: return std::unexpected("unsupported button: " + token + " (use mouse:NNN with a linux BTN code)");
    }
}

// ---- CChordManager ----

bool CChordManager::init(HANDLE handle) {
    m_timeout    = makeShared<Config::Values::CIntValue>("plugin:hyprchords:timeout", "pending chain timeout in ms, 0 disables the timeout", 3000,
                                                         Config::Values::SIntValueOptions{.min = 0});
    m_abortKey   = makeShared<Config::Values::CStringValue>("plugin:hyprchords:abort_key", "key that aborts a pending chain", "Escape");
    m_stickyMods = makeShared<Config::Values::CIntValue>("plugin:hyprchords:sticky_mods", "modless steps also match with the previous step's modifiers still held", 1,
                                                         Config::Values::SIntValueOptions{.min = 0, .max = 1});
    m_swallow    = makeShared<Config::Values::CIntValue>(
        "plugin:hyprchords:swallow", "1: unmatched keys abort a pending chain and are swallowed; 0: they pass to apps and the chain stays pending (sxhkd behavior)", 1,
        Config::Values::SIntValueOptions{.min = 0, .max = 1});

    if (!HyprlandAPI::addConfigValueV2(handle, m_timeout) || !HyprlandAPI::addConfigValueV2(handle, m_abortKey) || !HyprlandAPI::addConfigValueV2(handle, m_stickyMods) ||
        !HyprlandAPI::addConfigValueV2(handle, m_swallow))
        return false;

    // no V2 equivalent exists for plugin config keywords yet
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (!HyprlandAPI::addConfigKeyword(handle, "plugin:hyprchords:chord", chordKeywordHandler, Hyprlang::SHandlerOptions{}) ||
        !HyprlandAPI::addConfigKeyword(handle, "plugin:hyprchords:sxhkd_source", sxhkdSourceHandler, Hyprlang::SHandlerOptions{}))
        return false; // e.g. Lua config backend, where plugin keywords are unsupported
#pragma GCC diagnostic pop

    if (!HyprlandAPI::addDispatcherV2(handle, "hyprchords_enter", [](std::string arg) { return g_chordManager.enter(arg); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprchords_exec", [](std::string arg) { return g_chordManager.exec(arg); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprchords_abort", [](std::string arg) { return g_chordManager.abort(arg); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprchords_toggle", [](std::string arg) { return g_chordManager.toggle(arg); }) ||
        !HyprlandAPI::addDispatcherV2(handle, "hyprchords_import", [](std::string arg) { return g_chordManager.importSxhkd(arg); }))
        return false;

    m_timer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { g_chordManager.onTimeout(); }, nullptr);
    g_pEventLoopManager->addTimer(m_timer);

    m_preReloadListener = Event::bus()->m_events.config.preReload.listen([this] { onPreReload(); });
    m_reloadedListener  = Event::bus()->m_events.config.reloaded.listen([this] { onReloaded(); });
    m_submapListener    = Event::bus()->m_events.keybinds.submap.listen([this](const std::string& submap) {
        if (!submap.starts_with(SUBMAP_PREFIX)) {
            m_locked = false;
            if (m_timer)
                m_timer->updateTimeout(std::nullopt);
        }
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
    m_binds.clear();
    m_rootSteps.clear();
    m_warnings.clear();
}

Hyprlang::CParseResult CChordManager::onChordKeyword(const std::string& value) {
    Hyprlang::CParseResult result;
    if (const auto ERR = addChordLine(value))
        result.setError(("hyprchords: " + *ERR).c_str());
    return result;
}

// registers every hotkey of an sxhkdrc as an exec chord (commands verbatim). Problems
// don't fail the config: they surface as post-reload notifications like other warnings.
Hyprlang::CParseResult CChordManager::onSxhkdSource(const std::string& value) {
    Hyprlang::CParseResult result;

    const auto             CONVERTED = SxhkdConverter::convertFile(Hyprutils::String::trim(value));
    if (!CONVERTED) {
        result.setError(("hyprchords: sxhkd_source: " + CONVERTED.error()).c_str());
        return result;
    }

    for (const auto& WARNING : CONVERTED->warnings)
        m_warnings.push_back("sxhkd_source " + WARNING);

    for (const auto& ITEM : CONVERTED->items) {
        if (ITEM.type != SxhkdConverter::eItemType::CHORD)
            continue;
        if (const auto ERR = addChordLine(ITEM.text))
            m_warnings.push_back(std::format("sxhkd_source line {}: {}", ITEM.srcLine, *ERR));
    }

    return result;
}

std::optional<std::string> CChordManager::addChordLine(const std::string& value) {
    const auto EXPANDED = expandChordLine(value);
    if (!EXPANDED)
        return EXPANDED.error();

    if (EXPANDED->cycle) {
        // same key sequence, cycling command variants
        SChord base;
        for (size_t i = 0; i < EXPANDED->lines.size(); ++i) {
            auto parsed = parseChordLine(EXPANDED->lines[i]);
            if (!parsed)
                return std::format("{} (cycle variant '{}')", parsed.error(), Hyprutils::String::trim(EXPANDED->lines[i]));
            if (i == 0)
                base = *parsed;
            base.cycle.emplace_back(parsed->dispatcher, parsed->arg);
        }
        return registerChord(base);
    }

    for (const auto& LINE : EXPANDED->lines) {
        const auto CONTEXT = EXPANDED->lines.size() > 1 ? std::format(" (expanded to '{}')", Hyprutils::String::trim(LINE)) : "";

        auto       parsed = parseChordLine(LINE);
        if (!parsed)
            return parsed.error() + CONTEXT;

        if (const auto ERR = registerChord(*parsed))
            return *ERR + CONTEXT;
    }

    return std::nullopt;
}

std::expected<SChord, std::string> CChordManager::parseChordLine(const std::string& value) {
    // STEP (;|:) STEP ... , dispatcher , arg   — braces are already expanded here
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

    for (const auto& [RAWSTEP, LOCK] : splitSteps(SEQ)) {
        if (RAWSTEP.empty())
            return std::unexpected("empty step in chord sequence");

        SChordStep step;
        step.lockChain = LOCK;

        auto tokens = splitOn(RAWSTEP, '+');
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].empty())
                return std::unexpected("empty modifier in step '" + RAWSTEP + "'");
            if (upper(tokens[i]) == "ANY") {
                step.anyMods = true;
                step.hasMods = true;
                continue;
            }
            const auto MASK = modTokenToMask(tokens[i]);
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

        const auto KEYLOWER = lower(keyTok);
        if (KEYLOWER.starts_with("button")) {
            auto mapped = buttonToMouseKey(KEYLOWER);
            if (!mapped)
                return std::unexpected(mapped.error());
            step.key = *mapped;
        } else if (KEYLOWER.starts_with("mouse:")) {
            try {
                if (std::stoul(KEYLOWER.substr(6)) == 0)
                    return std::unexpected("invalid mouse button: " + keyTok);
            } catch (...) { return std::unexpected("invalid mouse button: " + keyTok); }
            step.key = KEYLOWER;
        } else if (KEYLOWER.starts_with("code:")) {
            try {
                step.keycode = std::stoul(keyTok.substr(5));
            } catch (...) { return std::unexpected("invalid keycode in step '" + RAWSTEP + "'"); }
            if (!step.keycode)
                return std::unexpected("invalid keycode in step '" + RAWSTEP + "'");
        } else {
            if (keyTok.contains(':'))
                return std::unexpected("unexpected ':' in key '" + keyTok + "'");
            if (xkb_keysym_from_name(keyTok.c_str(), XKB_KEYSYM_NO_FLAGS) == XKB_KEY_NoSymbol &&
                xkb_keysym_from_name(keyTok.c_str(), XKB_KEYSYM_CASE_INSENSITIVE) == XKB_KEY_NoSymbol)
                return std::unexpected("unknown key: " + keyTok);
            step.key = keyTok;
        }

        // sequence identity: mods + key, lowercase, prefixes excluded
        std::string repr;
        for (size_t i = 0; i + 1 < tokens.size(); ++i)
            repr += lower(tokens[i]) + "+";
        repr += KEYLOWER;
        step.repr = repr;

        chord.steps.push_back(step);
    }

    if (chord.steps.empty())
        return std::unexpected("empty chord sequence");
    if (chord.steps.front().lockChain)
        return std::unexpected("a chord cannot start with ':'");

    chord.lockOnFire = chord.steps.back().lockChain;

    std::string repr;
    for (size_t i = 0; i < chord.steps.size(); ++i)
        repr += (i == 0 ? "" : chord.steps[i].lockChain ? " : " : " ; ") + chord.steps[i].repr;
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
        const std::string ARG     = FINAL ? std::to_string(IDX) : (STEP.lockChain ? "+" : "") + NEXTSUBMAP;
        const std::string OWNER   = HANDLER + "|" + ARG;

        // (modmask, ignoreMods) bind variants for this step
        std::vector<std::pair<uint32_t, bool>> variants;
        if (STEP.anyMods)
            variants = {{0, true}};
        else {
            variants = {{STEP.hasMods ? STEP.modmask : 0, false}};
            if (!STEP.hasMods && STICKY && stickyMask)
                variants.emplace_back(stickyMask, false);
            if (STEP.hasMods)
                stickyMask = STEP.modmask;
        }

        for (const auto& [MASK, ANYMODS] : variants) {
            const auto IDENTITY = std::format("{}|{}|{}|{}|{}", BINDSUBMAP, ANYMODS ? "any" : std::to_string(MASK), lower(STEP.key), STEP.keycode, STEP.release);

            if (const auto IT = m_bindOwners.find(IDENTITY); IT != m_bindOwners.end()) {
                if (IT->second == OWNER)
                    continue; // shared prefix with an already registered chord
                return std::format("chord '{}' is ambiguous at step {} ('{}'): the same key already continues or finishes another chord in this position "
                                   "(a chord cannot be a prefix of another chord)",
                                   chord.repr, i + 1, STEP.repr);
            }

            SKeybind bind;
            bind.key          = STEP.key;
            bind.keycode      = STEP.keycode;
            bind.modmask      = MASK;
            bind.ignoreMods   = ANYMODS;
            bind.handler      = HANDLER;
            bind.arg          = ARG;
            bind.submap       = SSubmap{.name = BINDSUBMAP, .reset = ""};
            bind.release      = STEP.release;
            bind.nonConsuming = STEP.passthrough;
            bind.enabled      = m_enabled;
            bind.description  = FINAL ? (chord.cycle.size() > 1 ? std::format("hyprchords: {} -> cycles {} commands", chord.repr, chord.cycle.size()) :
                                                                  std::format("hyprchords: {} -> {} {}", chord.repr, chord.dispatcher, chord.arg)) :
                                        std::format("hyprchords: chain {} ...", prefix);
            bind.hasDescription = true;
            bind.displayKey     = std::format("hyprchords:{}:{}:{}", IDX, i, ANYMODS ? "any" : std::to_string(MASK));

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
    abortBind.enabled        = m_enabled;
    abortBind.description    = "hyprchords: abort chain";
    abortBind.hasDescription = true;
    abortBind.displayKey     = "hyprchords:abort:" + submapName;
    addBind(abortBind);

    if (m_swallow && !m_swallow->value())
        return; // sxhkd behavior: unmatched keys pass through, chain stays pending

    SKeybind catchallBind;
    catchallBind.catchAll       = true;
    catchallBind.ignoreMods     = true;
    catchallBind.handler        = "hyprchords_abort";
    catchallBind.arg            = "catchall";
    catchallBind.submap         = SSubmap{.name = submapName, .reset = ""};
    catchallBind.enabled        = m_enabled;
    catchallBind.description    = "hyprchords: abort chain (unmatched key)";
    catchallBind.hasDescription = true;
    catchallBind.displayKey     = "hyprchords:catchall:" + submapName;
    addBind(catchallBind);
}

SP<SKeybind> CChordManager::addBind(SKeybind bind) {
    auto sp = g_pKeybindManager->addKeybind(std::move(bind));
    if (sp) {
        m_displayKeys.push_back(sp->displayKey);
        m_binds.push_back(sp);
    }
    return sp;
}

SDispatchResult CChordManager::enter(const std::string& arg) {
    const bool LOCKS = arg.starts_with("+");
    const auto NAME  = LOCKS ? arg.substr(1) : arg;

    auto       res = Config::Actions::setSubmap(NAME);
    if (!res)
        return SDispatchResult{.success = false, .error = "hyprchords: " + res.error().message};

    armEventGuard();

    if (LOCKS && !m_locked) {
        m_locked = true;
        ipcEvent("lock," + NAME);
    }

    const auto TIMEOUT = m_timeout ? m_timeout->value() : 0;
    if (m_timer) {
        if (!m_locked && TIMEOUT > 0)
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

    auto& CHORD = m_chords[idx];

    armEventGuard();

    if (m_locked || CHORD.lockOnFire) {
        // locked mode: stay resident at the chain tail, no timeout; only the abort key exits
        if (!m_locked) {
            m_locked = true;
            ipcEvent("lock," + g_pKeybindManager->getCurrentSubmap().name);
        }
        if (m_timer)
            m_timer->updateTimeout(std::nullopt);
    } else {
        // reset before dispatching, so a dispatcher that sets its own submap wins
        if (inOurSubmap())
            (void)Config::Actions::setSubmap("reset");
        if (m_timer)
            m_timer->updateTimeout(std::nullopt);
    }

    std::string dispatcher = CHORD.dispatcher;
    std::string arg        = CHORD.arg;
    if (!CHORD.cycle.empty()) {
        const auto& VARIANT = CHORD.cycle[CHORD.cyclePos % CHORD.cycle.size()];
        dispatcher          = VARIANT.first;
        arg                 = VARIANT.second;
        CHORD.cyclePos      = (CHORD.cyclePos + 1) % CHORD.cycle.size();
    }

    ipcEvent(std::format("fire,{},{},{}", CHORD.repr, dispatcher, arg));

    const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find(dispatcher);
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end())
        return SDispatchResult{.success = false, .error = "hyprchords: no such dispatcher: " + dispatcher};

    return DISPATCHER->second(arg);
}

SDispatchResult CChordManager::abort(const std::string& mode) {
    if (!inOurSubmap())
        return {};

    if (mode == "catchall") {
        // the same event already advanced or finished a chain: not a mismatch
        if (eventGuardMatches())
            return {};
        // locked mode: unmatched keys don't abort; only the abort key exits
        if (m_locked)
            return {};
        // a bare modifier press (e.g. holding SUPER for the next step) must not abort
        if (g_pKeybindManager->keycodeToModifier(Config::Actions::state()->m_lastCode))
            return {};
    }

    ipcEvent("abort");
    leaveOurSubmap();
    return {};
}

SDispatchResult CChordManager::toggle(const std::string& arg) {
    if (arg == "on" || arg == "1" || arg == "enable")
        m_enabled = true;
    else if (arg == "off" || arg == "0" || arg == "disable")
        m_enabled = false;
    else
        m_enabled = !m_enabled;

    std::erase_if(m_binds, [](const auto& wp) { return wp.expired(); });
    for (auto& WPBIND : m_binds) {
        if (auto BIND = WPBIND.lock())
            BIND->enabled = m_enabled;
    }

    if (!m_enabled)
        leaveOurSubmap();

    ipcEvent(m_enabled ? "enabled" : "disabled");
    return {};
}

// hyprchords_import "SRC DST": one-shot sxhkdrc -> hyprchords config file conversion
SDispatchResult CChordManager::importSxhkd(const std::string& arg) {
    const auto TRIMMED = Hyprutils::String::trim(arg);
    const auto SPACE   = TRIMMED.find(' ');
    if (SPACE == std::string::npos)
        return {.success = false, .error = "usage: hyprchords_import <sxhkdrc> <output.conf>"};

    const auto SRC       = Hyprutils::String::trim(TRIMMED.substr(0, SPACE));
    const auto DST       = SxhkdConverter::expandTilde(Hyprutils::String::trim(TRIMMED.substr(SPACE + 1)));
    const auto CONVERTED = SxhkdConverter::convertFile(SRC);
    if (!CONVERTED)
        return {.success = false, .error = "hyprchords_import: " + CONVERTED.error()};

    std::ofstream out(DST);
    if (!out)
        return {.success = false, .error = "hyprchords_import: cannot write " + DST};
    out << SxhkdConverter::serialize(*CONVERTED, SRC);

    notify(std::format("imported {} chords from {} into {}; add 'source = {}' to hyprland.conf", CONVERTED->chords, SRC, DST, DST));
    constexpr size_t MAXNOTIFS = 3;
    for (size_t i = 0; i < CONVERTED->warnings.size() && i < MAXNOTIFS; ++i)
        notify(CONVERTED->warnings[i]);
    if (CONVERTED->warnings.size() > MAXNOTIFS)
        notify(std::format("...and {} more warnings", CONVERTED->warnings.size() - MAXNOTIFS));

    return {};
}

void CChordManager::onTimeout() {
    if (m_locked)
        return;
    if (inOurSubmap())
        ipcEvent("timeout");
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
    m_binds.clear();
    m_rootSteps.clear();
    m_warnings.clear();
    m_eventGuard.valid = false;
}

void CChordManager::onReloaded() {
    // warn about chord prefixes that collide with regular root binds: both would fire
    for (const auto& STEP : m_rootSteps) {
        if (STEP.key.empty() || STEP.key.starts_with("mouse:"))
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
    m_locked = false;
    if (inOurSubmap())
        (void)Config::Actions::setSubmap("reset");
    if (m_timer)
        m_timer->updateTimeout(std::nullopt);
}

void CChordManager::armEventGuard() {
    m_eventGuard = {.timeMs    = Config::Actions::state()->m_timeLastMs,
                    .code      = Config::Actions::state()->m_lastCode,
                    .mouseCode = Config::Actions::state()->m_lastMouseCode,
                    .valid     = true};
}

bool CChordManager::eventGuardMatches() const {
    return m_eventGuard.valid && m_eventGuard.timeMs == Config::Actions::state()->m_timeLastMs && m_eventGuard.code == Config::Actions::state()->m_lastCode &&
        m_eventGuard.mouseCode == Config::Actions::state()->m_lastMouseCode;
}
