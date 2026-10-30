/**
 * @file satellite-protocol.hpp
 * @brief Transport-independent codec for the Companion Satellite API wire protocol
 *
 * A C++ port of the parts of Companion's `@companion-app/satellite-protocol` package
 * that a client needs: line splitting, parameter parsing and message formatting
 * (including the quoting/escaping rules), the boolean conventions, API version
 * comparison and the feature table.
 *
 * This file deliberately depends only on the C++ standard library (no Arduino, no
 * FreeRTOS), so it can be unit tested on a desktop machine against Companion's own
 * conformance corpus (see test/host).
 */

#ifndef COMPANION_SATELLITE_PROTOCOL_HPP
#define COMPANION_SATELLITE_PROTOCOL_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace satellite_protocol
{

/** @brief The newest Satellite API version this codec knows about */
constexpr const char *kKnownApiVersion = "1.15.0";

/** @brief The bitmap encodings defined by the protocol, in CAPS BITMAP_FORMATS order */
constexpr const char *kBitmapFormats[] = {"rgb", "png", "webp"};

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

/** @brief A parsed parameter. A parameter given without a value (`FLAG`) has isFlag set and an empty value */
struct Param
{
    std::string_view key;
    std::string_view value;
    bool isFlag = false;
};

/** @brief Satellite API falsey check: missing, empty, "0" or "false" (any case). A valueless flag is truthy */
bool isFalsey(const Param *param);

/** @brief Satellite API truthy check: not falsey, and "true"/"yes" (any case) or a number >= 1. A valueless flag is truthy */
bool isTruthy(const Param *param);

/** @brief Parse a whole string as a number, as JavaScript's Number() would for the values the protocol uses */
std::optional<double> parseNumber(std::string_view value);

/**
 * @brief The parameters of a message, parsed in place
 *
 * Keys and values are views into the buffer that was parsed, so a Params must not
 * outlive that buffer. When a key appears more than once, the last one wins (as it
 * does in Companion).
 */
class Params
{
public:
    /**
     * @brief Parse the parameters part of a line (everything after the command name)
     *
     * Rewrites the buffer in place to remove quotes and escapes - unescaping never
     * makes the text longer, so no allocation is needed for keys or values.
     */
    static Params parseInPlace(char *body, size_t length);

    /** @brief Find a parameter by key, or nullptr */
    const Param *find(std::string_view key) const;

    /** @brief Whether a parameter is present at all (with or without a value) */
    bool has(std::string_view key) const { return find(key) != nullptr; }

    /** @brief The value of a parameter which was given with a value */
    std::optional<std::string_view> get(std::string_view key) const;

    bool isTruthy(std::string_view key) const { return satellite_protocol::isTruthy(find(key)); }
    bool isFalsey(std::string_view key) const { return satellite_protocol::isFalsey(find(key)); }

    const std::vector<Param> &items() const { return _items; }
    size_t size() const { return _items.size(); }

private:
    std::vector<Param> _items;
};

/**
 * @brief A received message, owning its text
 *
 * Neither copyable nor movable, as the parsed params point into the owned text -
 * allocate it on the heap if it needs to be passed around.
 */
class Message
{
public:
    /** @brief Take ownership of a received line (without its newline) and parse it */
    explicit Message(std::string &&line);
    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;

    /** @brief The command name, as received */
    std::string_view command() const { return _command; }

    /** @brief Whether the command name matches, ignoring case (as Companion does) */
    bool is(std::string_view command) const;

    /** @brief A response carrying OK (eg `ADD-DEVICE OK DEVICEID="x"`) */
    bool isOk() const { return _params.has("OK"); }

    /** @brief A response carrying ERROR (eg `KEY-PRESS ERROR DEVICEID="x" MESSAGE="..."`) */
    bool isError() const { return _params.has("ERROR"); }

    const Params &params() const { return _params; }

    /** @brief Approximate heap footprint, for bounding queues */
    size_t memoryUsage() const { return _text.capacity() + _params.size() * sizeof(Param) + sizeof(*this); }

private:
    std::string _text;
    std::string_view _command;
    Params _params;
};

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

/**
 * @brief Builds an outgoing message line
 *
 * Mirrors Companion's formatSatelliteMessage: string values are quoted with `\` and `"`
 * escaped, numbers are bare and booleans are sent as 1/0. Anything that cannot be
 * represented on the wire (a newline in a value, or a key that isn't a plain
 * identifier) marks the message invalid rather than producing a corrupt line.
 */
class MessageBuilder
{
public:
    explicit MessageBuilder(std::string_view command, std::string_view status = {});

    MessageBuilder &deviceId(std::string_view id);
    MessageBuilder &str(std::string_view key, std::string_view value);
    MessageBuilder &num(std::string_view key, long long value);
    MessageBuilder &boolean(std::string_view key, bool value);

    /** @brief A bare (unquoted) value, for tokens like `BITMAPS=false`. Must not contain spaces, quotes, backslashes or newlines */
    MessageBuilder &raw(std::string_view key, std::string_view value);

    /** @brief Whether everything added so far could be represented */
    bool ok() const { return _ok; }

    /** @brief The finished line, including the terminating newline */
    std::string finish();

private:
    bool appendKey(std::string_view key);

    std::string _line;
    bool _ok = true;
};

/** @brief Whether a key can be sent as-is (letters, digits, `_` and `-`) */
bool isValidKey(std::string_view key);

/** @brief Whether a string can be sent as a quoted value (no CR/LF) */
bool isValidStringValue(std::string_view value);

// ---------------------------------------------------------------------------
// Line buffering
// ---------------------------------------------------------------------------

/**
 * @brief Splits a received byte stream into lines
 *
 * Unlike Companion's server, which drops the connection when a line is too long,
 * this skips the oversized line and resynchronises at the next newline - a newline
 * can never appear inside a message, so the stream stays parseable, and a device
 * can survive eg. a bitmap bigger than it has memory for.
 */
class LineBuffer
{
public:
    explicit LineBuffer(size_t maxLineLength)
        : _maxLineLength(maxLineLength) {}

    /**
     * @brief Add received data, calling onLine(std::string &&line) for each complete line
     *
     * Lines are passed without the newline or a trailing carriage return. Large lines
     * are handed over by moving the internal buffer, so they are not copied again.
     * @return the number of lines that were discarded for exceeding the maximum length
     */
    template <typename F>
    size_t push(const char *data, size_t length, F &&onLine);

    /** @brief Discard any partial line, eg. when the connection is reset */
    void clear()
    {
        _buffer.clear();
        _buffer.shrink_to_fit();
        _discarding = false;
    }

    void setMaxLineLength(size_t maxLineLength) { _maxLineLength = maxLineLength; }
    size_t maxLineLength() const { return _maxLineLength; }

    /** @brief Returns whether a buffer of this many bytes can be allocated right now */
    using AllocationCheck = bool (*)(size_t bytes);

    /**
     * @brief Check before growing the buffer, and skip the line instead if it says no
     *
     * Without exceptions (as on the ESP32) a failed allocation aborts, so a burst of large
     * lines arriving faster than they're handled would otherwise take the whole device down.
     */
    void setAllocationCheck(AllocationCheck check) { _canAllocate = check; }

    /** @brief How many of the lines push() has dropped were for lack of memory, resetting the count */
    size_t takeOutOfMemoryDrops()
    {
        size_t drops = _outOfMemoryDrops;
        _outOfMemoryDrops = 0;
        return drops;
    }

private:
    static constexpr size_t kSmallLine = 256;

    size_t _maxLineLength;
    std::string _buffer;
    bool _discarding = false;
    AllocationCheck _canAllocate = nullptr;
    size_t _outOfMemoryDrops = 0;
};

template <typename F>
size_t LineBuffer::push(const char *data, size_t length, F &&onLine)
{
    size_t dropped = 0;
    size_t pos = 0;

    while (pos < length)
    {
        const void *found = memchr(data + pos, '\n', length - pos);
        size_t end = found ? static_cast<const char *>(found) - data : length;

        if (_discarding)
        {
            if (found)
            {
                _discarding = false;
            }
            pos = found ? end + 1 : length;
            continue;
        }

        if (_buffer.size() + (end - pos) > _maxLineLength)
        {
            // Too long - drop what we have, and everything up to the next newline
            _buffer.clear();
            _buffer.shrink_to_fit();
            dropped++;
            _discarding = !found;
            pos = found ? end + 1 : length;
            continue;
        }

        size_t needed = _buffer.size() + (end - pos);
        if (needed > _buffer.capacity())
        {
            // Grow in steps of our choosing - doubling, but never past the line limit - and
            // only when the memory is there to grow into (+ 1 for the terminator)
            size_t grown = std::min(std::max(needed, _buffer.capacity() * 2), _maxLineLength);
            if (_canAllocate && !_canAllocate(grown + 1))
            {
                _buffer.clear();
                _buffer.shrink_to_fit();
                dropped++;
                _outOfMemoryDrops++;
                _discarding = !found;
                pos = found ? end + 1 : length;
                continue;
            }
            // Not _buffer.reserve(grown): libstdc++ rounds that up to twice the old capacity,
            // more than was checked. A fresh string reserves exactly what's asked.
            std::string bigger;
            bigger.reserve(grown);
            bigger.append(_buffer);
            _buffer.swap(bigger);
        }
        _buffer.append(data + pos, end - pos);
        if (!found)
        {
            break;
        }
        pos = end + 1;

        if (!_buffer.empty() && _buffer.back() == '\r')
        {
            _buffer.pop_back();
        }

        if (_buffer.size() <= kSmallLine)
        {
            // Copy small lines, so the (possibly large) buffer allocation is reused
            onLine(std::string(_buffer));
            _buffer.clear();
        }
        else
        {
            // Hand a large line over without copying it
            std::string line;
            line.swap(_buffer);
            onLine(std::move(line));
        }
    }

    return dropped;
}

// ---------------------------------------------------------------------------
// Versions and features
// ---------------------------------------------------------------------------

/** @brief A parsed `major.minor.patch` API version */
struct ApiVersion
{
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;

    /** @brief Parse `major.minor.patch`, ignoring any prerelease/build suffix (as Companion does) */
    static std::optional<ApiVersion> parse(std::string_view text);

    int compare(const ApiVersion &other) const;
    bool atLeast(const ApiVersion &other) const { return compare(other) >= 0; }
};

/**
 * @brief Features a client may need to check before using, from Companion's SATELLITE_FEATURES
 *
 * The API version says whether a feature exists; a CAPS flag (where there is one)
 * says whether it is enabled on this connection.
 */
enum class Feature : uint8_t
{
    LockState,          /**< 1.8.0 - the surface can draw the locked state itself (LOCKED-STATE) */
    SurfaceManifest,    /**< 1.9.0 - ADD-DEVICE accepts a LAYOUT_MANIFEST */
    DeviceSerial,       /**< 1.10.0 - ADD-DEVICE accepts a SERIAL separate from DEVICEID */
    Caps,               /**< 1.10.0 - CAPS is sent after BEGIN */
    Subscriptions,      /**< 1.10.0 + CAPS SUBSCRIPTIONS - button subscriptions */
    KeyStateLocation,   /**< 1.10.0 - KEY-STATE carries LOCATION */
    ConfigFields,       /**< 1.10.0 - CONFIG_FIELDS / DEVICE-CONFIG */
    ChangePage,         /**< 1.10.0 - CHANGE-PAGE */
    FirmwareUpdateInfo, /**< 1.10.0 - FIRMWARE-UPDATE-INFO */
    NonSquareBitmaps,   /**< 1.11.0 + CAPS NONSQUARE - non-square bitmaps */
    BitmapFormats,      /**< 1.12.0 + CAPS BITMAP_FORMATS - BITMAP_FORMAT negotiation */
    Leds,               /**< 1.13.0 - `leds` in style presets, streamed as LEDS */
    RotaryAmount,       /**< 1.14.0 + CAPS ROTARY_AMOUNT - signed step count in DIRECTION */
    Appearance,         /**< 1.15.0 - DEVICE-APPEARANCE */
    Count
};

struct FeatureDefinition
{
    const char *name;
    ApiVersion since;
    const char *capsFlag; /**< nullptr when the feature is always enabled once the API version has it */
};

/** @brief The definition of a feature */
const FeatureDefinition &featureDefinition(Feature feature);

/**
 * @brief Whether a feature is available on a connection
 * @param caps the CAPS parameters, or nullptr if CAPS has not been received
 */
bool isFeatureEnabled(Feature feature, const ApiVersion &apiVersion, const Params *caps);

// ---------------------------------------------------------------------------
// Helpers for common value types
// ---------------------------------------------------------------------------

/** @brief An 8-bit RGB color */
struct Rgb
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

/** @brief Parse `#rrggbb` or `rgb(r,g,b)` (the two COLORS formats Companion can send) */
std::optional<Rgb> parseColor(std::string_view value);

/** @brief The number of bytes base64Decode would produce (an upper bound if the input is malformed) */
size_t base64DecodedLength(std::string_view input);

/**
 * @brief Decode base64 into a caller-provided buffer
 * @return the number of bytes written, or std::nullopt if the input is malformed or doesn't fit
 */
std::optional<size_t> base64Decode(std::string_view input, uint8_t *out, size_t capacity);

/** @brief Decode base64 into a string, or std::nullopt if malformed */
std::optional<std::string> base64Decode(std::string_view input);

/** @brief Encode bytes as base64 */
std::string base64Encode(const uint8_t *data, size_t length);
inline std::string base64Encode(std::string_view data)
{
    return base64Encode(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

/** @brief ASCII case-insensitive comparison */
bool equalsIgnoreCase(std::string_view a, std::string_view b);

} // namespace satellite_protocol

#endif // COMPANION_SATELLITE_PROTOCOL_HPP
