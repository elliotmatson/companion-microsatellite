/**
 * @file companion-satellite.hpp
 * @brief Companion Satellite API library for ESP32 microcontrollers
 * @author Elliot Matson
 * @version 1.0.1
 *
 * Connects ESP32 based devices to Bitfocus Companion via the Companion Satellite API,
 * as virtual surfaces (SatelliteSurface) and/or button subscriptions
 * (SatelliteSubscription).
 *
 * Including this header gives you the whole public API.
 *
 * Threading: by default, every callback (key state, variables, connection state...)
 * runs inside Satellite::loop(), so call that from your Arduino loop(). That keeps
 * your drawing/LED code off the network task, and lets you use the same display or
 * bus from loop() without locking. See SatelliteOptions::dispatch.
 */

#ifndef COMPANION_SATELLITE_HPP
#define COMPANION_SATELLITE_HPP

#include <Arduino.h>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <Preferences.h>
#include <Ticker.h>

#include "companion-satellite-types.hpp"
#include "companion-surface.hpp"
#include "companion-subscription.hpp"
#include "satellite-protocol.hpp"

// Only used as pointers here; companion-satellite.cpp includes the real headers.
class AsyncWebServer;
class AsyncClient;
struct pbuf;

/**
 * @brief Where callbacks run
 */
enum class SatelliteDispatchMode
{
    /**
     * Callbacks run inside Satellite::loop(), on whichever task calls it (usually the
     * Arduino loop task). Received updates are queued until then, and repeated updates
     * to the same key are merged so a slow loop() only sees the latest state.
     */
    LOOP,

    /**
     * Callbacks run immediately on the AsyncTCP network task. Lowest latency, but keep
     * them short and non-blocking, and make anything they share with other tasks
     * thread-safe. loop() does not need to be called.
     */
    IMMEDIATE,
};

/**
 * @brief Options for Satellite::begin
 */
struct SatelliteOptions
{
    /** Where callbacks run - see SatelliteDispatchMode */
    SatelliteDispatchMode dispatch = SatelliteDispatchMode::LOOP;

    /** Serve the REST config API (GET/POST /api/host, /api/port, /api/config) on the API port */
    bool webConfig = true;

    /** Advertise this device over mDNS, so Companion can discover and configure it */
    bool mdns = true;

    /**
     * The longest message that will be accepted, in bytes. Longer ones (eg. bitmaps
     * bigger than you asked for) are skipped. A raw rgb bitmap of W x H pixels takes
     * about W * H * 4 bytes, plus a couple of hundred for the rest of the message.
     */
    size_t maxLineLength = 32768;

    /**
     * In LOOP mode, the most received data to hold waiting for loop(), in bytes. Once the
     * queue is within maxLineLength of this, receiving pauses (TCP flow control: Companion
     * waits) until loop() catches up, so updates are only dropped if it's set below that.
     */
    size_t maxQueuedBytes = 65536;

    /** Reconnect if nothing has been received from Companion for this long (Companion answers our PING every 2s) */
    uint32_t keepaliveTimeoutMs = 6000;
};

/**
 * @brief The connection to Companion
 *
 * Owns the TCP connection, reconnects as needed, and registers every SatelliteSurface
 * and SatelliteSubscription created against it each time the connection becomes ready.
 */
class Satellite
{
public:
    /** @brief Default name shown in Companion */
    static constexpr const char *kDefaultDeviceName = "Companion Microsatellite";

    /** @brief Default mDNS hostname prefix */
    static constexpr const char *kDefaultHostname = "microsatellite";

    /** @brief Default port for the REST config API */
    static constexpr uint16_t kDefaultApiPort = 9999;

    /** @brief Companion's Satellite API TCP port */
    static constexpr uint16_t kCompanionPort = 16622;

    /**
     * @brief Construct a new Satellite
     * @param deviceName Name of the device (displayed in Companion). The chip id is appended
     * @param apiPort Port for the REST config API
     */
    Satellite(const char *deviceName = kDefaultDeviceName, uint16_t apiPort = kDefaultApiPort);
    ~Satellite();

    Satellite(const Satellite &) = delete;
    Satellite &operator=(const Satellite &) = delete;

    /**
     * @brief Start the satellite: load the saved target, start the config API and mDNS, and connect
     *
     * Call after the network (WiFi/Ethernet) is up.
     */
    void begin(const SatelliteOptions &options = SatelliteOptions{});

    /**
     * @brief Run queued callbacks
     *
     * Call regularly (eg. from your Arduino loop()) when using SatelliteDispatchMode::LOOP.
     */
    void loop();

    /** @brief The device name, with the chip id */
    String getDeviceName() const { return _deviceName; }

    /** @brief The mDNS hostname */
    String getHostname() const { return _hostname; }

    /** @brief The Companion host to connect to */
    String getTargetHostname() const;

    /** @brief The Companion port to connect to */
    uint16_t getTargetPort() const;

    /** @brief Set (and save) the Companion host, and reconnect */
    void setTargetHostname(const String &hostname);

    /** @brief Set (and save) the Companion port, and reconnect */
    void setTargetPort(uint16_t port);

    /** @brief Set (and save) the Companion host and port together, and reconnect once */
    void setTarget(const String &hostname, uint16_t port = kCompanionPort);

    /** @brief Companion's version, from BEGIN, or "unknown" */
    String getCompanionVersion() const;

    /** @brief Companion's Satellite API version, from BEGIN, or "unknown" */
    String getSatelliteApiVersion() const;

    /** @brief The state of the connection */
    SatelliteConnectionState getConnectionState() const;

    /** @brief Whether the TCP connection is up (negotiation may still be in progress) */
    bool isConnected() const;

    /** @brief Whether BEGIN and CAPS have been received and the connection is usable */
    bool isReady() const { return getConnectionState() == SatelliteConnectionState::READY; }

    /**
     * @brief Whether a feature is available on the current connection
     *
     * Combines the API version from BEGIN with the flags from CAPS. Always false until ready.
     */
    bool hasFeature(SatelliteFeature feature) const;

    /** @brief Whether button subscriptions are enabled (they can be turned off in Companion's settings) */
    bool supportsSubscriptions() const { return hasFeature(SatelliteFeature::Subscriptions); }

    /** @brief Whether non-square bitmaps are supported */
    bool supportsNonsquareButtons() const { return hasFeature(SatelliteFeature::NonSquareBitmaps); }

    /** @brief Whether a rotation can carry a step count (otherwise multi-step rotations are sent as single steps) */
    bool supportsRotaryAmount() const { return hasFeature(SatelliteFeature::RotaryAmount); }

    /** @brief Bitmap formats advertised in CAPS (eg rgb/png/webp) */
    std::vector<String> getCapsBitmapFormats() const;

    /**
     * @brief Set a callback for connection state changes
     *
     * Use READY to (re)send anything Companion needs after a reconnect. Surfaces and
     * subscriptions are re-registered automatically.
     */
    void setOnConnectionStateChange(std::function<void(SatelliteConnectionState)> callback) { _onConnectionStateChange = std::move(callback); }

private:
    friend class SatelliteSurface;
    friend class SatelliteSubscription;

    using Message = satellite_protocol::Message;

    /** @brief What a queued item is for */
    enum class TargetKind : uint8_t
    {
        CONNECTION,
        SURFACE,
        SUBSCRIPTION,
    };

    struct QueuedItem
    {
        TargetKind kind = TargetKind::CONNECTION;
        SatelliteConnectionState state = SatelliteConnectionState::IDLE; /**< For CONNECTION items */
        std::string target;                                              /**< Device id or subscription id */
        std::string coalesceKey;                                         /**< Non-empty when a newer item may replace this one */
        std::unique_ptr<Message> message;
        size_t bytes = 0;
    };

    /**
     * A client that is no longer in use. AsyncTCP may still be inside one of its callbacks
     * (eg. we closed it from its own onData), or have a DNS lookup in flight for it, so it
     * is only deleted later, from the timer task, once that can no longer be the case.
     */
    struct RetiredClient
    {
        AsyncClient *client;
        uint32_t retiredAt;
        bool disposed;       /**< onDisconnect has fired, so AsyncTCP is done with it */
        uint32_t disposedAt; /**< When it was disposed */
    };

    /** @brief RAII holder for _mutex */
    class Lock
    {
    public:
        explicit Lock(SemaphoreHandle_t mutex)
            : _mutex(mutex) { xSemaphoreTakeRecursive(_mutex, portMAX_DELAY); }
        ~Lock() { xSemaphoreGiveRecursive(_mutex); }
        Lock(const Lock &) = delete;
        Lock &operator=(const Lock &) = delete;

    private:
        SemaphoreHandle_t _mutex;
    };

    // --- connection (companion-satellite.cpp) ---
    void connectToCompanion();
    void retireClient();
    void scheduleReconnect();
    void tick();
    void setState(SatelliteConnectionState state);
    void onClientConnect(AsyncClient *client);
    void onClientDisconnect(AsyncClient *client);
    void sendAddDevice(SatelliteSurface &surface);
    void retryUnansweredAddDevices(uint32_t now);
    void onClientPacket(AsyncClient *client, pbuf *pb);
    void onClientData(AsyncClient *client, const char *data, size_t length);
    size_t receivePauseThreshold() const;
    void resumeReceiving();
    void dropHeldPackets();
    bool handleLine(AsyncClient *client, std::string &&line);
    void handleBegin(const Message &message);
    void handleCaps(std::unique_ptr<Message> message);
    void handleResponse(std::unique_ptr<Message> message);
    void registerAll();
    void resetRegistrations();
    bool send(std::string &&line);
    void flushTx();

    // --- dispatch ---
    void enqueue(QueuedItem &&item);
    void drainQueue();
    void purgeQueue(TargetKind kind, const String &target);
    void dispatch(QueuedItem &item);
    void dispatchToSurface(const std::string &deviceId, const Message &message);
    void dispatchToSubscription(const std::string &subId, const Message &message);

    // --- surfaces and subscriptions ---
    bool add(SatelliteSurface &surface);
    void remove(SatelliteSurface &surface);
    void add(SatelliteSubscription &subscription);
    void remove(SatelliteSubscription &subscription);
    void sendAddSubscription(SatelliteSubscription &subscription);
    bool isBitmapFormatAdvertised(const String &format) const;

    /** @brief Add BITMAP_FORMAT if one was requested and Companion supports it (shared by ADD-DEVICE and ADD-SUB) */
    void appendBitmapFormat(satellite_protocol::MessageBuilder &command, const std::optional<String> &format) const;

    /**
     * @brief Split a rotation into the DIRECTION values to send (shared by KEY-ROTATE and SUB-ROTATE)
     *
     * With ROTARY_AMOUNT the whole rotation goes in one message (in chunks of up to Companion's
     * limit), otherwise each step is its own +1/-1 message. Never 0, which Companion reads as a
     * counter-clockwise step for compatibility.
     */
    std::vector<int> rotationDirections(int steps) const;

    // --- config API and mDNS (companion-satellite-web.cpp) ---

    /** @brief HTTP status and plain-text body to answer a config API write with */
    struct ConfigApiResponse
    {
        int status;
        const char *message;
    };

    /**
     * @brief Apply a POST to /api/host, /api/port or /api/config, and say how to answer it
     *
     * Kept apart from the web server routing (companion-satellite-web.cpp) so the host tests can drive it.
     */
    ConfigApiResponse handleConfigPost(const String &path, String body);

    void beginWebConfig();
    void endWebConfig();
    void beginMdns();

    void saveTarget();

    String _deviceName;
    String _hostname;
    uint16_t _apiPort;

    SatelliteOptions _options;
    bool _begun = false;

    /**
     * Recursive mutex guarding everything below that is shared between the network task,
     * the timer task and application code. Recursive so call chains that already hold it
     * (eg. registering surfaces while handling CAPS, which then sends) don't deadlock.
     * Callbacks into application code are never made while holding it.
     */
    SemaphoreHandle_t _mutex = xSemaphoreCreateRecursiveMutex();

    /** Held while running callbacks, so they never run concurrently on two tasks */
    SemaphoreHandle_t _dispatchMutex = xSemaphoreCreateRecursiveMutex();

    String _targetHostname;
    uint16_t _targetPort = 0;

    SatelliteConnectionState _state = SatelliteConnectionState::IDLE;
    String _companionVersion = "unknown";
    String _apiVersionText = "unknown";
    std::optional<satellite_protocol::ApiVersion> _apiVersion;
    std::unique_ptr<Message> _caps; /**< The CAPS message, kept as its params point into it */
    std::vector<String> _capsBitmapFormats;

    AsyncClient *_client = nullptr;
    std::vector<RetiredClient> _retiredClients;
    std::string _txBuffer;
    satellite_protocol::LineBuffer _rxBuffer{32768}; /**< Only used on the network task */
    uint32_t _lastRxMs = 0;
    uint32_t _lastPingMs = 0;
    uint32_t _stateSinceMs = 0;
    uint32_t _reconnectAtMs = 0;
    uint32_t _reconnectDelayMs = 1000;

    std::deque<QueuedItem> _queue;
    size_t _queuedBytes = 0;
    bool _rxPaused = false;           /**< Holding acks back so Companion stops sending until loop() catches up */
    std::vector<pbuf *> _heldPackets; /**< Received while paused, acked by resumeReceiving() */
    uint32_t _lastLoopMs = 0;
    bool _loopWarningLogged = false;
    uint32_t _droppedUpdates = 0;

    std::list<SatelliteSurface *> _surfaces;
    std::list<SatelliteSubscription *> _subscriptions;
    std::deque<String> _pendingAddSubs; /**< Subscription ids awaiting ADD-SUB OK/ERROR, in request order */
    uint32_t _nextSubscriptionId = 1;

    AsyncWebServer *_server = nullptr;
    Preferences _prefs;
    Ticker _ticker;

    std::function<void(SatelliteConnectionState)> _onConnectionStateChange;
};

#endif // COMPANION_SATELLITE_HPP
