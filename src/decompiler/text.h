#pragma once

#include <optional>
#include <string>
#include <string_view>

// Formats bytes as a double-quoted Luau string literal. Control bytes use
// fixed-width decimal escapes, so a following digit can never extend them.
inline std::string quote(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out += '"';
    for (unsigned char c : value)
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 32 || c == 127)
            {
                const char escape[] = {'\\', char('0' + c / 100), char('0' + c / 10 % 10), char('0' + c % 10)};
                out.append(escape, sizeof(escape));
            }
            else out += char(c);
        }
    out += '"';
    return out;
}

// The bytes of a literal produced by quote().
inline std::optional<std::string> unquote(std::string_view text)
{
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return std::nullopt;
    std::string out;
    for (size_t i = 1; i + 1 < text.size(); ++i)
    {
        if (text[i] != '\\') { out += text[i]; continue; }
        if (++i + 1 >= text.size()) return std::nullopt;
        switch (text[i])
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '"': case '\\': out += text[i]; break;
        default:
            if (i + 3 >= text.size()) return std::nullopt;
            out += char((text[i] - '0') * 100 + (text[i + 1] - '0') * 10 + (text[i + 2] - '0'));
            i += 2;
        }
    }
    return out;
}

inline bool isIdentifierStart(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool isIdentifierChar(unsigned char c) { return isIdentifierStart(c) || (c >= '0' && c <= '9'); }
