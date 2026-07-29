/**
 * @file companion-surface.hpp
 * @brief Satellite surface implementation for Companion Satellite API
 * @author Elliot Matson
 * @version 1.0
 * @date November 2025
 *
 * This file contains the satellite surface implementation that handles
 * communication between ESP32 devices and Bitfocus Companion via the
 * Satellite API. It provides virtual control surfaces with buttons,
 * variables, and callback support for interactive functionality.
 *
 * Features:
 * - Virtual control surfaces with configurable layouts
 * - Button state management and callbacks
 * - Variable input/output with base64 encoding
 * - Brightness control support
 * - Pincode input functionality
 * - Lock state management
 * - Color and bitmap support for enhanced UI
 * - Page navigation and firmware update notification
 * - Custom config fields with runtime updates via DEVICE-CONFIG
 */

#ifndef COMPANION_SURFACE_HPP
#define COMPANION_SURFACE_HPP

// ---------------------------------------------------------------------------
// Includes
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <functional>
#include <optional>
#include <vector>
#include "esp_event.h"
#include "companion-satellite.hpp"

class Satellite; // forward declaration (see matching note in companion-satellite.hpp)

/** @brief ESP event base for satellite surface events */
ESP_EVENT_DECLARE_BASE(SATELLITE_SURFACE_EVENTS);

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/**
 * @brief Satellite event types
 *
 * These events are posted when various satellite operations occur. The arg
 * is a pointer to the satellite instance that triggered the event.
 */
enum
{
    SATELLITE_EVENT_READY,         /**< Satellite is initialized and ready */
    SATELLITE_EVENT_CONNECTED,     /**< Connected to Companion */
    SATELLITE_EVENT_PING_SENT,     /**< Keepalive ping sent to Companion */
    SATELLITE_EVENT_PONG_RECEIVED, /**< Keepalive pong received from Companion */
    SATELLITE_EVENT_DISCONNECTED   /**< Disconnected from Companion */
};

/**
 * @brief Pincode support levels for satellite surfaces
 *
 * Defines the level of pincode functionality supported by a surface.
 */
enum class SatelliteSurfacePincodeSupport
{
    NONE,    /**< No pincode support */
    PARTIAL, /**< Partial pincode support */
    FULL     /**< Full pincode support */
};

/**
 * @brief Variable types for satellite surfaces
 *
 * Defines whether a variable is for input or output purposes.
 */
enum class SatelliteSurfaceVariableType
{
    VARIABLE_INPUT, /**< Input variable (can be set from Companion) */
    VARIABLE_OUTPUT /**< Output variable (can be read by Companion) */
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

/**
 * @brief Configuration structure for satellite surfaces
 *
 * Contains all configuration options for a satellite surface including
 * layout, capabilities, and feature support.
 */
struct SatelliteSurfaceConfig
{
    uint8_t totalKeys = 32;                                                               /**< Total number of keys on the surface */
    uint8_t keysPerRow = 8;                                                               /**< Number of keys per row */
    bool sendBitmaps = false;                                                             /**< Whether to send bitmap images */
    bool sendColors = true;                                                               /**< Whether to send color information */
    bool sendText = true;                                                                 /**< Whether to send text labels */
    bool sendTextStyle = false;                                                           /**< Whether to send text styling */
    bool supportsBrightness = false;                                                      /**< Whether brightness control is supported */
    SatelliteSurfacePincodeSupport pincodeSupport = SatelliteSurfacePincodeSupport::NONE; /**< Level of pincode support */
    std::optional<String> configFieldsJson = std::nullopt;                                /**< Optional JSON array of CONFIG_FIELDS definitions */

    /**
     * Enables device-initiated page up/down navigation (CHANGE-PAGE messages) for this
     * surface. Set to the label Companion should show on the "allow this device to
     * change pages" checkbox in the surface settings panel. CAN_CHANGE_PAGE is a label
     * string in the protocol, not a boolean - its mere presence enables the feature, so
     * leave this unset (nullopt) to omit CAN_CHANGE_PAGE entirely and disable page
     * changes from this device.
     */
    std::optional<String> canChangePageLabel = std::nullopt;

    /**
     * Bitmap encoding to request for this surface's KEY-STATE BITMAP field (rgb/png/webp).
     * Only used when sendBitmaps is true. Falls back to rgb if unset, or if the requested
     * format was not advertised by the server's CAPS BITMAP_FORMATS list.
     */
    std::optional<String> bitmapFormat = std::nullopt;
};

/**
 * @brief Structure representing a satellite surface variable
 *
 * Contains metadata about a variable associated with a surface.
 */
struct SatelliteSurfaceVariable
{
    String id;                         /**< Unique identifier for the variable */
    SatelliteSurfaceVariableType type; /**< Whether this is an input or output variable */
    String name;                       /**< Display name of the variable */
    String description;                /**< Description of the variable's purpose */
};

/**
 * @brief Key state structure for satellite surfaces
 *
 * Contains all the visual and state information for a single key/button
 * on a satellite surface. Used for updating key appearance and behavior.
 *
 * color and SatelliteSurfaceKeyType are shared with Satellite::SubscriptionState
 * (see companion-satellite.hpp), since KEY-STATE and SUB-STATE report the same
 * kinds of data.
 */
struct SatelliteSurfaceKeyState
{
    uint8_t index;                          /**< Key index */
    SatelliteSurfaceKeyType type = SatelliteSurfaceKeyType::BUTTON; /**< Type of key */
    std::optional<String> location;         /**< Optional absolute location, "page/row/column" */
    std::optional<String> bitmap;           /**< Optional base64-encoded bitmap */
    std::optional<color> keyColor;          /**< Optional key color */
    std::optional<color> textColor;         /**< Optional text color */
    std::optional<String> text;             /**< Optional text label */
    std::optional<uint8_t> fontSize;        /**< Optional font size */
    std::optional<bool> pressed;            /**< Optional pressed state */
};

// ---------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------

/**
 * @brief Represents a virtual control surface with buttons and variables
 *
 * A SatelliteSurface provides a virtual control surface that can be used in Companion.
 * It contains buttons that can be pressed, released, or rotated, and variables that
 * can be set to display information.
 */
class SatelliteSurface
{
public:
    /**
     * @brief Construct a new Satellite Surface object
     * @param satellite Reference to the parent Satellite object
     * @param surfaceName Name of the surface (displayed in Companion)
     * @param config Configuration options for the surface
     * @param variableList List of variables associated with this surface
     */
    SatelliteSurface(Satellite &satellite, const String &surfaceName, const SatelliteSurfaceConfig &config = SatelliteSurfaceConfig{}, const std::vector<SatelliteSurfaceVariable> &variableList = {});

    /**
     * @brief Destroy the Satellite Surface object
     *
     * Automatically removes the surface from the parent satellite.
     */
    ~SatelliteSurface();

    /**
     * @brief Handle incoming messages from Companion
     * @param message The message string received from Companion
     *
     * Processes commands from Companion including KEY-STATE, KEYS-CLEAR,
     * BRIGHTNESS, VARIABLE-VALUE, LOCKED-STATE, and DEVICE-CONFIG. Triggers
     * appropriate callbacks based on the command type.
     */
    void handleMessage(const String &message);

    /**
     * @brief Get the unique ID of this surface
     * @return String The surface ID (generated from surface name and device name)
     */
    const String &getID() const { return _surfaceID; }

    /**
     * @brief Get the display name of this surface
     * @return String The surface name
     */
    const String &getName() const { return _surfaceName; }

    /**
     * @brief Get the configuration of this surface
     * @return SatelliteSurfaceConfig The surface configuration
     */
    const SatelliteSurfaceConfig &getConfig() const { return _config; }

    /**
     * @brief Get the list of variables associated with this surface
     * @return std::vector<SatelliteSurfaceVariable> The list of surface variables
     */
    const std::vector<SatelliteSurfaceVariable> &getVariables() const { return _variables; }

    /**
     * @brief Trigger a button action on this surface
     * @param buttonIndex The index of the button (0-based)
     * @param action The type of action (PRESSED, RELEASED, RIGHT, LEFT)
     */
    void buttonAction(uint8_t buttonIndex, SatelliteAction action);

    /**
     * @brief Set a variable value on this surface
     * @param id The variable identifier
     * @param value The value to set (will be base64 encoded automatically)
     */
    void setVariable(const String &id, const String &value);

    /**
     * @brief Press a pincode key on this surface
     * @param keyIndex The index of the pincode key to press (0-based)
     */
    void pressPincodeKey(uint8_t keyIndex);

    /**
     * @brief Report firmware update availability to Companion (API >= 1.10.0)
     * @param updateUrl URL to firmware binary; empty string clears availability
     */
    void reportFirmwareUpdateInfo(const String &updateUrl);

    /**
     * @brief Request Companion to change page for this surface (API >= 1.10.0)
     * @param direction SatellitePageDirection::NEXT or SatellitePageDirection::PREVIOUS
     */
    void changePage(SatellitePageDirection direction);

    /**
     * @brief Set callback for key state changes
     * @param callback Function to call when a key's visual state changes
     *
     * The callback receives a SatelliteSurfaceKeyState object containing
     * the key's index, type, and visual properties (color, text, bitmap, etc.).
     */
    void setOnKeyStateChange(std::function<void(SatelliteSurfaceKeyState)> callback) { onKeyStateChange = callback; }

    /**
     * @brief Set callback for keys clear event
     * @param callback Function to call when all keys are cleared
     *
     * This callback is triggered when Companion sends a KEYS-CLEAR command.
     */
    void setOnKeysClear(std::function<void()> callback) { onKeysClear = callback; }

    /**
     * @brief Set callback for brightness changes
     * @param callback Function to call when brightness is changed
     *
     * The callback receives the new brightness value (0-100).
     */
    void setOnBrightnessSet(std::function<void(uint8_t)> callback) { onBrightnessSet = callback; }

    /**
     * @brief Set callback for variable value changes
     * @param callback Function to call when a variable value is set
     *
     * The callback receives the variable ID and the decoded value string.
     * Values are automatically base64 decoded before calling the callback.
     */
    void setOnVariableSet(std::function<void(String, String)> callback) { onVariableSet = callback; }

    /**
     * @brief Set callback for lock state changes
     * @param callback Function to call when the lock state changes
     *
     * The callback receives the lock state (true/false) and the number
     * of characters entered in the pincode field.
     */
    void setOnLockedStateSet(std::function<void(bool, uint8_t)> callback) { onLockedStateSet = callback; }

    /**
     * @brief Set callback for DEVICE-CONFIG updates
     * @param callback Function to call with decoded JSON CONFIG payload
     */
    void setOnDeviceConfig(std::function<void(const String &)> callback) { onDeviceConfig = callback; }

private:
    Satellite *_satellite; /**< Pointer to parent satellite */

    // Callback functions for events
    std::function<void(SatelliteSurfaceKeyState keyState)> onKeyStateChange;        /**< Callback triggered when a key's visual state changes */
    std::function<void()> onKeysClear;                                              /**< Callback triggered when all keys are cleared */
    std::function<void(uint8_t brightness)> onBrightnessSet;                        /**< Callback triggered when brightness is changed */
    std::function<void(String variableID, String variableValue)> onVariableSet;     /**< Callback triggered when a variable value is set */
    std::function<void(bool isLocked, uint8_t charactersEntered)> onLockedStateSet; /**< Callback triggered when lock state changes */
    std::function<void(const String &configJson)> onDeviceConfig;                   /**< Callback triggered when DEVICE-CONFIG is received */

    String _surfaceID;                                /**< Unique surface identifier */
    String _surfaceName;                              /**< Display name of the surface */
    SatelliteSurfaceConfig _config;                   /**< Configuration options for the surface */
    std::vector<SatelliteSurfaceVariable> _variables; /**< List of variables associated with this surface */
};

#endif // COMPANION_SURFACE_HPP