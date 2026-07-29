/**
 * @file companion-surface.cpp
 * @brief Implementation of satellite surface functionality for Companion API
 * @author Elliot Matson
 * @version 1.0
 * @date November 2025
 *
 * This file implements the satellite surface class that provides virtual
 * control surfaces for interaction with Bitfocus Companion. It handles
 * message processing, callback management, and communication protocols.
 */

#include "companion-surface.hpp"

#include <base64.h>
#include <MD5Builder.h>

static bool parseApiBool(const String &value)
{
    return value == "1" || value == "true";
}

/**
 * @brief Validate a token value for direct inclusion in a quoted API parameter
 */
static bool isSafeApiTokenValue(const String &value)
{
    return value.length() > 0 && value.indexOf('"') == -1 && value.indexOf('\n') == -1 && value.indexOf('\r') == -1;
}

ESP_EVENT_DEFINE_BASE(SATELLITE_SURFACE_EVENTS);

/**
 * @brief Constructor for SatelliteSurface
 *
 * Creates a new surface with a unique ID generated from the surface name and device name.
 * The surface is automatically registered with the parent satellite.
 */
SatelliteSurface::SatelliteSurface(Satellite &satellite, const String &surfaceName, const SatelliteSurfaceConfig &config, const std::vector<SatelliteSurfaceVariable> &variableList)
{
    ESP_LOGI("SatelliteSurface", "Creating surface: %s", surfaceName.c_str());
    MD5Builder md5;
    md5.begin();
    md5.add(surfaceName);
    md5.add(satellite.getDeviceName());
    md5.calculate();

    _surfaceID = "usat:" + md5.toString();
    _surfaceName = surfaceName;
    _config = config;
    _variables = variableList;
    ESP_LOGI("SatelliteSurface", "Surface ID: %s", _surfaceID.c_str());
    _satellite = &satellite;
    _satellite->add(*this);
    ESP_LOGI("SatelliteSurface", "Surface %s added to satellite", surfaceName.c_str());
}

/**
 * @brief Destructor for SatelliteSurface
 *
 * Automatically removes the surface from the parent satellite when destroyed.
 */
SatelliteSurface::~SatelliteSurface()
{
    ESP_LOGI("SatelliteSurface", "Destroying surface: %s", _surfaceName.c_str());
    if (_satellite)
    {
        _satellite->remove(*this);
        ESP_LOGI("SatelliteSurface", "Surface %s removed from satellite", _surfaceName.c_str());
    }
}

/**
 * @brief Handle incoming messages from Companion
 *
 * Parses and processes various command types received from Companion:
 * - KEY-STATE: Updates key visual state and triggers onKeyStateChange callback
 * - KEYS-CLEAR: Clears all keys and triggers onKeysClear callback
 * - BRIGHTNESS: Sets brightness level and triggers onBrightnessSet callback
 * - VARIABLE-VALUE: Updates variable value (base64 decoded) and triggers onVariableSet callback
 * - LOCKED-STATE: Updates lock state and triggers onLockedStateSet callback
 * - DEVICE-CONFIG: Decodes CONFIG_FIELDS values and triggers onDeviceConfig callback
 *
 * The function parses command parameters and extracts relevant values before
 * calling the appropriate callback functions if they are registered. See the
 * field comments on SatelliteSurfaceKeyState for what KEY-STATE can carry.
 */
void SatelliteSurface::handleMessage(const String &message)
{
    ESP_LOGD("SatelliteSurface", "Handling message: %s", message.c_str());

    // Split message into command and parameters by spaces
    int separator = message.indexOf(' ');
    String command = separator == -1 ? message : message.substring(0, separator);
    String params = separator == -1 ? "" : message.substring(separator + 1);
    if (params.startsWith("OK"))
    {
        ESP_LOGI("SatelliteSurface", "Received OK for command: %s", command.c_str());
        return;
    }
    if (params.startsWith("ERROR"))
    {
        ESP_LOGE("SatelliteSurface", "Error for command %s: %s", command.c_str(), params.c_str());
        return;
    }


    // Handle different commands
    if (command == "KEY-STATE")
    {
        // Handle key state update
        ESP_LOGI("SatelliteSurface", "KEY-STATE command received: %s", params.c_str());
        SatelliteSurfaceKeyState state;
        // Tokenize once - a bitmap-carrying KEY-STATE line can be tens of KB,
        // so re-scanning it from scratch for every field would be wasteful.
        std::vector<Satellite::ApiParam> tokens = _satellite->tokenizeApiParams(params);
        // extract key index
        Satellite::ApiParam keyParam = _satellite->findApiParam(tokens, "KEY");
        Satellite::ApiParam typeParam = _satellite->findApiParam(tokens, "TYPE");
        if (keyParam.val.has_value())
        {
            state.index = static_cast<uint8_t>(keyParam.val.value().toInt());
            state.type = parseSatelliteKeyType(typeParam.val.value_or("BUTTON"));
            // extract optional parameters
            Satellite::ApiParam locationParam = _satellite->findApiParam(tokens, "LOCATION");
            if (locationParam.val.has_value())
            {
                state.location = locationParam.val.value();
            }
            Satellite::ApiParam bitmapParam = _satellite->findApiParam(tokens, "BITMAP");
            if (bitmapParam.val.has_value())
            {
                state.bitmap = bitmapParam.val.value();
            }
            Satellite::ApiParam colorParam = _satellite->findApiParam(tokens, "COLOR");
            if (colorParam.val.has_value())
            {
                state.keyColor = parseSatelliteColor(colorParam.val.value());
            }
            Satellite::ApiParam textColorParam = _satellite->findApiParam(tokens, "TEXTCOLOR");
            if (textColorParam.val.has_value())
            {
                state.textColor = parseSatelliteColor(textColorParam.val.value());
            }
            Satellite::ApiParam textParam = _satellite->findApiParam(tokens, "TEXT", true);
            if (textParam.val.has_value())
            {
                state.text = textParam.val.value();
            }
            Satellite::ApiParam fontSizeParam = _satellite->findApiParam(tokens, "FONT_SIZE");
            if (fontSizeParam.val.has_value())
            {
                state.fontSize = static_cast<uint8_t>(fontSizeParam.val.value().toInt());
            }
            Satellite::ApiParam pressedParam = _satellite->findApiParam(tokens, "PRESSED");
            if (pressedParam.val.has_value())
            {
                state.pressed = parseApiBool(pressedParam.val.value());
            }
            // call the onKeyStateChange callback if set
            if (onKeyStateChange)
            {
                onKeyStateChange(state);
            }
        }
    }
    else if (command == "KEYS-CLEAR")
    {
        // Handle keys clear command
        ESP_LOGI("SatelliteSurface", "KEYS-CLEAR command received: %s", params.c_str());
        // call the onKeysClear callback if set
        if (onKeysClear)
        {
            onKeysClear();
        }
    }
    else if (command == "BRIGHTNESS")
    {
        // Handle brightness command
        ESP_LOGI("SatelliteSurface", "BRIGHTNESS command received: %s", params.c_str());
        // extract brightness value
        std::vector<Satellite::ApiParam> tokens = _satellite->tokenizeApiParams(params);
        Satellite::ApiParam brightnessParam = _satellite->findApiParam(tokens, "VALUE");
        // call the onBrightnessSet callback if set
        if (onBrightnessSet && brightnessParam.val.has_value())
        {
            long brightness = brightnessParam.val.value().toInt();
            if (brightness < 0)
            {
                brightness = 0;
            }
            else if (brightness > 100)
            {
                brightness = 100;
            }
            onBrightnessSet(static_cast<uint8_t>(brightness));
        }
    }
    else if (command == "VARIABLE-VALUE")
    {
        // Handle variable value update
        ESP_LOGI("SatelliteSurface", "VARIABLE-VALUE command received: %s", params.c_str());
        // extract variable ID and value
        std::vector<Satellite::ApiParam> tokens = _satellite->tokenizeApiParams(params);
        Satellite::ApiParam variableIDParam = _satellite->findApiParam(tokens, "VARIABLE");
        Satellite::ApiParam variableValueParam = _satellite->findApiParam(tokens, "VALUE", true);
        ESP_LOGI("SatelliteSurface", "Decoded variable %s value: %s", variableIDParam.val.value_or("<invalid>").c_str(), variableValueParam.val.value_or("<invalid>").c_str());
        // call the onVariableSet callback if set
        if (onVariableSet && variableIDParam.val.has_value() && variableValueParam.val.has_value())
        {
            onVariableSet(variableIDParam.val.value(), variableValueParam.val.value());
        }
    }
    else if (command == "LOCKED-STATE")
    {
        // Handle locked state update
        ESP_LOGI("SatelliteSurface", "LOCKED-STATE command received: %s", params.c_str());
        // extract locked state and characters entered
        std::vector<Satellite::ApiParam> tokens = _satellite->tokenizeApiParams(params);
        Satellite::ApiParam lockedParam = _satellite->findApiParam(tokens, "LOCKED");
        Satellite::ApiParam charCountParam = _satellite->findApiParam(tokens, "CHARACTER_COUNT");
        if (onLockedStateSet && lockedParam.val.has_value() && charCountParam.val.has_value())
        {
            bool isLocked = parseApiBool(lockedParam.val.value());
            uint8_t charactersEntered = static_cast<uint8_t>(charCountParam.val.value().toInt());
            onLockedStateSet(isLocked, charactersEntered);
        }
    }
    else if (command == "DEVICE-CONFIG")
    {
        // Handle runtime config values pushed by Companion.
        ESP_LOGI("SatelliteSurface", "DEVICE-CONFIG command received: %s", params.c_str());
        Satellite::ApiParam configParam = _satellite->findApiParam(_satellite->tokenizeApiParams(params), "CONFIG", true);
        if (onDeviceConfig && configParam.val.has_value())
        {
            onDeviceConfig(configParam.val.value());
        }
    }
    else
    {
        ESP_LOGW("SatelliteSurface", "Unknown command received: %s", command.c_str());
    }
}

/**
 * @brief Send a button action to Companion
 *
 * Sends the appropriate command to Companion based on the action type:
 * - PRESSED/RELEASED: KEY-PRESS command
 * - RIGHT/LEFT: KEY-ROTATE command
 */
void SatelliteSurface::buttonAction(uint8_t buttonIndex, SatelliteAction action)
{
    if (buttonIndex >= _config.totalKeys)
    {
        ESP_LOGW("SatelliteSurface", "Ignoring button action for out-of-range key index %u (totalKeys=%u)", buttonIndex, _config.totalKeys);
        return;
    }

    ESP_LOGI("SatelliteSurface", "Button action %d on button %d", static_cast<int>(action), buttonIndex);
    switch (action)
    {
    case SatelliteAction::PRESSED:
        _satellite->send("KEY-PRESS DEVICEID=" + _surfaceID + " KEY=" + String(buttonIndex) + " PRESSED=true\n");
        break;
    case SatelliteAction::RELEASED:
        _satellite->send("KEY-PRESS DEVICEID=" + _surfaceID + " KEY=" + String(buttonIndex) + " PRESSED=false\n");
        break;
    case SatelliteAction::RIGHT:
        _satellite->send("KEY-ROTATE DEVICEID=" + _surfaceID + " KEY=" + String(buttonIndex) + " DIRECTION=1\n");
        break;
    case SatelliteAction::LEFT:
        _satellite->send("KEY-ROTATE DEVICEID=" + _surfaceID + " KEY=" + String(buttonIndex) + " DIRECTION=-1\n");
        break;
    default:
        ESP_LOGE("SatelliteSurface", "Invalid action for button: %d", static_cast<int>(action));
        return;
    }
}

/**
 * @brief Set a variable value on this surface
 *
 * Sends a SET-VARIABLE-VALUE command to Companion with the base64-encoded value.
 */
void SatelliteSurface::setVariable(const String &id, const String &value)
{
    if (!isSafeApiTokenValue(id))
    {
        ESP_LOGW("SatelliteSurface", "Invalid VARIABLE id (must be non-empty and must not contain quotes/newlines)");
        return;
    }

    ESP_LOGI("SatelliteSurface", "Setting variable %s to %s", id.c_str(), value.c_str());
    // encode value as base64
    String encodedValue = base64::encode(value);
    String command;
    command.reserve(64 + _surfaceID.length() + id.length() + encodedValue.length());
    command = "SET-VARIABLE-VALUE DEVICEID=" + _surfaceID + " VARIABLE=\"" + id + "\" VALUE=\"" + encodedValue + "\"\n";
    _satellite->send(command);
}

/**
 * @brief Press a pincode key on this surface
 *
 * Sends a PINCODE-KEY command to Companion for the specified key index.
 * Used for surfaces that support pincode input functionality.
 */
void SatelliteSurface::pressPincodeKey(uint8_t keyIndex)
{
    if (keyIndex > 9)
    {
        ESP_LOGW("SatelliteSurface", "Invalid pincode key index %u, expected 0-9", keyIndex);
        return;
    }

    ESP_LOGI("SatelliteSurface", "Pressing pincode key: %d", keyIndex);
    _satellite->send("PINCODE-KEY DEVICEID=" + _surfaceID + " KEY=" + String(keyIndex) + "\n");
}

/**
 * @brief Report firmware update availability for this surface (API >= 1.10.0)
 *
 * The update URL may be empty to clear availability. Non-empty values must be
 * protocol-safe (no quotes or newlines).
 */
void SatelliteSurface::reportFirmwareUpdateInfo(const String &updateUrl)
{
    if (updateUrl.length() > 0 && !isSafeApiTokenValue(updateUrl))
    {
        ESP_LOGW("SatelliteSurface", "Invalid UPDATE_URL value (must not contain quotes/newlines)");
        return;
    }

    ESP_LOGI("SatelliteSurface", "Reporting firmware update URL for %s", _surfaceID.c_str());
    _satellite->send("FIRMWARE-UPDATE-INFO DEVICEID=\"" + _surfaceID + "\" UPDATE_URL=\"" + updateUrl + "\"\n");
}

/**
 * @brief Request a page change for this surface (API >= 1.10.0)
 */
void SatelliteSurface::changePage(SatellitePageDirection direction)
{
    int directionValue;
    switch (direction)
    {
    case SatellitePageDirection::NEXT:
        directionValue = 1;
        break;
    case SatellitePageDirection::PREVIOUS:
        // CHANGE-PAGE uses 0 for previous, unlike KEY-ROTATE/SUB-ROTATE which use -1.
        directionValue = 0;
        break;
    default:
        ESP_LOGE("SatelliteSurface", "Invalid direction for page change: %d", static_cast<int>(direction));
        return;
    }

    ESP_LOGI("SatelliteSurface", "Requesting page change for %s direction=%d", _surfaceID.c_str(), directionValue);
    _satellite->send("CHANGE-PAGE DEVICEID=\"" + _surfaceID + "\" DIRECTION=" + String(directionValue) + "\n");
}