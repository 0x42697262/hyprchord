#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// sxhkdrc -> hyprchords "KEYS , exec , CMD" lines. Format-level translation only:
// {} groups, ranges and chain separators pass through unchanged — the chord parser
// implements sxhkd's expansion semantics natively. Commands are kept verbatim.
namespace SxhkdConverter {
    enum class eItemType : uint8_t {
        BLANK,
        COMMENT,
        CHORD,
    };

    struct SItem {
        eItemType   type;
        std::string text;        // comment (incl. '#') or chord value ("KEYS , exec , CMD")
        size_t      srcLine = 0; // sxhkdrc line number (CHORD only)
    };

    struct SResult {
        std::vector<SItem>       items;
        std::vector<std::string> warnings; // "line N: ..."
        size_t                   chords = 0;
    };

    SResult                             convert(const std::string& text);
    std::expected<SResult, std::string> convertFile(const std::string& path); // path may start with ~/
    std::string                         serialize(const SResult& result, const std::string& sourcePath);
    std::string                         expandTilde(const std::string& path);
}
