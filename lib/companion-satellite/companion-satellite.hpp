/**
 * @file companion-satellite.hpp
 * @brief Companion Satellite API library for ESP32 microcontrollers
 * @author Elliot Matson
 * @version 1.0
 * @date November 2025
 *
 * This library provides functionality to connect ESP32-based devices to Bitfocus Companion
 * via the Companion Satellite API. It enables the creation of virtual surfaces with buttons
 * and variables that can interact with Companion software.
 *
 * Features:
 * - TCP connection to Companion with automatic reconnection
 * - REST API for configuration via web interface
 * - mDNS service discovery
 * - Multiple virtual surfaces, plus standalone button subscriptions
 * - Button press/release, rotary encoder, and page-navigation support
 * - Variable input/output support with base64 encoding
 * - Pincode input support
 * - Bitmap format negotiation (rgb/png/webp)
 * - Thread-safe operations with mutex protection
 * - Persistent configuration storage
 */

#ifndef COMPANION_SATELLITE_HPP
#define COMPANION_SATELLITE_HPP

// ---------------------------------------------------------------------------
// Includes
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <functional>
#include <list>
#include <optional>
#include <vector>
#include <Preferences.h>
#include <Ticker.h>
#include "esp_event.h"

// _server/_client are only ever used as pointers in this header; the full
// ESPAsyncWebServer.h/AsyncTCP.h definitions are only needed by
// companion-satellite.cpp, which includes them directly.
class AsyncWebServer;
class AsyncClient;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** @brief Default instance name for the satellite device */
constexpr const char *DEFAULT_INSTANCE_NAME = "Companion Microsatellite";

/** @brief Default hostname for the satellite device */
constexpr const char *DEFAULT_HOSTNAME = "microsatellite";

/** @brief Default API port for the satellite web server */
constexpr uint16_t DEFAULT_PORT = 9999;

/** @brief ESP event base for satellite events */
ESP_EVENT_DECLARE_BASE(SATELLITE_EVENTS);

class SatelliteSurface; // forward declaration (see note below, above the companion-surface.hpp include)

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/**
 * @brief Type of a key/button/control location
 *
 * Shared by KEY-STATE (surfaces) and SUB-STATE (subscriptions) - both
 * messages report the same set of location types.
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
 * PRESSED/RELEASED are used for press-style actions (SatelliteSurface::buttonAction,
 * Satellite::subscriptionAction). RIGHT/LEFT are used for rotation direction
 * (SatelliteSurface::buttonAction rotate, Satellite::subscriptionAction rotate).
 */
enum class SatelliteAction
{
    PRESSED,
    RELEASED,
    RIGHT,
    LEFT,
};

/**
 * @brief Direction for surface page navigation (CHANGE-PAGE, API >= 1.10.0)
 */
enum class SatellitePageDirection
{
    NEXT,
    PREVIOUS,
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

/**
 * @brief RGB color structure
 *
 * Represents a color using red, green, and blue components.
 * Each component ranges from 0 (no intensity) to 255 (full intensity).
 */
struct color
{
    uint8_t r; /**< Red component (0-255) */
    uint8_t g; /**< Green component (0-255) */
    uint8_t b; /**< Blue component (0-255) */
};

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/**
 * @brief Parse a KEY-STATE/SUB-STATE TYPE value
 * @return The matching SatelliteSurfaceKeyType, defaulting to BUTTON for unknown or missing values
 */
SatelliteSurfaceKeyType parseSatelliteKeyType(const String &typeStr);

/**
 * @brief Parse a hex (#RRGGBB) or css rgb(r,g,b) color string
 * @return std::nullopt if the value doesn't match either supported format
 */
std::optional<color> parseSatelliteColor(const String &value);

// companion-surface.hpp depends on the enums/structs declared above (color,
// SatelliteSurfaceKeyType, SatelliteAction, SatellitePageDirection), so it must be
// included after them rather than grouped with the includes at the top of this file.
// Pulling it in here also means consumers get the whole public API (Satellite +
// SatelliteSurface) from a single `#include "companion-satellite.hpp"`.
#include "companion-surface.hpp"

// ---------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------

/**
 * @brief Main satellite class for connecting to Bitfocus Companion
 *
 * The Satellite class manages the connection to Companion software via TCP socket
 * and provides a REST API for configuration. It can host multiple SatelliteSurface
 * objects that represent virtual control surfaces.
 */
class Satellite
{
public:
    /**
     * @brief State of a single subscribed control (SUB-STATE), see addSubscription
     */
    struct SubscriptionState
    {
        String subId;                                                   /**< Subscription id, as passed to addSubscription */
        SatelliteSurfaceKeyType type = SatelliteSurfaceKeyType::BUTTON; /**< Type of the location */
        std::optional<String> location;                                 /**< Absolute location, "page/row/column" */
        std::optional<String> bitmap;                                   /**< Optional base64-encoded bitmap */
        std::optional<color> keyColor;                                  /**< Optional key color */
        std::optional<color> textColor;                                 /**< Optional text color */
        std::optional<String> text;                                     /**< Optional text label */
        std::optional<uint8_t> fontSize;                                /**< Optional font size */
        std::optional<bool> pressed;                                    /**< Optional pressed state */
    };

    /**
     * @brief Construct a new Satellite object
     * @param deviceName Name of the device (displayed in Companion)
     * @param apiPort Port number for the configuration web server
     */
    Satellite(const char *deviceName = DEFAULT_INSTANCE_NAME, uint16_t apiPort = DEFAULT_PORT);

    /**
     * @brief Destroy the Satellite object
     *
     * Disconnects from Companion and cleans up resources.
     */
    ~Satellite();

    /**
     * @brief Initialize the satellite
     *
     * Sets up the web server, mDNS, preferences, and attempts to connect to Companion.
     * Must be called after WiFi is connected.
     */
    void begin();

    /**
     * @brief Get the device name
     * @return String The device name with chip ID
     */
    String getDeviceName() const { return _deviceName; }

    /**
     * @brief Get the hostname for this device
     * @return String The hostname used for mDNS
     */
    String getHostname() const { return _hostname; }

    /**
     * @brief Get the target Companion hostname
     * @return String The hostname of the Companion server to connect to
     */
    String getTargetHostname() const { return lockedRead(_targetHostname); }

    /**
     * @brief Get the target Companion port
     * @return uint16_t The port of the Companion server to connect to
     */
    uint16_t getTargetPort() const { return lockedRead(_targetPort); }

    /**
     * @brief Set the target Companion hostname
     * @param hostname The hostname or IP address of the Companion server
     *
     * This will save the hostname to preferences and attempt to reconnect.
     */
    void setTargetHostname(const String &hostname)
    {
        ESP_LOGI("Satellite", "Setting target hostname: %s", hostname.c_str());
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        _targetHostname = hostname;
        _apiVersionRejected = false;
        xSemaphoreGiveRecursive(_mutex);
        _prefs.putString("host", hostname);
        connectToCompanion();
    }

    /**
     * @brief Set the target Companion port
     * @param port The port number of the Companion server
     *
     * This will save the port to preferences and attempt to reconnect.
     */
    void setTargetPort(uint16_t port)
    {
        ESP_LOGI("Satellite", "Setting target port: %d", port);
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        _targetPort = port;
        _apiVersionRejected = false;
        xSemaphoreGiveRecursive(_mutex);
        _prefs.putUInt("port", port);
        connectToCompanion();
    }

    /**
     * @brief Get the Companion software version
     * @return String The version of the connected Companion software
     *
     * Returns "unknown" if not connected or version information hasn't been received yet.
     */
    String getCompanionVersion() const { return lockedRead(_companionVersion); }

    /**
     * @brief Get the Satellite API version
     * @return String The version of the Satellite API being used
     *
     * Returns "unknown" if not connected or version information hasn't been received yet.
     */
    String getSatelliteApiVersion() const { return lockedRead(_satelliteApiVersion); }

    /**
     * @brief Whether the TCP socket is currently connected
     *
     * This indicates transport connectivity only. Protocol negotiation may
     * still be in progress.
     */
    bool isConnected() const { return lockedRead(_isConnected); }

    /**
     * @brief Whether the satellite is fully ready for protocol operations
     *
     * Ready means connected and successful BEGIN/ApiVersion negotiation has
     * completed.
     */
    bool isReady() const
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        bool ready = _isConnected && _apiNegotiated && _satelliteApiVersionValid && !_apiVersionRejected;
        xSemaphoreGiveRecursive(_mutex);
        return ready;
    }

    /**
     * @brief Whether the server advertised button subscriptions support via CAPS
     */
    bool supportsSubscriptions() const { return lockedRead(_capsSubscriptions); }

    /**
     * @brief Whether the server advertised non-square button support via CAPS
     */
    bool supportsNonsquareButtons() const { return lockedRead(_capsNonsquare); }

    /**
     * @brief Bitmap formats advertised by the server via CAPS (eg rgb/png/webp)
     *
     * Returns a copy, since the underlying list can be mutated by the network
     * task (eg. on reconnect) concurrently with the caller reading it.
     */
    std::vector<String> getCapsBitmapFormats() const { return lockedRead(_capsBitmapFormats); }

    /**
     * @brief Add a button subscription (API >= 1.10.0)
     * @param subId Client-defined subscription id
     * @param location Control location in page/row/column form, e.g. "1/2/3"
     * @param bitmapSize Optional bitmap size hint
     * @param bitmapFormat Optional negotiated bitmap format (rgb/png/webp)
     * @param colors Optional simple style flag to request COLOR/TEXTCOLOR fields
     * @param text Optional simple style flag to request TEXT field
     * @param textStyle Optional simple style flag to request text style fields (eg FONT_SIZE)
     */
    void addSubscription(const String &subId, const String &location, std::optional<uint16_t> bitmapSize = std::nullopt,
                         std::optional<String> bitmapFormat = std::nullopt,
                         std::optional<bool> colors = std::nullopt,
                         std::optional<bool> text = std::nullopt,
                         std::optional<bool> textStyle = std::nullopt);

    /**
     * @brief Remove a button subscription (API >= 1.10.0)
     */
    void removeSubscription(const String &subId);

    /**
     * @brief Press/release or rotate a subscribed control (API >= 1.10.0)
     * @param action PRESSED/RELEASED sends SUB-PRESS; RIGHT/LEFT sends SUB-ROTATE
     *               (RIGHT = forward, LEFT = backward)
     */
    void subscriptionAction(const String &subId, SatelliteAction action);

    /**
     * @brief Set callback for subscription state updates (SUB-STATE)
     */
    void setOnSubscriptionState(std::function<void(const SubscriptionState &)> callback) { onSubscriptionState = callback; }

private:
    /**
     * Hard cap for buffered TCP input to avoid unbounded memory growth.
     *
     * A single KEY-STATE/SUB-STATE line can contain a base64-encoded bitmap.
     * The default 72px RGB bitmap is 72*72*3 = 15552 raw bytes, which is
     * ~20736 bytes base64-encoded. This cap must stay comfortably above that
     * (with room for the surrounding fields) or bitmap-enabled surfaces will
     * have their KEY-STATE lines dropped. If you configure larger bitmaps
     * (eg. via a larger BITMAPS/BITMAP size), increase this accordingly:
     * ceil(width * height * 3 * 4 / 3) + ~256 bytes of overhead.
     */
    static constexpr size_t MAX_RX_BUFFER_SIZE = 32768;

    String _deviceName = DEFAULT_INSTANCE_NAME; /**< Device name with chip ID */
    String _hostname = DEFAULT_HOSTNAME;        /**< mDNS hostname */
    uint16_t _apiPort = DEFAULT_PORT;           /**< Web server port */
    String _targetHostname = "";                /**< Companion server hostname */
    uint16_t _targetPort = 0;                   /**< Companion server port */
    String _companionVersion = "unknown";       /**< Companion version */
    String _satelliteApiVersion = "unknown";    /**< Satellite API version */
    uint16_t _satelliteApiMajor = 0;            /**< Parsed satellite API major version */
    uint16_t _satelliteApiMinor = 0;            /**< Parsed satellite API minor version */
    uint16_t _satelliteApiPatch = 0;            /**< Parsed satellite API patch version */
    bool _satelliteApiVersionValid = false;     /**< True when ApiVersion was parsed successfully */
    bool _apiNegotiated = false;                /**< True after BEGIN was received and validated */
    bool _apiVersionRejected = false;           /**< True when server ApiVersion is below minimum */
    bool _isConnected = false;                  /**< Connection status */
    bool _capsSubscriptions = false;            /**< CAPS SUBSCRIPTIONS flag */
    bool _capsNonsquare = false;                /**< CAPS NONSQUARE flag */
    std::vector<String> _capsBitmapFormats;     /**< CAPS BITMAP_FORMATS list */

    AsyncWebServer *_server = nullptr; /**< Web server for REST API */
    AsyncClient *_client = nullptr;    /**< TCP client for Companion connection */

    /**
     * Recursive mutex guarding all satellite state that is written from the
     * AsyncTCP callback context (connect/disconnect/receive) and read from
     * application code (or vice versa): _client, connection/negotiation
     * flags, CAPS state, negotiated version info, target host/port, and the
     * registered surfaces list. Recursive so that call chains which already
     * hold the lock (eg. receiveCallback locking around sendAddCommand,
     * which locks again inside send()) don't self-deadlock.
     */
    SemaphoreHandle_t _mutex = xSemaphoreCreateRecursiveMutex();
    Preferences _prefs;       /**< Persistent storage for configuration */
    Ticker _keepaliveTicker;  /**< Timer for keepalive pings */
    Ticker _reconnectTicker;  /**< Timer for reconnection attempts */
    String _rxBuffer;         /**< Buffered TCP input for line-oriented parsing (receiveCallback only, single-threaded) */

    /**
     * @brief Read a field's current value while holding _mutex
     */
    template <typename T>
    T lockedRead(const T &field) const
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        T copy = field;
        xSemaphoreGiveRecursive(_mutex);
        return copy;
    }

    struct ApiParam
    {
        std::optional<String> id = std::nullopt;
        std::optional<String> val = std::nullopt;
    }; /**< A single parsed API parameter (id/value pair) */

    std::list<SatelliteSurface *> _surfaces;                            /**< List of registered surfaces */
    std::function<void(const SubscriptionState &)> onSubscriptionState; /**< Callback for SUB-STATE updates */

    /**
     * @brief Disconnect from Companion server
     *
     * Closes the TCP connection and stops keepalive timer.
     */
    void disconnectFromCompanion();

    /**
     * @brief Connect to Companion server
     *
     * Establishes TCP connection using stored hostname and port.
     */
    void connectToCompanion();

    /**
     * @brief Arm the reconnect timer, unless reconnection is permanently disabled
     *
     * Used both when the transport disconnects after a successful connection,
     * and when the initial (synchronous) AsyncClient::connect() call itself
     * fails, which does not otherwise trigger onDisconnect.
     */
    void scheduleReconnect();

    /**
     * @brief Send a message to Companion
     * @param message The message string to send
     *
     * Thread-safe method to send data over the TCP connection.
     */
    void send(const String &message);

    /**
     * @brief Callback for receiving data from Companion
     * @param arg User argument (unused)
     * @param client The AsyncClient instance
     * @param data Pointer to received data
     * @param len Length of received data
     */
    void receiveCallback(void *arg, AsyncClient *client, void *data, size_t len);

    /**
     * @brief Send an ADD-DEVICE command for a surface
     * @param surface Reference to the surface to add
     */
    void sendAddCommand(SatelliteSurface &surface);

    /**
     * @brief Add a surface to this satellite
     * @param surface Reference to the surface to add
     * @return bool True if successfully added, false if ID already exists
     */
    bool add(SatelliteSurface &surface);

    /**
     * @brief Remove a surface from this satellite
     * @param surface Reference to the surface to remove
     */
    void remove(SatelliteSurface &surface);

    /**
     * @brief Parse all KEY=VALUE tokens out of a Satellite API message line in a single pass
     *
     * Handles quoted and unquoted values. Any leading text that isn't part of
     * a KEY=VALUE token (eg. the leading command name) is skipped. Values are
     * left encoded as received; use findApiParam's decodeBase64 to decode a
     * specific field on demand.
     * @param data Full message line (or the parameter portion of one)
     * @return std::vector<ApiParam> All tokens found, in order of appearance
     */
    std::vector<ApiParam> tokenizeApiParams(const String &data) const;

    /**
     * @brief Look up a single field by name within a previously tokenized message
     * @param tokens Tokens produced by tokenizeApiParams
     * @param id Token name to search for
     * @param decodeBase64 Whether to base64-decode the extracted value
     * @return ApiParam Parsed token id/value pair when present
     */
    ApiParam findApiParam(const std::vector<ApiParam> &tokens, const char *id, bool decodeBase64 = false) const;

    /**
     * @brief Convenience overload for String token ids
     */
    ApiParam findApiParam(const std::vector<ApiParam> &tokens, const String &id, bool decodeBase64 = false) const { return findApiParam(tokens, id.c_str(), decodeBase64); }

    /**
     * @brief Append a BITMAP_FORMAT="..." param to a command if valid and advertised by CAPS
     *
     * Shared by ADD-DEVICE and ADD-SUB. Silently omits the parameter (Companion
     * defaults to rgb) if the format is unsafe or wasn't advertised in CAPS.
     * @param command Command string to append to
     * @param commandName Command name used in log messages (eg. "ADD-DEVICE")
     * @param requestedFormat Requested bitmap format (rgb/png/webp)
     */
    void appendBitmapFormatParam(String &command, const char *commandName, const String &requestedFormat) const;

    /**
     * @brief Check negotiated ApiVersion against a minimum version
     * @param major Minimum required major version
     * @param minor Minimum required minor version
     * @param patch Minimum required patch version
     * @return true when parsed ApiVersion is valid and >= requested minimum
     */
    bool isApiVersionAtLeast(uint16_t major, uint16_t minor, uint16_t patch = 0) const;

    friend class SatelliteSurface; /**< Allow SatelliteSurface to access private members */
};

#endif // COMPANION_SATELLITE_HPP
