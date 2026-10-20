/**
 * @file companion-subscription.cpp
 * @brief A button subscription (ADD-SUB): observe and use one button by its location
 * @author Elliot Matson
 */

#include "companion-satellite.hpp"

using satellite_protocol::Message;
using satellite_protocol::MessageBuilder;

[[maybe_unused]] static const char *TAG = "SatelliteSubscription";

/** Upper bound Companion applies to a single rotation's step count */
static constexpr int kMaxRotationSteps = 100;

static bool isValidLocation(const String &location)
{
    // page/row/column, each a non-negative integer
    int slashes = 0;
    bool digitInPart = false;
    for (size_t i = 0; i < location.length(); i++)
    {
        char c = location[i];
        if (c >= '0' && c <= '9')
        {
            digitInPart = true;
        }
        else if (c == '/' && digitInPart)
        {
            slashes++;
            digitInPart = false;
        }
        else
        {
            return false;
        }
    }
    return slashes == 2 && digitInPart;
}

SatelliteSubscription::SatelliteSubscription(Satellite &satellite, const String &location, const SatelliteSubscriptionConfig &config)
    : _satellite(&satellite), _location(location), _config(config)
{
    if (!isValidLocation(location))
    {
        ESP_LOGW(TAG, "Location '%s' should be page/row/column, eg. 1/0/3", location.c_str());
    }

    {
        Satellite::Lock lock(_satellite->_mutex);
        _subId = "usat-" + String(_satellite->_nextSubscriptionId++);
    }
    _satellite->add(*this);
}

SatelliteSubscription::~SatelliteSubscription()
{
    _satellite->remove(*this);
}

String SatelliteSubscription::getLocation() const
{
    Satellite::Lock lock(_satellite->_mutex);
    return _location;
}

void SatelliteSubscription::setLocation(const String &location)
{
    if (!isValidLocation(location))
    {
        ESP_LOGW(TAG, "Location '%s' should be page/row/column, eg. 1/0/3", location.c_str());
    }

    // Re-subscribe under the same id at the new location
    _satellite->remove(*this);
    {
        Satellite::Lock lock(_satellite->_mutex);
        _location = location;
    }
    _satellite->add(*this);
}

bool SatelliteSubscription::isActive() const
{
    Satellite::Lock lock(_satellite->_mutex);
    return _active;
}

std::string SatelliteSubscription::buildAddCommand() const
{
    MessageBuilder command("ADD-SUB");
    command.str("SUBID", _subId.c_str()).str("LOCATION", _location.c_str());

    if (_config.bitmapSize)
    {
        command.num("BITMAP", *_config.bitmapSize);
        if (_config.bitmapFormat && _config.bitmapFormat->length() > 0)
        {
            if (_satellite->isBitmapFormatAdvertised(*_config.bitmapFormat))
            {
                command.str("BITMAP_FORMAT", _config.bitmapFormat->c_str());
            }
            else
            {
                ESP_LOGW(TAG, "Bitmap format '%s' not supported by this Companion, using rgb", _config.bitmapFormat->c_str());
            }
        }
    }
    command.boolean("COLORS", _config.colors).boolean("TEXT", _config.text).boolean("TEXT_STYLE", _config.textStyle);

    if (!command.ok())
    {
        ESP_LOGE(TAG, "Invalid location '%s'", _location.c_str());
        return {};
    }
    return command.finish();
}

void SatelliteSubscription::handleMessage(const Message &message)
{
    if (message.isOk())
    {
        return;
    }
    if (message.isError())
    {
        if (_onError)
        {
            std::string_view text = message.params().get("MESSAGE").value_or("");
            String error;
            error.concat(text.data(), text.size());
            _onError(error);
        }
        return;
    }

    if (message.is("SUB-STATE"))
    {
        SatelliteControlState state;
        satelliteParseControlState(message.params(), state);
        if (_onStateChange)
        {
            _onStateChange(state);
        }
    }
}

void SatelliteSubscription::action(SatelliteAction action)
{
    switch (action)
    {
    case SatelliteAction::RIGHT:
        rotate(1);
        return;
    case SatelliteAction::LEFT:
        rotate(-1);
        return;
    case SatelliteAction::PRESSED:
    case SatelliteAction::RELEASED:
        break;
    }

    if (!isActive())
    {
        ESP_LOGD(TAG, "Subscription %s not active, dropping press", _subId.c_str());
        return;
    }
    _satellite->send(MessageBuilder("SUB-PRESS").str("SUBID", _subId.c_str()).boolean("PRESSED", action == SatelliteAction::PRESSED).finish());
}

void SatelliteSubscription::rotate(int steps)
{
    if (steps == 0)
    {
        return;
    }
    if (!isActive())
    {
        ESP_LOGD(TAG, "Subscription %s not active, dropping rotation", _subId.c_str());
        return;
    }

    bool amounts = _satellite->supportsRotaryAmount();
    while (steps != 0)
    {
        int chunk = amounts ? std::max(-kMaxRotationSteps, std::min(kMaxRotationSteps, steps)) : (steps > 0 ? 1 : -1);
        steps -= chunk;
        _satellite->send(MessageBuilder("SUB-ROTATE").str("SUBID", _subId.c_str()).num("DIRECTION", chunk).finish());
    }
}
