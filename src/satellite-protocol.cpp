/**
 * @file satellite-protocol.cpp
 * @brief Transport-independent codec for the Companion Satellite API wire protocol
 */

#include "satellite-protocol.hpp"

#include <cmath>
#include <cstdlib>

namespace satellite_protocol
{

namespace
{

char toLowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/** Keys Companion drops while parsing, as they could pollute a JavaScript prototype chain */
bool isBannedKey(std::string_view key)
{
    static constexpr std::string_view kBanned[] = {
        "__proto__", "constructor", "prototype", "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__"};
    for (std::string_view banned : kBanned)
    {
        if (key == banned)
        {
            return true;
        }
    }
    return false;
}

std::string_view trimJsWhitespace(std::string_view value)
{
    auto isSpace = [](char c)
    { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    while (!value.empty() && isSpace(value.front()))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && isSpace(value.back()))
    {
        value.remove_suffix(1);
    }
    return value;
}

constexpr int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

uint8_t clampComponent(long value)
{
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

} // namespace

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++)
    {
        if (toLowerAscii(a[i]) != toLowerAscii(b[i]))
        {
            return false;
        }
    }
    return true;
}

std::optional<double> parseNumber(std::string_view value)
{
    value = trimJsWhitespace(value);
    if (value.empty())
    {
        return 0.0; // Number('') === 0
    }

    std::string copy(value);
    char *end = nullptr;
    double result = strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size())
    {
        return std::nullopt;
    }
    return result;
}

bool isFalsey(const Param *param)
{
    if (!param)
    {
        return true;
    }
    if (param->isFlag)
    {
        return false;
    }
    return param->value.empty() || param->value == "0" || equalsIgnoreCase(param->value, "false");
}

bool isTruthy(const Param *param)
{
    if (isFalsey(param))
    {
        return false;
    }
    if (param->isFlag)
    {
        return true; // Number(true) === 1
    }
    if (equalsIgnoreCase(param->value, "true") || equalsIgnoreCase(param->value, "yes"))
    {
        return true;
    }
    std::optional<double> number = parseNumber(param->value);
    return number.has_value() && *number >= 1;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

Params Params::parseInPlace(char *body, size_t length)
{
    // A port of Companion's parseLineParameters. It walks the text once, splitting it into
    // space separated fragments (outside of quotes), dropping the quotes themselves and
    // replacing `\x` with `x`. The output is written back over the input, which is safe
    // as it is never longer than what has been read so far.
    struct Fragment
    {
        size_t start;
        size_t end;
    };
    std::vector<Fragment> fragments;

    size_t read = 0;
    size_t write = 0;
    size_t fragmentStart = 0;
    bool quoted = false;

    while (read < length)
    {
        char c = body[read];
        if (c == '\\')
        {
            if (read + 1 < length)
            {
                body[write++] = body[read + 1];
            }
            read += 2; // a dangling backslash is dropped
        }
        else if (c == '"')
        {
            quoted = !quoted;
            read++;
        }
        else if (c == ' ' && !quoted)
        {
            fragments.push_back({fragmentStart, write});
            fragmentStart = write;
            read++;
        }
        else
        {
            // Move a whole run of ordinary characters at once - a bitmap value is tens of KB
            // of them, which otherwise get shifted down one byte at a time after the first quote
            size_t runEnd = read + 1;
            while (runEnd < length && body[runEnd] != '\\' && body[runEnd] != '"' && (quoted || body[runEnd] != ' '))
            {
                runEnd++;
            }
            if (write != read)
            {
                memmove(body + write, body + read, runEnd - read);
            }
            write += runEnd - read;
            read = runEnd;
        }
    }
    fragments.push_back({fragmentStart, write});

    Params result;
    result._items.reserve(fragments.size());
    for (const Fragment &fragment : fragments)
    {
        std::string_view text(body + fragment.start, fragment.end - fragment.start);
        size_t equals = text.find('=');

        Param param;
        if (equals == std::string_view::npos)
        {
            if (text.empty() || isBannedKey(text))
            {
                continue;
            }
            param.key = text;
            param.isFlag = true;
        }
        else
        {
            param.key = text.substr(0, equals);
            if (param.key.empty() || isBannedKey(param.key))
            {
                continue;
            }
            param.value = text.substr(equals + 1);
        }
        result._items.push_back(param);
    }

    return result;
}

const Param *Params::find(std::string_view key) const
{
    // Search backwards, so the last occurrence of a repeated key wins
    for (auto it = _items.rbegin(); it != _items.rend(); ++it)
    {
        if (it->key == key)
        {
            return &*it;
        }
    }
    return nullptr;
}

std::optional<std::string_view> Params::get(std::string_view key) const
{
    const Param *param = find(key);
    if (!param || param->isFlag)
    {
        return std::nullopt;
    }
    return param->value;
}

Message::Message(std::string &&line)
    : _text(std::move(line))
{
    size_t space = _text.find(' ');
    if (space == std::string::npos)
    {
        _command = std::string_view(_text);
        return;
    }

    _command = std::string_view(_text.data(), space);
    _params = Params::parseInPlace(_text.data() + space + 1, _text.size() - space - 1);
}

bool Message::is(std::string_view command) const
{
    return equalsIgnoreCase(_command, command);
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

bool isValidKey(std::string_view key)
{
    if (key.empty())
    {
        return false;
    }
    for (char c : key)
    {
        bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid)
        {
            return false;
        }
    }
    return true;
}

bool isValidStringValue(std::string_view value)
{
    return value.find_first_of("\r\n") == std::string_view::npos;
}

MessageBuilder::MessageBuilder(std::string_view command, std::string_view status)
{
    _line.reserve(64);
    _line.append(command);
    if (!status.empty())
    {
        _line.push_back(' ');
        _line.append(status);
    }
}

bool MessageBuilder::appendKey(std::string_view key)
{
    if (!isValidKey(key))
    {
        _ok = false;
        return false;
    }
    _line.push_back(' ');
    _line.append(key);
    _line.push_back('=');
    return true;
}

MessageBuilder &MessageBuilder::deviceId(std::string_view id)
{
    // Companion omits DEVICEID entirely when it is empty
    if (!id.empty())
    {
        str("DEVICEID", id);
    }
    return *this;
}

MessageBuilder &MessageBuilder::str(std::string_view key, std::string_view value)
{
    if (!isValidStringValue(value))
    {
        _ok = false;
        return *this;
    }
    if (!appendKey(key))
    {
        return *this;
    }

    _line.reserve(_line.size() + value.size() + 2);
    _line.push_back('"');
    for (char c : value)
    {
        if (c == '\\' || c == '"')
        {
            _line.push_back('\\');
        }
        _line.push_back(c);
    }
    _line.push_back('"');
    return *this;
}

MessageBuilder &MessageBuilder::num(std::string_view key, long long value)
{
    if (appendKey(key))
    {
        _line.append(std::to_string(value));
    }
    return *this;
}

MessageBuilder &MessageBuilder::boolean(std::string_view key, bool value)
{
    if (appendKey(key))
    {
        _line.push_back(value ? '1' : '0');
    }
    return *this;
}

MessageBuilder &MessageBuilder::raw(std::string_view key, std::string_view value)
{
    if (value.find_first_of(" \"\\\r\n") != std::string_view::npos)
    {
        _ok = false;
        return *this;
    }
    if (appendKey(key))
    {
        _line.append(value);
    }
    return *this;
}

std::string MessageBuilder::finish()
{
    // Companion joins its chunks with spaces, the last being the newline, so every line
    // ends with " \n". Matching it exactly keeps the conformance corpus happy.
    _line.append(" \n");
    return std::move(_line);
}

// ---------------------------------------------------------------------------
// Versions and features
// ---------------------------------------------------------------------------

std::optional<ApiVersion> ApiVersion::parse(std::string_view text)
{
    text = trimJsWhitespace(text);

    uint32_t parts[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3; i++)
    {
        size_t start = pos;
        uint32_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
        {
            value = value * 10 + static_cast<uint32_t>(text[pos] - '0');
            if (value > UINT16_MAX)
            {
                return std::nullopt;
            }
            pos++;
        }
        if (pos == start)
        {
            return std::nullopt;
        }
        parts[i] = value;

        if (i < 2)
        {
            if (pos >= text.size() || text[pos] != '.')
            {
                return std::nullopt;
            }
            pos++;
        }
    }

    // Anything after the patch must be a prerelease (-) or build (+) suffix
    if (pos < text.size() && text[pos] != '-' && text[pos] != '+')
    {
        return std::nullopt;
    }

    return ApiVersion{static_cast<uint16_t>(parts[0]), static_cast<uint16_t>(parts[1]), static_cast<uint16_t>(parts[2])};
}

int ApiVersion::compare(const ApiVersion &other) const
{
    if (major != other.major)
        return major < other.major ? -1 : 1;
    if (minor != other.minor)
        return minor < other.minor ? -1 : 1;
    if (patch != other.patch)
        return patch < other.patch ? -1 : 1;
    return 0;
}

const FeatureDefinition &featureDefinition(Feature feature)
{
    static const FeatureDefinition kFeatures[] = {
        {"lockState", {1, 8, 0}, nullptr},
        {"surfaceManifest", {1, 9, 0}, nullptr},
        {"deviceSerial", {1, 10, 0}, nullptr},
        {"caps", {1, 10, 0}, nullptr},
        {"subscriptions", {1, 10, 0}, "SUBSCRIPTIONS"},
        {"keyStateLocation", {1, 10, 0}, nullptr},
        {"configFields", {1, 10, 0}, nullptr},
        {"changePage", {1, 10, 0}, nullptr},
        {"firmwareUpdateInfo", {1, 10, 0}, nullptr},
        {"nonSquareBitmaps", {1, 11, 0}, "NONSQUARE"},
        {"bitmapFormats", {1, 12, 0}, "BITMAP_FORMATS"},
        {"leds", {1, 13, 0}, nullptr},
        {"rotaryAmount", {1, 14, 0}, "ROTARY_AMOUNT"},
        {"appearance", {1, 15, 0}, nullptr},
    };
    static_assert(sizeof(kFeatures) / sizeof(kFeatures[0]) == static_cast<size_t>(Feature::Count), "feature table out of sync with Feature");

    return kFeatures[static_cast<size_t>(feature)];
}

bool isFeatureEnabled(Feature feature, const ApiVersion &apiVersion, const Params *caps)
{
    if (feature >= Feature::Count)
    {
        return false;
    }

    const FeatureDefinition &definition = featureDefinition(feature);
    if (!apiVersion.atLeast(definition.since))
    {
        return false;
    }
    if (definition.capsFlag == nullptr)
    {
        return true;
    }
    return caps != nullptr && !satellite_protocol::isFalsey(caps->find(definition.capsFlag));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::optional<Rgb> parseColor(std::string_view value)
{
    if (value.size() == 7 && value[0] == '#')
    {
        char hex[7];
        memcpy(hex, value.data() + 1, 6);
        hex[6] = '\0';
        char *end = nullptr;
        unsigned long rgb = strtoul(hex, &end, 16);
        if (end != hex + 6)
        {
            return std::nullopt;
        }
        return Rgb{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
    }

    if (value.size() > 5 && value.substr(0, 4) == "rgb(" && value.back() == ')')
    {
        std::string inner(value.substr(4, value.size() - 5));
        const char *p = inner.c_str();
        long components[3];
        for (int i = 0; i < 3; i++)
        {
            char *end = nullptr;
            components[i] = strtol(p, &end, 10);
            if (end == p)
            {
                return std::nullopt;
            }
            p = end;
            while (*p == ' ')
            {
                p++;
            }
            if (i < 2)
            {
                if (*p != ',')
                {
                    return std::nullopt;
                }
                p++;
            }
        }
        return Rgb{clampComponent(components[0]), clampComponent(components[1]), clampComponent(components[2])};
    }

    return std::nullopt;
}

size_t base64DecodedLength(std::string_view input)
{
    while (!input.empty() && input.back() == '=')
    {
        input.remove_suffix(1);
    }
    return (input.size() * 3) / 4;
}

namespace
{
/** base64Value() for every byte, -1 where it isn't a base64 character ('=' included) */
struct Base64Table
{
    int8_t values[256];
    constexpr Base64Table() : values()
    {
        for (int c = 0; c < 256; c++)
        {
            values[c] = static_cast<int8_t>(base64Value(static_cast<char>(c)));
        }
    }
};
constexpr Base64Table kBase64Table;
} // namespace

std::optional<size_t> base64Decode(std::string_view input, uint8_t *out, size_t capacity)
{
    uint32_t accumulator = 0;
    int bits = 0;
    size_t written = 0;
    bool padding = false;

    // Bitmaps are tens of KB of base64, so take whole groups of 4 valid characters at
    // a time; anything else (padding, bad characters, a full buffer) falls through to
    // the character-at-a-time loop below, which handles it exactly as before
    const uint8_t *in = reinterpret_cast<const uint8_t *>(input.data());
    size_t pos = 0;
    while (pos + 4 <= input.size() && written + 3 <= capacity)
    {
        int a = kBase64Table.values[in[pos]], b = kBase64Table.values[in[pos + 1]];
        int c = kBase64Table.values[in[pos + 2]], d = kBase64Table.values[in[pos + 3]];
        if ((a | b | c | d) < 0)
        {
            break;
        }
        uint32_t group = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) | (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
        out[written++] = static_cast<uint8_t>(group >> 16);
        out[written++] = static_cast<uint8_t>(group >> 8);
        out[written++] = static_cast<uint8_t>(group);
        pos += 4;
    }
    input.remove_prefix(pos);

    for (char c : input)
    {
        if (c == '=')
        {
            padding = true;
            continue;
        }
        int value = kBase64Table.values[static_cast<uint8_t>(c)];
        if (value < 0 || padding)
        {
            return std::nullopt;
        }

        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (written >= capacity)
            {
                return std::nullopt;
            }
            out[written++] = static_cast<uint8_t>(accumulator >> bits);
        }
    }

    return written;
}

std::optional<std::string> base64Decode(std::string_view input)
{
    std::string out(base64DecodedLength(input), '\0');
    std::optional<size_t> length = base64Decode(input, reinterpret_cast<uint8_t *>(out.data()), out.size());
    if (!length)
    {
        return std::nullopt;
    }
    out.resize(*length);
    return out;
}

std::string base64Encode(const uint8_t *data, size_t length)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    for (size_t i = 0; i < length; i += 3)
    {
        uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < length)
            chunk |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < length)
            chunk |= data[i + 2];

        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < length ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < length ? kAlphabet[chunk & 0x3f] : '=');
    }
    return out;
}

} // namespace satellite_protocol
