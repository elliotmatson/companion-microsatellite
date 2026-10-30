/**
 * @file companion-subscription.hpp
 * @brief A button subscription (ADD-SUB): observe and use one button by its location
 */

#ifndef COMPANION_SUBSCRIPTION_HPP
#define COMPANION_SUBSCRIPTION_HPP

#include <Arduino.h>
#include <functional>
#include <optional>
#include <string>

#include "companion-satellite-types.hpp"

class Satellite;

/**
 * @brief What a subscription should receive
 */
struct SatelliteSubscriptionConfig
{
    std::optional<uint16_t> bitmapSize = std::nullopt; /**< Square bitmap size in pixels, unset for no bitmap */
    std::optional<String> bitmapFormat = std::nullopt; /**< rgb/png/webp, falls back to rgb if not advertised */
    bool colors = true;                                /**< Receive COLOR/TEXTCOLOR */
    bool text = true;                                  /**< Receive TEXT */
    bool textStyle = false;                            /**< Receive FONT_SIZE */
};

/**
 * @brief Follows a single button at an absolute page/row/column location
 *
 * Unlike a SatelliteSurface, a subscription does not appear in Companion's Surfaces list.
 * It is (re)registered automatically every time the connection becomes ready, as long
 * as button subscriptions are enabled in Companion's settings (see
 * Satellite::supportsSubscriptions).
 */
class SatelliteSubscription
{
public:
    /**
     * @brief Create a subscription
     * @param satellite The satellite to use (must outlive this subscription)
     * @param location The button location, "page/row/column" (eg. "1/0/3")
     * @param config What to receive
     */
    SatelliteSubscription(Satellite &satellite, const String &location, const SatelliteSubscriptionConfig &config = SatelliteSubscriptionConfig{});

    /** @brief Removes the subscription */
    ~SatelliteSubscription();

    SatelliteSubscription(const SatelliteSubscription &) = delete;
    SatelliteSubscription &operator=(const SatelliteSubscription &) = delete;

    /** @brief The subscription id used on the wire */
    const String &getID() const { return _subId; }

    /** @brief The subscribed location */
    String getLocation() const;

    /** @brief Follow a different location */
    void setLocation(const String &location);

    /** @brief Whether Companion has accepted the subscription on the current connection */
    bool isActive() const;

    /** @brief Press, release or rotate (a single step) the button */
    void action(SatelliteAction action);

    /** @brief Rotate by a number of steps, positive for clockwise (see SatelliteSurface::rotate) */
    void rotate(int steps);

    /** @brief Called with the button's state, once after subscribing and then whenever it changes */
    void setOnStateChange(std::function<void(const SatelliteControlState &)> callback) { _onStateChange = std::move(callback); }

    /** @brief Called when Companion rejects the subscription (eg. an invalid location) */
    void setOnError(std::function<void(const String &)> callback) { _onError = std::move(callback); }

private:
    friend class Satellite;

    void handleMessage(const satellite_protocol::Message &message);
    std::string buildAddCommand() const;

    Satellite *_satellite;
    String _subId;
    String _location; /**< Guarded by the satellite's mutex */
    SatelliteSubscriptionConfig _config;
    bool _active = false; /**< Guarded by the satellite's mutex */

    std::function<void(const SatelliteControlState &)> _onStateChange;
    std::function<void(const String &)> _onError;
};

#endif // COMPANION_SUBSCRIPTION_HPP
