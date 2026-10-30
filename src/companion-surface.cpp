/**
 * @file companion-surface.cpp
 * @brief A virtual control surface registered with Companion (ADD-DEVICE)
 * @author Elliot Matson
 */

#include "companion-satellite.hpp"

#include <ArduinoJson.h>
#include <MD5Builder.h>

using satellite_protocol::Message;
using satellite_protocol::MessageBuilder;

[[maybe_unused]] static const char *TAG = "SatelliteSurface";

static String toString(std::string_view value)
{
    String result;
    result.concat(value.data(), value.size());
    return result;
}

static bool isValidControlId(const String &id)
{
    if (id.length() == 0)
    {
        return false;
    }
    for (size_t i = 0; i < id.length(); i++)
    {
        char c = id[i];
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '/')
        {
            return false;
        }
    }
    return true;
}

SatelliteSurface::SatelliteSurface(Satellite &satellite, const String &surfaceName, const SatelliteSurfaceConfig &config, const std::vector<SatelliteSurfaceVariable> &variableList)
    : _satellite(&satellite), _surfaceName(surfaceName), _config(config), _variables(variableList)
{
    MD5Builder md5;
    md5.begin();
    md5.add(surfaceName);
    md5.add(satellite.getDeviceName());
    md5.calculate();
    _surfaceID = "usat:" + md5.toString();

    ESP_LOGI(TAG, "Surface %s has ID %s", surfaceName.c_str(), _surfaceID.c_str());
    _satellite->add(*this);
}

SatelliteSurface::~SatelliteSurface()
{
    _satellite->remove(*this);
}

bool SatelliteSurface::isRegistered() const
{
    Satellite::Lock lock(_satellite->_mutex);
    return _registered;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

std::string SatelliteSurface::buildAddDeviceCommand() const
{
    MessageBuilder command("ADD-DEVICE");
    command.deviceId(_surfaceID.c_str()).str("PRODUCT_NAME", _surfaceName.c_str());

    if (_config.layout)
    {
        String manifest = buildLayoutManifest();
        command.str("LAYOUT_MANIFEST", satellite_protocol::base64Encode(std::string_view(manifest.c_str(), manifest.length())));
    }
    else
    {
        command.num("KEYS_TOTAL", _config.totalKeys).num("KEYS_PER_ROW", _config.keysPerRow);
        if (_config.bitmapSize)
        {
            command.num("BITMAPS", *_config.bitmapSize);
        }
        else
        {
            command.boolean("BITMAPS", false);
        }
        command.boolean("COLORS", _config.sendColors).boolean("TEXT", _config.sendText).boolean("TEXT_STYLE", _config.sendTextStyle);
    }

    command.boolean("BRIGHTNESS", _config.supportsBrightness);

    _satellite->appendBitmapFormat(command, _config.bitmapFormat);

    if (_config.canChangePageLabel && _config.canChangePageLabel->length() > 0)
    {
        command.str("CAN_CHANGE_PAGE", _config.canChangePageLabel->c_str());
    }

    switch (_config.pincodeSupport)
    {
    case SatelliteSurfacePincodeSupport::PARTIAL:
        command.raw("PINCODE_LOCK", "PARTIAL");
        break;
    case SatelliteSurfacePincodeSupport::FULL:
        command.raw("PINCODE_LOCK", "FULL");
        break;
    case SatelliteSurfacePincodeSupport::NONE:
        break;
    }

    if (!_variables.empty())
    {
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();
        for (const SatelliteSurfaceVariable &variable : _variables)
        {
            JsonObject object = array.add<JsonObject>();
            object["id"] = variable.id;
            object["type"] = variable.type == SatelliteSurfaceVariableType::VARIABLE_INPUT ? "input" : "output";
            object["name"] = variable.name;
            object["description"] = variable.description;
        }
        // String rather than std::string, like the other JSON this library writes, so
        // ArduinoJson's serializer is only compiled in once
        String json;
        serializeJson(doc, json);
        command.str("VARIABLES", satellite_protocol::base64Encode(std::string_view(json.c_str(), json.length())));
    }

    if (_config.configFieldsJson && _config.configFieldsJson->length() > 0)
    {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *_config.configFieldsJson);
        if (error)
        {
            ESP_LOGW(TAG, "configFieldsJson is not valid JSON (%s), omitting it", error.c_str());
        }
        else if (!doc.is<JsonArray>())
        {
            ESP_LOGW(TAG, "configFieldsJson must be a JSON array, omitting it");
        }
        else
        {
            command.str("CONFIG_FIELDS", satellite_protocol::base64Encode(std::string_view(_config.configFieldsJson->c_str(), _config.configFieldsJson->length())));
        }
    }

    if (!command.ok())
    {
        ESP_LOGE(TAG, "Surface %s can't be registered: its name or a label contains a newline", _surfaceName.c_str());
        return {};
    }
    ESP_LOGI(TAG, "Registering surface %s", _surfaceID.c_str());
    return command.finish();
}

String SatelliteSurface::buildLayoutManifest() const
{
    const SatelliteLayout &layout = *_config.layout;
    bool nonSquare = _satellite->hasFeature(SatelliteFeature::NonSquareBitmaps);
    bool leds = _satellite->hasFeature(SatelliteFeature::Leds);

    JsonDocument doc;
    JsonObject presets = doc["stylePresets"].to<JsonObject>();
    bool hasDefault = false;

    for (const SatelliteStylePreset &preset : layout.stylePresets)
    {
        if (preset.name.length() == 0)
        {
            ESP_LOGW(TAG, "Ignoring a style preset with no name");
            continue;
        }
        hasDefault |= preset.name == "default";

        JsonObject object = presets[preset.name].to<JsonObject>();
        if (preset.bitmapWidth > 0 && preset.bitmapHeight > 0)
        {
            if (preset.bitmapWidth != preset.bitmapHeight && !nonSquare)
            {
                ESP_LOGW(TAG, "Style preset '%s' asks for a non-square bitmap, which this Companion doesn't advertise", preset.name.c_str());
            }
            object["bitmap"]["w"] = preset.bitmapWidth;
            object["bitmap"]["h"] = preset.bitmapHeight;
        }
        if (preset.colors)
        {
            object["colors"] = "hex";
        }
        if (preset.text)
        {
            object["text"] = true;
        }
        if (preset.textStyle)
        {
            object["textStyle"] = true;
        }
        if (preset.ledSegments > 0)
        {
            if (!leds)
            {
                ESP_LOGW(TAG, "Style preset '%s' asks for LEDs, which need Satellite API 1.13.0 - omitting them", preset.name.c_str());
            }
            else
            {
                object["leds"]["segments"] = std::min<uint8_t>(preset.ledSegments, 200);
                object["leds"]["mode"] = preset.ledsMode == SatelliteLedsMode::FULL_RING ? "full-ring" : "simple";
            }
        }
    }

    if (!hasDefault)
    {
        ESP_LOGW(TAG, "Layout has no \"default\" style preset, adding an empty one");
        presets["default"].to<JsonObject>();
    }

    JsonObject controls = doc["controls"].to<JsonObject>();
    for (const SatelliteLayoutControl &control : layout.controls)
    {
        if (!isValidControlId(control.id))
        {
            ESP_LOGW(TAG, "Ignoring control with invalid id '%s' (use letters, digits, '-' and '/')", control.id.c_str());
            continue;
        }
        JsonObject object = controls[control.id].to<JsonObject>();
        object["row"] = control.row;
        object["column"] = control.column;
        if (control.stylePreset.length() > 0)
        {
            object["stylePreset"] = control.stylePreset;
        }
    }

    String json;
    serializeJson(doc, json);
    return json;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void SatelliteSurface::handleMessage(const Message &message)
{
    const satellite_protocol::Params &params = message.params();
    String command = toString(message.command());

    if (message.isOk() || message.isError())
    {
        if (message.isOk())
        {
            if (message.is("ADD-DEVICE"))
            {
                ESP_LOGI(TAG, "Surface %s registered", _surfaceID.c_str());
                if (onRegistered)
                {
                    onRegistered();
                }
            }
        }
        else if (onError)
        {
            onError(command, toString(params.get("MESSAGE").value_or("")));
        }
        return;
    }

    if (message.is("KEY-STATE"))
    {
        SatelliteControlState state;
        if (std::optional<std::string_view> controlId = params.get("CONTROLID"))
        {
            state.controlId = toString(*controlId);
            if (_config.layout)
            {
                const std::vector<SatelliteLayoutControl> &controls = _config.layout->controls;
                for (size_t i = 0; i < controls.size(); i++)
                {
                    if (controls[i].id == state.controlId)
                    {
                        state.index = static_cast<uint16_t>(i);
                        break;
                    }
                }
            }
        }
        else if (std::optional<std::string_view> key = params.get("KEY"))
        {
            std::optional<double> index = satellite_protocol::parseNumber(*key);
            if (!index || *index < 0)
            {
                ESP_LOGW(TAG, "KEY-STATE with invalid KEY");
                return;
            }
            state.index = static_cast<uint16_t>(*index);
        }
        else
        {
            ESP_LOGW(TAG, "KEY-STATE without KEY or CONTROLID");
            return;
        }

        satelliteParseControlState(params, state);
        if (onKeyStateChange)
        {
            onKeyStateChange(state);
        }
    }
    else if (message.is("KEYS-CLEAR"))
    {
        if (onKeysClear)
        {
            onKeysClear();
        }
    }
    else if (message.is("BRIGHTNESS"))
    {
        std::optional<double> value = satellite_protocol::parseNumber(params.get("VALUE").value_or(""));
        if (onBrightnessSet && value)
        {
            onBrightnessSet(static_cast<uint8_t>(std::max(0.0, std::min(100.0, *value))));
        }
    }
    else if (message.is("VARIABLE-VALUE"))
    {
        std::optional<std::string_view> id = params.get("VARIABLE");
        std::optional<std::string_view> encoded = params.get("VALUE");
        std::optional<std::string> value = encoded ? satellite_protocol::base64Decode(*encoded) : std::nullopt;
        if (onVariableSet && id && value)
        {
            onVariableSet(toString(*id), toString(*value));
        }
    }
    else if (message.is("LOCKED-STATE"))
    {
        SatelliteLockedState state;
        state.locked = params.isTruthy("LOCKED");
        state.characterCount = static_cast<uint8_t>(satellite_protocol::parseNumber(params.get("CHARACTER_COUNT").value_or("0")).value_or(0));
        state.rotation = static_cast<int16_t>(satellite_protocol::parseNumber(params.get("ROTATION").value_or("0")).value_or(0));
        if (onLockedStateSet)
        {
            onLockedStateSet(state);
        }
    }
    else if (message.is("DEVICE-CONFIG"))
    {
        std::optional<std::string_view> encoded = params.get("CONFIG");
        std::optional<std::string> config = encoded ? satellite_protocol::base64Decode(*encoded) : std::nullopt;
        if (onDeviceConfig && config)
        {
            onDeviceConfig(toString(*config));
        }
    }
    else
    {
        ESP_LOGW(TAG, "Unhandled message %s", command.c_str());
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool SatelliteSurface::resolveControl(uint16_t index, const String *&controlId) const
{
    if (_config.layout)
    {
        if (index >= _config.layout->controls.size())
        {
            ESP_LOGW(TAG, "Control index %u out of range (layout has %u controls)", index, static_cast<unsigned>(_config.layout->controls.size()));
            return false;
        }
        controlId = &_config.layout->controls[index].id;
        return true;
    }

    if (index >= _config.totalKeys)
    {
        ESP_LOGW(TAG, "Key index %u out of range (totalKeys=%u)", index, _config.totalKeys);
        return false;
    }
    controlId = nullptr;
    return true;
}

void SatelliteSurface::sendKeyAction(const String *controlId, uint16_t index, SatelliteAction action)
{
    switch (action)
    {
    case SatelliteAction::RIGHT:
        sendRotate(controlId, index, 1);
        return;
    case SatelliteAction::LEFT:
        sendRotate(controlId, index, -1);
        return;
    case SatelliteAction::PRESSED:
    case SatelliteAction::RELEASED:
        break;
    }

    if (!isRegistered())
    {
        ESP_LOGD(TAG, "Surface not registered, dropping key press");
        return;
    }

    MessageBuilder command("KEY-PRESS");
    command.deviceId(_surfaceID.c_str());
    if (controlId)
    {
        command.str("CONTROLID", controlId->c_str());
    }
    else
    {
        command.num("KEY", index);
    }
    command.boolean("PRESSED", action == SatelliteAction::PRESSED);
    _satellite->send(command.finish());
}

void SatelliteSurface::sendRotate(const String *controlId, uint16_t index, int steps)
{
    if (steps == 0)
    {
        return;
    }
    if (!isRegistered())
    {
        ESP_LOGD(TAG, "Surface not registered, dropping rotation");
        return;
    }

    for (int direction : _satellite->rotationDirections(steps))
    {
        MessageBuilder command("KEY-ROTATE");
        command.deviceId(_surfaceID.c_str());
        if (controlId)
        {
            command.str("CONTROLID", controlId->c_str());
        }
        else
        {
            command.num("KEY", index);
        }
        command.num("DIRECTION", direction);
        _satellite->send(command.finish());
    }
}

void SatelliteSurface::buttonAction(uint16_t buttonIndex, SatelliteAction action)
{
    const String *controlId = nullptr;
    if (resolveControl(buttonIndex, controlId))
    {
        sendKeyAction(controlId, buttonIndex, action);
    }
}

void SatelliteSurface::rotate(uint16_t buttonIndex, int steps)
{
    const String *controlId = nullptr;
    if (resolveControl(buttonIndex, controlId))
    {
        sendRotate(controlId, buttonIndex, steps);
    }
}

void SatelliteSurface::controlAction(const String &controlId, SatelliteAction action)
{
    if (!_config.layout)
    {
        ESP_LOGW(TAG, "controlAction() needs a layout surface, use buttonAction()");
        return;
    }
    sendKeyAction(&controlId, 0, action);
}

void SatelliteSurface::rotateControl(const String &controlId, int steps)
{
    if (!_config.layout)
    {
        ESP_LOGW(TAG, "rotateControl() needs a layout surface, use rotate()");
        return;
    }
    sendRotate(&controlId, 0, steps);
}

void SatelliteSurface::setVariable(const String &id, const String &value)
{
    if (!isRegistered())
    {
        ESP_LOGD(TAG, "Surface not registered, dropping variable %s", id.c_str());
        return;
    }

    MessageBuilder command("SET-VARIABLE-VALUE");
    command.deviceId(_surfaceID.c_str())
        .str("VARIABLE", id.c_str())
        .str("VALUE", satellite_protocol::base64Encode(std::string_view(value.c_str(), value.length())));
    if (!command.ok())
    {
        ESP_LOGW(TAG, "Invalid variable id '%s'", id.c_str());
        return;
    }
    _satellite->send(command.finish());
}

void SatelliteSurface::pressPincodeKey(uint8_t keyIndex)
{
    if (keyIndex > 9)
    {
        ESP_LOGW(TAG, "Invalid pincode key %u, expected 0-9", keyIndex);
        return;
    }
    if (!isRegistered())
    {
        ESP_LOGD(TAG, "Surface not registered, dropping pincode key");
        return;
    }
    _satellite->send(MessageBuilder("PINCODE-KEY").deviceId(_surfaceID.c_str()).num("KEY", keyIndex).finish());
}

void SatelliteSurface::reportFirmwareUpdateInfo(const String &updateUrl)
{
    if (!isRegistered())
    {
        ESP_LOGD(TAG, "Surface not registered, dropping firmware update info");
        return;
    }

    MessageBuilder command("FIRMWARE-UPDATE-INFO");
    command.deviceId(_surfaceID.c_str()).str("UPDATE_URL", updateUrl.c_str());
    if (!command.ok())
    {
        ESP_LOGW(TAG, "Invalid update URL");
        return;
    }
    _satellite->send(command.finish());
}

void SatelliteSurface::changePage(SatellitePageDirection direction)
{
    if (!isRegistered())
    {
        ESP_LOGD(TAG, "Surface not registered, dropping page change");
        return;
    }
    // CHANGE-PAGE uses 1 for next and 0 for previous
    _satellite->send(MessageBuilder("CHANGE-PAGE").deviceId(_surfaceID.c_str()).num("DIRECTION", direction == SatellitePageDirection::NEXT ? 1 : 0).finish());
}
