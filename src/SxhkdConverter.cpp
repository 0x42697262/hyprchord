#include "SxhkdConverter.hpp"

#include <hyprutils/string/String.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>

using Hyprutils::String::trim;
using enum SxhkdConverter::eItemType;

static bool escapedBrace(const std::string& s, size_t i) {
    return s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '{' || s[i + 1] == '}');
}

static std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), ::tolower);
    return s;
}

// sxhkd modifier spellings; also prettifies the common ones to the README's style
static const std::unordered_map<std::string, std::string> MODMAP = {
    {"super", "SUPER"}, {"alt", "ALT"},   {"meta", "ALT"},   {"shift", "SHIFT"}, {"ctrl", "CTRL"},   {"control", "CTRL"},
    {"mod1", "MOD1"},   {"mod2", "MOD2"}, {"mod3", "MOD3"},  {"mod4", "MOD4"},   {"mod5", "MOD5"},   {"hyper", "hyper"},
    {"lock", "lock"},   {"mode_switch", "mode_switch"},      {"any", "any"},
};

// strip all whitespace, then rename sxhkd modifier tokens (identifier directly followed by '+')
static std::string normalizeStep(const std::string& raw) {
    std::string s;
    for (const char C : raw) {
        if (!std::isspace((unsigned char)C))
            s += C;
    }

    const auto  IDENT = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    std::string out;
    size_t      i = 0;
    while (i < s.size()) {
        if (!IDENT(s[i])) {
            out += s[i++];
            continue;
        }
        size_t j = i;
        while (j < s.size() && IDENT(s[j]))
            j++;
        auto word = s.substr(i, j - i);
        if (j < s.size() && s[j] == '+') {
            if (const auto IT = MODMAP.find(lower(word)); IT != MODMAP.end())
                word = IT->second;
        }
        out += word;
        i = j;
    }
    return out;
}

// splits a hotkey on top-level ';'/':'. Returned char = the separator BEFORE the step, 0 for the first.
static std::vector<std::pair<std::string, char>> splitHotkey(const std::string& hk) {
    std::vector<std::pair<std::string, char>> steps;
    std::string                               cur;
    char                                      sep   = 0;
    int                                       depth = 0;

    for (size_t i = 0; i < hk.size(); ++i) {
        if (escapedBrace(hk, i)) {
            cur += hk[i];
            cur += hk[i + 1];
            i++;
            continue;
        }
        const char C = hk[i];
        if (C == '{')
            depth++;
        else if (C == '}')
            depth = std::max(0, depth - 1);
        else if ((C == ';' || C == ':') && depth == 0) {
            steps.emplace_back(cur, sep);
            sep = C;
            cur.clear();
            continue;
        }
        cur += C;
    }
    steps.emplace_back(cur, sep);
    return steps;
}

// {} groups with their elements, trimmed like the chord parser's pushElement
static std::vector<std::vector<std::string>> scanGroups(const std::string& s) {
    std::vector<std::vector<std::string>> groups;
    for (size_t i = 0; i < s.size(); ++i) {
        if (escapedBrace(s, i)) {
            i++;
            continue;
        }
        if (s[i] != '{')
            continue;

        std::vector<std::string> elems;
        std::string              elem;
        size_t                   j = i + 1;
        for (; j < s.size(); ++j) {
            if (escapedBrace(s, j)) {
                elem += s[j + 1];
                j++;
                continue;
            }
            if (s[j] == '}')
                break;
            if (s[j] == ',') {
                elems.push_back(trim(elem));
                elem.clear();
                continue;
            }
            elem += s[j];
        }
        elems.push_back(trim(elem));
        groups.push_back(elems);
        i = j;
    }
    return groups;
}

// element count after single-char range expansion ({1-9} = 9 elements)
static size_t elementCount(const std::vector<std::string>& elems) {
    size_t n = 0;
    for (const auto& E : elems) {
        if (E.size() == 3 && E[1] == '-' && E[0] <= E[2] && ((isdigit(E[0]) && isdigit(E[2])) || (isalpha(E[0]) && isalpha(E[2]))))
            n += E[2] - E[0] + 1;
        else
            n++;
    }
    return n;
}

static void lintGroups(const std::string& keys, const std::string& cmd, size_t line, std::vector<std::string>& warnings) {
    const auto KEYGROUPS = scanGroups(keys);
    const auto CMDGROUPS = scanGroups(cmd);

    for (const auto* GROUPS : {&KEYGROUPS, &CMDGROUPS}) {
        for (const auto& G : *GROUPS) {
            if (std::ranges::any_of(G, [](const auto& E) { return E.empty(); }))
                warnings.push_back(std::format("line {}: empty element in a {{}} group (did you mean '_', or a stray trailing comma?)", line));
        }
    }

    if (KEYGROUPS.empty() || CMDGROUPS.empty())
        return;
    if (CMDGROUPS.size() != KEYGROUPS.size()) {
        warnings.push_back(std::format("line {}: {} {{}} groups in the hotkey but {} in the command", line, KEYGROUPS.size(), CMDGROUPS.size()));
        return;
    }
    for (size_t i = 0; i < KEYGROUPS.size(); ++i) {
        if (elementCount(KEYGROUPS[i]) != elementCount(CMDGROUPS[i]))
            warnings.push_back(std::format("line {}: {{}} group {} has {} elements in the hotkey but {} in the command", line, i + 1, elementCount(KEYGROUPS[i]),
                                           elementCount(CMDGROUPS[i])));
    }
}

static std::string collapseWs(const std::string& s) {
    std::string out;
    bool        ws = false;
    for (const char C : s) {
        if (std::isspace((unsigned char)C)) {
            ws = true;
            continue;
        }
        if (ws && !out.empty())
            out += ' ';
        ws = false;
        out += C;
    }
    return out;
}

SxhkdConverter::SResult SxhkdConverter::convert(const std::string& text) {
    SResult                  res;

    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string        l;
        while (std::getline(ss, l))
            lines.push_back(l);
    }

    struct SRaw {
        eItemType   type;
        std::string text; // comment or hotkey
        std::string cmd;
        bool        hasCmd = false;
        size_t      line   = 0;
    };
    std::vector<SRaw> raws;

    // joins backslash-continued physical lines into one logical line
    const auto        TAKELOGICAL = [&](size_t start, size_t& next) {
        std::string joined;
        size_t      j = start;
        while (j < lines.size()) {
            std::string l = lines[j++];
            while (!l.empty() && std::isspace((unsigned char)l.back()))
                l.pop_back();
            const bool CONT = !l.empty() && l.back() == '\\';
            if (CONT)
                l.pop_back();
            if (const auto T = trim(l); !T.empty())
                joined += (joined.empty() ? "" : " ") + T;
            if (!CONT)
                break;
        }
        next = j;
        return joined;
    };

    for (size_t i = 0; i < lines.size();) {
        const auto& RAW = lines[i];
        const auto  S   = trim(RAW);
        if (S.empty()) {
            raws.push_back({.type = BLANK});
            i++;
            continue;
        }
        if (S[0] == '#') {
            raws.push_back({.type = COMMENT, .text = S});
            i++;
            continue;
        }

        const bool   INDENTED = RAW[0] == ' ' || RAW[0] == '\t';
        const size_t LINENO   = i + 1;
        const auto   LOGICAL  = TAKELOGICAL(i, i);

        if (!INDENTED) {
            raws.push_back({.type = CHORD, .text = LOGICAL, .line = LINENO});
            continue;
        }

        // command line: attach to the most recent hotkey
        const auto LAST = std::ranges::find_if(raws.rbegin(), raws.rend(), [](const SRaw& r) { return r.type == CHORD; });
        if (LAST == raws.rend())
            res.warnings.push_back(std::format("line {}: command line without a preceding hotkey, skipped", LINENO));
        else if (LAST->hasCmd)
            res.warnings.push_back(std::format("line {}: extra command line for one hotkey, skipped", LINENO));
        else {
            LAST->cmd    = LOGICAL;
            LAST->hasCmd = true;
        }
    }

    bool prevBlank = true; // collapse leading/duplicate blanks
    for (const auto& R : raws) {
        if (R.type == BLANK) {
            if (!prevBlank)
                res.items.push_back({.type = BLANK});
            prevBlank = true;
            continue;
        }
        prevBlank = false;
        if (R.type == COMMENT) {
            res.items.push_back({.type = COMMENT, .text = R.text});
            continue;
        }
        if (!R.hasCmd) {
            res.warnings.push_back(std::format("line {}: hotkey '{}' has no command, skipped", R.line, R.text));
            res.items.push_back({.type = COMMENT, .text = "# skipped (no command): " + R.text});
            continue;
        }

        lintGroups(R.text, R.cmd, R.line, res.warnings);

        auto steps = splitHotkey(R.text);
        while (steps.size() > 1 && trim(steps.back().first).empty()) {
            steps.pop_back();
            res.warnings.push_back(std::format("line {}: '{}': dropped trailing empty chain step", R.line, R.text));
        }

        std::string keys;
        for (const auto& [STEP, SEP] : steps) {
            if (SEP)
                keys += std::format(" {} ", SEP);
            keys += normalizeStep(STEP);
        }

        res.items.push_back({.type = CHORD, .text = keys + " , exec , " + collapseWs(R.cmd), .srcLine = R.line});
        res.chords++;
    }
    while (!res.items.empty() && res.items.back().type == BLANK)
        res.items.pop_back();

    return res;
}

std::expected<SxhkdConverter::SResult, std::string> SxhkdConverter::convertFile(const std::string& path) {
    std::ifstream f(expandTilde(path));
    if (!f)
        return std::unexpected("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return convert(ss.str());
}

std::string SxhkdConverter::serialize(const SResult& result, const std::string& sourcePath) {
    std::string out = std::format("# generated by hyprchords from {}\nplugin {{\n    hyprchords {{\n", sourcePath);
    for (const auto& IT : result.items) {
        switch (IT.type) {
            case BLANK: out += "\n"; break;
            case COMMENT: out += "        " + IT.text + "\n"; break;
            case CHORD: out += "        chord = " + IT.text + "\n"; break;
        }
    }
    return out + "    }\n}\n";
}

std::string SxhkdConverter::expandTilde(const std::string& path) {
    if (path.starts_with("~/")) {
        if (const char* HOME = std::getenv("HOME"))
            return HOME + path.substr(1);
    }
    return path;
}
