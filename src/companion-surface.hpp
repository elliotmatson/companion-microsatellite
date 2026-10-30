/**
 * @file companion-surface.hpp
 * @brief A virtual control surface registered with Companion (ADD-DEVICE)
 */

#ifndef COMPANION_SURFACE_HPP
#define COMPANION_SURFACE_HPP

#include <Arduino.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "companion-satellite-types.hpp"

class Satellite;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/**
 * @brief Pincode support levels for satellite surfaces
 */
enum class SatelliteSurfacePincodeSupport
{
    NONE,    /**< Companion draws the pincode screen on the surface's keys itself */
    PARTIAL, /**< The surface shows the locked state itself, but the pincode is entered elsewhere */
    FULL     /**< The surface shows the locked state and lets the user enter the pincode (pressPincodeKey) */
};

/**
 * @brief Direction of a surface variable
 */
enum class SatelliteSurfaceVariableType
{
    VARIABLE_INPUT, /**< A value this device produces and sends to Companion with setVariable() */
    VARIABLE_OUTPUT /**< A value Companion produces and sends to this device (setOnVariableSet) */
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

/**
 * @brief Configuration of a surface
 */
struct SatelliteSurfaceConfig
{
    uint16_t totalKeys = 32;                                                              /**< Total number of keys on the surface (simple grid) */
    uint16_t keysPerRow = 8;                                                              /**< Number of keys per row (simple grid) */
    bool sendColors = true;                                                               /**< Whether to send color information (simple grid) */
    bool sendText = true;                                                                 /**< Whether to send text labels (simple grid) */
    bool sendTextStyle = false;                                                           /**< Whether to send text styling (simple grid) */
    bool supportsBrightness = false;                                                      /**< Whether brightness control is supported */
    SatelliteSurfacePincodeSupport pincodeSupport = SatelliteSurfacePincodeSupport::NONE; /**< Level of pincode support */
    std::optional<String> configFieldsJson = std::nullopt;                                /**< Optional JSON array of CONFIG_FIELDS definitions */

    /**
     * Enables device-initiated page up/down navigation (CHANGE-PAGE). Set to the label
     * Companion should show on the "allow this device to change pages" checkbox in the
     * surface settings. Leave unset to disable page changes from this device.
     */
    std::optional<String> canChangePageLabel = std::nullopt;

    /**
     * Requested square bitmap size in pixels for each key (simple grid). Leave unset to
     * disable bitmaps. Values below 5 are treated by Companion as its 72px default. Make
     * sure SatelliteOptions::maxLineLength is big enough (about size * size * 4 bytes).
     */
    std::optional<uint16_t> bitmapSize = std::nullopt;

    /**
     * Bitmap encoding to request (rgb/png/webp). Falls back to rgb if unset, or if
     * Companion did not advertise it. Applies to both simple grids and layouts.
     */
    std::optional<String> bitmapFormat = std::nullopt;

    /**
     * Describe each control individually instead of a simple grid (LAYOUT_MANIFEST).
     * When set, totalKeys, keysPerRow, sendColors, sendText, sendTextStyle and bitmapSize
     * are ignored - the layout's style presets say what each control receives.
     */
    std::optional<SatelliteLayout> layout = std::nullopt;
};

/**
 * @brief A variable attached to a surface
 */
struct SatelliteSurfaceVariable
{
    String id;                         /**< Unique identifier for the variable */
    SatelliteSurfaceVariableType type; /**< Input (device -> Companion) or output (Companion -> device) */
    String name;                       /**< Display name */
    String description;                /**< Description of the variable's purpose */
};

/**
 * @brief Pincode lock state, from LOCKED-STATE
 */
struct SatelliteLockedState
{
    bool locked = false;        /**< Whether the surface is locked */
    uint8_t characterCount = 0; /**< How many pincode digits have been entered */
    int16_t rotation = 0;       /**< The surface rotation configured in Companion: 0, 90, -90 or 180 (API >= 1.10.1) */
};

// ---------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------

/**
 * @brief A virtual control surface
 *
 * Shows up in Companion's Surfaces list once the satellite is connected, and is
 * re-registered automatically after every reconnect.
 */
class SatelliteSurface
{
public:
    /**
     * @brief Create a surface and register it with a satellite
     * @param satellite The satellite to register with (must outlive this surface)
     * @param surfaceName Name of the surface (displayed in Companion)
     * @param config Configuration options for the surface
     * @param variableList Variables associated with this surface
     */
    SatelliteSurface(Satellite &satellite, const String &surfaceName, const SatelliteSurfaceConfig &config = SatelliteSurfaceConfig{}, const std::vector<SatelliteSurfaceVariable> &variableList = {});

    /** @brief Removes the surface from the satellite (and from Companion) */
    ~SatelliteSurface();

    SatelliteSurface(const SatelliteSurface &) = delete;
    SatelliteSurface &operator=(const SatelliteSurface &) = delete;

    /** @brief The unique ID of this surface (derived from its name and the device name) */
    const String &getID() const { return _surfaceID; }

    /** @brief The display name of this surface */
    const String &getName() const { return _surfaceName; }

    /** @brief The configuration of this surface */
    const SatelliteSurfaceConfig &getConfig() const { return _config; }

    /** @brief The variables associated with this surface */
    const std::vector<SatelliteSurfaceVariable> &getVariables() const { return _variables; }

    /** @brief Whether Companion has accepted this surface on the current connection (ADD-DEVICE OK) */
    bool isRegistered() const;

    /**
     * @brief Press, release or rotate (a single step) a key
     * @param buttonIndex The key index (for a layout, the position in its controls list)
     */
    void buttonAction(uint16_t buttonIndex, SatelliteAction action);

    /**
     * @brief Rotate an encoder by a number of steps
     * @param buttonIndex The key index (for a layout, the position in its controls list)
     * @param steps Positive for clockwise, negative for counter-clockwise
     *
     * Sent as one message when Companion supports rotation amounts (API >= 1.14.0),
     * otherwise as one message per step.
     */
    void rotate(uint16_t buttonIndex, int steps);

    /** @brief Press, release or rotate a control of a layout surface by its id */
    void controlAction(const String &controlId, SatelliteAction action);

    /** @brief Rotate a control of a layout surface by its id, see rotate() */
    void rotateControl(const String &controlId, int steps);

    /**
     * @brief Set the value of an input variable
     * @param id The variable id
     * @param value The value (base64 encoded automatically)
     */
    void setVariable(const String &id, const String &value);

    /** @brief Press a pincode digit (0-9) */
    void pressPincodeKey(uint8_t keyIndex);

    /**
     * @brief Report firmware update availability to Companion
     * @param updateUrl URL users can get the update from; empty clears it
     */
    void reportFirmwareUpdateInfo(const String &updateUrl);

    /** @brief Ask Companion to change this surface's page (needs config.canChangePageLabel) */
    void changePage(SatellitePageDirection direction);

    /**
     * @brief Called when a key's state changes (KEY-STATE)
     *
     * Bitmap and LED data in the state are only valid until the callback returns.
     */
    void setOnKeyStateChange(std::function<void(const SatelliteControlState &)> callback) { onKeyStateChange = std::move(callback); }

    /** @brief Called when Companion clears all keys (KEYS-CLEAR) */
    void setOnKeysClear(std::function<void()> callback) { onKeysClear = std::move(callback); }

    /** @brief Called with the new brightness (0-100) */
    void setOnBrightnessSet(std::function<void(uint8_t)> callback) { onBrightnessSet = std::move(callback); }

    /** @brief Called with the id and (decoded) value when Companion sets an output variable */
    void setOnVariableSet(std::function<void(const String &, const String &)> callback) { onVariableSet = std::move(callback); }

    /** @brief Called when the pincode lock state changes (needs pincodeSupport other than NONE) */
    void setOnLockedStateSet(std::function<void(const SatelliteLockedState &)> callback) { onLockedStateSet = std::move(callback); }

    /** @brief Called with the decoded CONFIG JSON object whenever the config fields' values change */
    void setOnDeviceConfig(std::function<void(const String &)> callback) { onDeviceConfig = std::move(callback); }

    /** @brief Called each time Companion accepts the surface, ie. after every (re)connect */
    void setOnRegistered(std::function<void()> callback) { onRegistered = std::move(callback); }

    /** @brief Called when Companion rejects a command for this surface, with the command name and error message */
    void setOnError(std::function<void(const String &, const String &)> callback) { onError = std::move(callback); }

private:
    friend class Satellite;

    /** @brief Handle a message for this surface, on the dispatch task */
    void handleMessage(const satellite_protocol::Message &message);

    /** @brief Build the ADD-DEVICE line. Called by Satellite with its mutex held */
    std::string buildAddDeviceCommand() const;

    /** @brief Build the LAYOUT_MANIFEST JSON */
    String buildLayoutManifest() const;

    /** @brief Send a KEY-PRESS/KEY-ROTATE, addressing the key by index or control id */
    void sendKeyAction(const String *controlId, uint16_t index, SatelliteAction action);
    void sendRotate(const String *controlId, uint16_t index, int steps);
    bool resolveControl(uint16_t index, const String *&controlId) const;

    Satellite *_satellite;

    std::function<void(const SatelliteControlState &)> onKeyStateChange;
    std::function<void()> onKeysClear;
    std::function<void(uint8_t)> onBrightnessSet;
    std::function<void(const String &, const String &)> onVariableSet;
    std::function<void(const SatelliteLockedState &)> onLockedStateSet;
    std::function<void(const String &)> onDeviceConfig;
    std::function<void()> onRegistered;
    std::function<void(const String &, const String &)> onError;

    String _surfaceID;
    String _surfaceName;
    SatelliteSurfaceConfig _config;
    std::vector<SatelliteSurfaceVariable> _variables;
    bool _registered = false;       /**< Guarded by the satellite's mutex */
    bool _addDevicePending = false; /**< ADD-DEVICE sent, no OK/ERROR yet (guarded by the satellite's mutex) */
    uint32_t _addDeviceSentMs = 0;  /**< When it was last sent */
    uint32_t _addDeviceRetryMs = 0; /**< How long to wait before sending it again */
};

#endif // COMPANION_SURFACE_HPP
