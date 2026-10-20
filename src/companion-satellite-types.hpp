/**
 * @file companion-satellite-types.hpp
 * @brief Value types shared by Satellite, SatelliteSurface and SatelliteSubscription
 */

#ifndef COMPANION_SATELLITE_TYPES_HPP
#define COMPANION_SATELLITE_TYPES_HPP

#include <Arduino.h>
#include <optional>
#include <string_view>
#include <vector>

#include "satellite-protocol.hpp"

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/**
 * @brief Type of a key/button/control, as reported by KEY-STATE and SUB-STATE
 */
enum class SatelliteSurfaceKeyType
{
    BUTTON,   /**< Standard button key */
    PAGEUP,   /**< Page up key */
    PAGEDOWN, /**< Page down key */
    PAGENUM,  /**< Page number key */
};

/**
 * @brief Action/direction shared by button presses and encoder rotation
 *
 * PRESSED/RELEASED are press-style actions; RIGHT/LEFT rotate by a single step
 * (clockwise/counter-clockwise). To rotate by more than one step at once, use
 * SatelliteSurface::rotate or SatelliteSubscription::rotate.
 */
enum class SatelliteAction
{
    PRESSED,
    RELEASED,
    RIGHT,
    LEFT,
};

/**
 * @brief Direction for surface page navigation (CHANGE-PAGE)
 */
enum class SatellitePageDirection
{
    NEXT,
    PREVIOUS,
};

/**
 * @brief State of the connection to Companion
 */
enum class SatelliteConnectionState
{
    IDLE,        /**< begin() not called yet, or no target host/port configured */
    CONNECTING,  /**< Opening the TCP connection */
    NEGOTIATING, /**< Connected, waiting for BEGIN and CAPS */
    READY,       /**< BEGIN and CAPS received - surfaces and subscriptions are being registered */
    REJECTED,    /**< Companion's API version is too old. Retrying stops until the target changes */
    WAITING,     /**< Disconnected, waiting to retry */
};

/** @brief Human readable name of a connection state, for logging */
const char *satelliteConnectionStateName(SatelliteConnectionState state);

/**
 * @brief Features of the Satellite API that depend on Companion's version or settings
 *
 * See Satellite::hasFeature. The API version says whether a feature exists; where a
 * feature has a CAPS flag, that says whether it is enabled on this connection.
 */
using SatelliteFeature = satellite_protocol::Feature;

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

/**
 * @brief RGB color, each component 0-255
 */
struct SatelliteColor
{
    uint8_t r; /**< Red component (0-255) */
    uint8_t g; /**< Green component (0-255) */
    uint8_t b; /**< Blue component (0-255) */
};

/** @brief Previous name of SatelliteColor */
using color [[deprecated("use SatelliteColor")]] = SatelliteColor;

/**
 * @brief A base64 encoded buffer received from Companion (a BITMAP or LEDS value)
 *
 * This is a view into the received message - it is only valid until the callback it
 * was passed to returns. Decode it (or copy it) inside the callback.
 */
class SatelliteEncodedData
{
public:
    SatelliteEncodedData() = default;
    explicit SatelliteEncodedData(std::string_view value)
        : _value(value) {}

    /** @brief The value exactly as received */
    std::string_view raw() const { return _value; }

    /**
     * @brief Whether this is a `data:` url (a png or webp bitmap) rather than raw base64 rgb
     *
     * Only bitmaps can be data urls, and only when a BITMAP_FORMAT other than rgb was negotiated.
     */
    bool isDataUrl() const { return _value.substr(0, 5) == "data:"; }

    /** @brief The media type of a data url (eg "image/png"), or empty for raw rgb */
    std::string_view mimeType() const;

    /** @brief The base64 payload - the part after `base64,` for a data url, else the whole value */
    std::string_view base64() const;

    /** @brief The number of bytes decode() will produce */
    size_t decodedLength() const { return satellite_protocol::base64DecodedLength(base64()); }

    /**
     * @brief Decode the payload into a caller-provided buffer
     *
     * For raw rgb bitmaps and LEDS this is packed 8-bit R,G,B triples. For a data url
     * it is the encoded png/webp file, which you will need an image decoder for.
     * @return the number of bytes written, or 0 if the value is malformed or does not fit
     */
    size_t decode(uint8_t *out, size_t capacity) const;

private:
    std::string_view _value;
};

/**
 * @brief The state of a control, from KEY-STATE (surfaces) or SUB-STATE (subscriptions)
 *
 * Which of the optional fields are present depends on what the surface or subscription
 * asked for (colors, text, bitmaps, LEDs...).
 */
struct SatelliteControlState
{
    uint16_t index = 0;                                             /**< Key index. For a layout surface, the position of the control in the layout's controls list */
    String controlId;                                               /**< CONTROLID for layout surfaces, else empty */
    SatelliteSurfaceKeyType type = SatelliteSurfaceKeyType::BUTTON; /**< Type of key */
    std::optional<String> location;                                 /**< Absolute location, "page/row/column", when known */
    std::optional<SatelliteEncodedData> bitmap;                     /**< Bitmap - only valid during the callback, see SatelliteEncodedData */
    std::optional<SatelliteColor> keyColor;                         /**< Background color */
    std::optional<SatelliteColor> textColor;                        /**< Text color */
    std::optional<String> text;                                     /**< Text label (already base64 decoded) */
    std::optional<uint8_t> fontSize;                                /**< Font size, 0 when Companion says "auto" */
    std::optional<bool> pressed;                                    /**< Pressed state */
    std::optional<SatelliteEncodedData> leds;                       /**< LED colors, one R,G,B triple per segment - only valid during the callback */
};

/** @brief KEY-STATE uses the shared control state */
using SatelliteSurfaceKeyState = SatelliteControlState;

/**
 * @brief Fill in the style fields of a control state (everything but index/controlId) from KEY-STATE/SUB-STATE params
 *
 * Internal - the state points into the params for bitmap and LED data.
 */
void satelliteParseControlState(const satellite_protocol::Params &params, SatelliteControlState &state);

/**
 * @brief How a layout control's LEDs (eg. a ring around an encoder) are arranged (API >= 1.13.0)
 */
enum class SatelliteLedsMode
{
    FULL_RING, /**< The LEDs form a full circle, segment 0 at 6 o'clock, increasing clockwise */
    SIMPLE,    /**< Any other shape - a value is swept across all segments, segment 0 being the 0% end */
};

/**
 * @brief What Companion should stream for a control (a "style preset" in the layout manifest)
 */
struct SatelliteStylePreset
{
    String name;                                            /**< Preset name. A layout must define one named "default" */
    uint16_t bitmapWidth = 0;                               /**< Bitmap size in pixels, 0 for no bitmap */
    uint16_t bitmapHeight = 0;                              /**< Bitmap height, non-square sizes need Companion to advertise NONSQUARE */
    bool colors = false;                                    /**< Stream COLOR/TEXTCOLOR */
    bool text = false;                                      /**< Stream TEXT */
    bool textStyle = false;                                 /**< Stream FONT_SIZE */
    uint8_t ledSegments = 0;                                /**< Number of addressable LEDs (1-200), 0 for none (API >= 1.13.0) */
    SatelliteLedsMode ledsMode = SatelliteLedsMode::SIMPLE; /**< How the LEDs are arranged */
};

/**
 * @brief A control in a layout manifest
 */
struct SatelliteLayoutControl
{
    String id;           /**< Unique id (letters, digits, `-` and `/`), eg. "0/3" */
    uint16_t row = 0;    /**< Zero-based row, used to place it in Companion's grid */
    uint16_t column = 0; /**< Zero-based column */
    String stylePreset;  /**< Name of the preset to use, or empty for "default" */
};

/**
 * @brief A surface layout manifest (LAYOUT_MANIFEST, API >= 1.9.0)
 *
 * Describes each control individually, rather than as a simple grid of identical keys.
 * Each control can ask for different data - eg. a big bitmap for an LCD key, just a
 * color for an LED button, and an LED ring for an encoder - so the device only receives
 * (and has to buffer) what it can actually show.
 */
struct SatelliteLayout
{
    std::vector<SatelliteStylePreset> stylePresets; /**< Must include one named "default" */
    std::vector<SatelliteLayoutControl> controls;
};

#endif // COMPANION_SATELLITE_TYPES_HPP
