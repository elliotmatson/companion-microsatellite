/**
 * @file companion-satellite.cpp
 * @brief The connection to Companion: TCP lifecycle, keepalive, negotiation and dispatch
 * @author Elliot Matson
 */

#include "companion-satellite.hpp"

#include <algorithm>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <MD5Builder.h>
#include <new>
#include <strings.h>

using satellite_protocol::ApiVersion;
using satellite_protocol::Message;

[[maybe_unused]] static const char *TAG = "Satellite";

/** Oldest Companion Satellite API this library talks to */
static constexpr ApiVersion kMinimumApiVersion{1, 10, 0};

static constexpr uint32_t kTickIntervalMs = 250;
static constexpr uint32_t kPingIntervalMs = 2000;
static constexpr uint32_t kConnectTimeoutMs = 10000;
static constexpr uint32_t kNegotiateTimeoutMs = 10000;
static constexpr uint32_t kInitialReconnectDelayMs = 1000;
static constexpr uint32_t kMaxReconnectDelayMs = 30000;
static constexpr uint32_t kRetiredClientGraceMs = 1000;
static constexpr uint32_t kRetiredClientMaxAgeMs = 60000;
static constexpr uint32_t kLoopWarningMs = 5000;

/** Most outgoing data to hold while waiting for TCP send buffer space */
static constexpr size_t kMaxTxBufferBytes = 16384;

const char *satelliteConnectionStateName(SatelliteConnectionState state)
{
    switch (state)
    {
    case SatelliteConnectionState::IDLE:
        return "IDLE";
    case SatelliteConnectionState::CONNECTING:
        return "CONNECTING";
    case SatelliteConnectionState::NEGOTIATING:
        return "NEGOTIATING";
    case SatelliteConnectionState::READY:
        return "READY";
    case SatelliteConnectionState::REJECTED:
        return "REJECTED";
    case SatelliteConnectionState::WAITING:
        return "WAITING";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// SatelliteEncodedData
// ---------------------------------------------------------------------------

std::string_view SatelliteEncodedData::mimeType() const
{
    if (!isDataUrl())
    {
        return {};
    }
    size_t end = _value.find_first_of(";,", 5);
    return end == std::string_view::npos ? std::string_view{} : _value.substr(5, end - 5);
}

std::string_view SatelliteEncodedData::base64() const
{
    if (!isDataUrl())
    {
        return _value;
    }
    size_t comma = _value.find(',');
    return comma == std::string_view::npos ? std::string_view{} : _value.substr(comma + 1);
}

size_t SatelliteEncodedData::decode(uint8_t *out, size_t capacity) const
{
    return satellite_protocol::base64Decode(base64(), out, capacity).value_or(0);
}

static String toString(std::string_view value)
{
    String result;
    result.concat(value.data(), value.size());
    return result;
}

void satelliteParseControlState(const satellite_protocol::Params &params, SatelliteControlState &state)
{
    std::string_view type = params.get("TYPE").value_or("BUTTON");
    if (type == "PAGEUP")
        state.type = SatelliteSurfaceKeyType::PAGEUP;
    else if (type == "PAGEDOWN")
        state.type = SatelliteSurfaceKeyType::PAGEDOWN;
    else if (type == "PAGENUM")
        state.type = SatelliteSurfaceKeyType::PAGENUM;
    else
        state.type = SatelliteSurfaceKeyType::BUTTON;

    if (std::optional<std::string_view> location = params.get("LOCATION"))
    {
        state.location = toString(*location);
    }
    if (std::optional<std::string_view> bitmap = params.get("BITMAP"))
    {
        state.bitmap = SatelliteEncodedData(*bitmap);
    }
    if (std::optional<std::string_view> leds = params.get("LEDS"))
    {
        state.leds = SatelliteEncodedData(*leds);
    }
    if (std::optional<std::string_view> value = params.get("COLOR"))
    {
        if (std::optional<satellite_protocol::Rgb> rgb = satellite_protocol::parseColor(*value))
        {
            state.keyColor = SatelliteColor{rgb->r, rgb->g, rgb->b};
        }
    }
    if (std::optional<std::string_view> value = params.get("TEXTCOLOR"))
    {
        if (std::optional<satellite_protocol::Rgb> rgb = satellite_protocol::parseColor(*value))
        {
            state.textColor = SatelliteColor{rgb->r, rgb->g, rgb->b};
        }
    }
    if (std::optional<std::string_view> value = params.get("TEXT"))
    {
        std::optional<std::string> decoded = satellite_protocol::base64Decode(*value);
        state.text = decoded ? toString(*decoded) : String();
    }
    if (std::optional<std::string_view> value = params.get("FONT_SIZE"))
    {
        // Companion sends "auto" when the button's text size is automatic
        std::optional<double> size = satellite_protocol::parseNumber(*value);
        state.fontSize = (size && *size > 0) ? static_cast<uint8_t>(std::min(*size, 255.0)) : 0;
    }
    if (params.has("PRESSED"))
    {
        state.pressed = params.isTruthy("PRESSED");
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Satellite::Satellite(const char *deviceName, uint16_t apiPort)
    : _apiPort(apiPort)
{
    char chipId[18];
    snprintf(chipId, sizeof(chipId), "%llx", ESP.getEfuseMac());
    _deviceName = String(deviceName) + " (" + String(chipId) + ")";
    _hostname = String(kDefaultHostname) + "-" + String(chipId);
}

Satellite::~Satellite()
{
    _ticker.detach();

    {
        Lock lock(_mutex);
        if (_client)
        {
            // Stop it calling back into this object, and close it
            _client->onConnect(nullptr);
            _client->onDisconnect(nullptr);
            _client->onData(nullptr);
            _client->onAck(nullptr);
            _client->onError(nullptr);
            _client->close();
            // While still connecting, a DNS lookup may be in flight that AsyncTCP will
            // complete on this client later, so it has to be leaked rather than deleted
            if (_state != SatelliteConnectionState::CONNECTING)
            {
                delete _client;
            }
            _client = nullptr;
        }
        for (const RetiredClient &retired : _retiredClients)
        {
            if (retired.disposed)
            {
                delete retired.client;
            }
        }
        _retiredClients.clear();
    }

    endWebConfig();

    vSemaphoreDelete(_dispatchMutex);
    vSemaphoreDelete(_mutex);
}

void Satellite::begin(const SatelliteOptions &options)
{
    {
        Lock lock(_mutex);
        if (_begun)
        {
            ESP_LOGW(TAG, "begin() called twice, ignoring");
            return;
        }
        _begun = true;
        _options = options;
        _rxBuffer.setMaxLineLength(options.maxLineLength);
        _lastLoopMs = millis();
    }

    // Load the saved target. The namespace is derived from the device name, so several
    // satellites on one chip keep separate settings.
    MD5Builder md5;
    md5.begin();
    md5.add(_deviceName);
    md5.calculate();
    String prefsNamespace = "usat" + md5.toString().substring(0, 8);
    _prefs.begin(prefsNamespace.c_str(), false);
    bool targetSetInCode;
    {
        Lock lock(_mutex);
        targetSetInCode = _targetHostname.length() > 0;
        if (!targetSetInCode)
        {
            _targetHostname = _prefs.getString("host", "");
            _targetPort = _prefs.getUInt("port", 0);
        }
    }
    if (targetSetInCode)
    {
        // setTarget() before begin() wins over (and replaces) whatever was saved
        saveTarget();
    }

    if (options.webConfig)
    {
        beginWebConfig();
    }
    if (options.mdns)
    {
        beginMdns();
    }

    _ticker.attach_ms(kTickIntervalMs, [this]()
                      { tick(); });

    Lock lock(_mutex);
    connectToCompanion();
}

void Satellite::loop()
{
    drainQueue();
}

// ---------------------------------------------------------------------------
// Target and status
// ---------------------------------------------------------------------------

String Satellite::getTargetHostname() const
{
    Lock lock(_mutex);
    return _targetHostname;
}

uint16_t Satellite::getTargetPort() const
{
    Lock lock(_mutex);
    return _targetPort;
}

void Satellite::setTargetHostname(const String &hostname)
{
    setTarget(hostname, getTargetPort());
}

void Satellite::setTargetPort(uint16_t port)
{
    setTarget(getTargetHostname(), port);
}

void Satellite::setTarget(const String &hostname, uint16_t port)
{
    ESP_LOGI(TAG, "Setting target: %s:%u", hostname.c_str(), port);
    {
        Lock lock(_mutex);
        _targetHostname = hostname;
        _targetPort = port;
        _reconnectDelayMs = kInitialReconnectDelayMs;
    }
    saveTarget();

    Lock lock(_mutex);
    if (_begun)
    {
        connectToCompanion();
    }
}

static bool parsePort(JsonVariantConst value, uint16_t &port)
{
    if (value.is<int>())
    {
        int number = value.as<int>();
        if (number > 0 && number <= 65535)
        {
            port = static_cast<uint16_t>(number);
            return true;
        }
    }
    return false;
}

Satellite::ConfigApiResponse Satellite::handleConfigPost(const String &path, String body)
{
    body.trim();
    JsonDocument doc;
    bool isJson = deserializeJson(doc, body) == DeserializationError::Ok && doc.is<JsonObject>();

    if (path == "/api/host")
    {
        String host = isJson ? doc["host"].as<String>() : body;
        if (host.length() == 0 || host == "null")
        {
            return {400, "Missing host"};
        }
        setTargetHostname(host);
    }
    else if (path == "/api/port")
    {
        uint16_t port = 0;
        if (isJson ? !parsePort(doc["port"], port) : !(body.toInt() > 0 && body.toInt() <= 65535))
        {
            return {400, "Invalid port"};
        }
        if (!isJson)
        {
            port = static_cast<uint16_t>(body.toInt());
        }
        setTargetPort(port);
    }
    else if (path == "/api/config")
    {
        if (!isJson)
        {
            return {400, "Invalid JSON"};
        }
        String host = doc["host"].is<const char *>() ? doc["host"].as<String>() : getTargetHostname();
        uint16_t port = getTargetPort();
        if (!doc["port"].isNull() && !parsePort(doc["port"], port))
        {
            return {400, "Invalid port"};
        }
        setTarget(host, port);
    }
    else
    {
        return {404, "Not Found"};
    }
    return {200, "OK"};
}

void Satellite::saveTarget()
{
    String host;
    uint16_t port;
    {
        Lock lock(_mutex);
        if (!_begun)
        {
            return; // Preferences aren't open yet, begin() will load whatever was saved before
        }
        host = _targetHostname;
        port = _targetPort;
    }
    _prefs.putString("host", host);
    _prefs.putUInt("port", port);
}

String Satellite::getCompanionVersion() const
{
    Lock lock(_mutex);
    return _companionVersion;
}

String Satellite::getSatelliteApiVersion() const
{
    Lock lock(_mutex);
    return _apiVersionText;
}

SatelliteConnectionState Satellite::getConnectionState() const
{
    Lock lock(_mutex);
    return _state;
}

bool Satellite::isConnected() const
{
    Lock lock(_mutex);
    return _client != nullptr && (_state == SatelliteConnectionState::NEGOTIATING || _state == SatelliteConnectionState::READY);
}

bool Satellite::hasFeature(SatelliteFeature feature) const
{
    Lock lock(_mutex);
    if (!_apiVersion || !_caps)
    {
        return false;
    }
    return satellite_protocol::isFeatureEnabled(feature, *_apiVersion, &_caps->params());
}

std::vector<String> Satellite::getCapsBitmapFormats() const
{
    Lock lock(_mutex);
    return _capsBitmapFormats;
}

bool Satellite::isBitmapFormatAdvertised(const String &format) const
{
    Lock lock(_mutex);
    if (format == "rgb")
    {
        return true; // always supported, and the default
    }
    if (!hasFeature(SatelliteFeature::BitmapFormats))
    {
        return false;
    }
    return std::find(_capsBitmapFormats.begin(), _capsBitmapFormats.end(), format) != _capsBitmapFormats.end();
}

void Satellite::appendBitmapFormat(satellite_protocol::MessageBuilder &command, const std::optional<String> &format) const
{
    if (!format || format->length() == 0)
    {
        return;
    }
    if (isBitmapFormatAdvertised(*format))
    {
        command.str("BITMAP_FORMAT", format->c_str());
    }
    else
    {
        ESP_LOGW(TAG, "Bitmap format '%s' not supported by this Companion, using rgb", format->c_str());
    }
}

std::vector<int> Satellite::rotationDirections(int steps) const
{
    /** Upper bound Companion applies to a single rotation's step count */
    constexpr int kMaxRotationSteps = 100;

    std::vector<int> directions;
    bool amounts = supportsRotaryAmount();
    while (steps != 0)
    {
        int chunk = amounts ? std::max(-kMaxRotationSteps, std::min(kMaxRotationSteps, steps)) : (steps > 0 ? 1 : -1);
        directions.push_back(chunk);
        steps -= chunk;
    }
    return directions;
}

// ---------------------------------------------------------------------------
// Connection (all called with _mutex held)
// ---------------------------------------------------------------------------

void Satellite::setState(SatelliteConnectionState state)
{
    if (_state == state)
    {
        return;
    }

    ESP_LOGI(TAG, "Connection state: %s -> %s", satelliteConnectionStateName(_state), satelliteConnectionStateName(state));
    _state = state;
    _stateSinceMs = millis();

    QueuedItem item;
    item.kind = TargetKind::CONNECTION;
    item.state = state;
    enqueue(std::move(item));
}

void Satellite::retireClient()
{
    if (!_client)
    {
        return;
    }

    AsyncClient *client = _client;
    _client = nullptr;
    _retiredClients.push_back({client, millis(), false, 0});
    resetRegistrations();
    _txBuffer.clear();

    // If there is a connection, this calls onDisconnect synchronously, which marks it
    // disposed. Without one (eg. still resolving the hostname) that happens later.
    client->close();
}

void Satellite::resetRegistrations()
{
    for (SatelliteSurface *surface : _surfaces)
    {
        surface->_registered = false;
    }
    for (SatelliteSubscription *subscription : _subscriptions)
    {
        subscription->_active = false;
    }
    _pendingAddSubs.clear();
}

void Satellite::connectToCompanion()
{
    retireClient();
    resetRegistrations();
    _txBuffer.clear();
    _txBuffer.shrink_to_fit();
    _caps.reset();
    _apiVersion.reset();
    _capsBitmapFormats.clear();

    if (_targetHostname.length() == 0 || _targetPort == 0)
    {
        ESP_LOGW(TAG, "Companion host/port not set - configure it via the REST API or setTarget()");
        setState(SatelliteConnectionState::IDLE);
        return;
    }

    AsyncClient *client = new (std::nothrow) AsyncClient();
    if (!client)
    {
        ESP_LOGE(TAG, "Out of memory creating client");
        scheduleReconnect();
        return;
    }

    client->onConnect([this](void *, AsyncClient *c)
                      { onClientConnect(c); });
    client->onDisconnect([this](void *, AsyncClient *c)
                         { onClientDisconnect(c); });
    client->onData([this](void *, AsyncClient *c, void *data, size_t length)
                   { onClientData(c, static_cast<const char *>(data), length); });
    client->onAck([this](void *, AsyncClient *c, size_t, uint32_t)
                  {
                      Lock lock(_mutex);
                      if (c == _client)
                      {
                          flushTx();
                      } });
    client->onError([](void *, AsyncClient *, int8_t error)
                    { ESP_LOGW(TAG, "Connection error: %s", AsyncClient::errorToString(error)); });

    _client = client;
    setState(SatelliteConnectionState::CONNECTING);
    ESP_LOGI(TAG, "Connecting to %s:%u", _targetHostname.c_str(), _targetPort);

    if (!client->connect(_targetHostname.c_str(), _targetPort))
    {
        // A synchronous failure never calls onDisconnect, so retry from here
        ESP_LOGW(TAG, "Failed to start connecting to %s:%u", _targetHostname.c_str(), _targetPort);
        _client = nullptr;
        _retiredClients.push_back({client, millis(), false, 0});
        scheduleReconnect();
    }
}

void Satellite::scheduleReconnect()
{
    _reconnectAtMs = millis() + _reconnectDelayMs;
    ESP_LOGI(TAG, "Reconnecting in %u ms", static_cast<unsigned>(_reconnectDelayMs));
    _reconnectDelayMs = std::min(_reconnectDelayMs * 2, kMaxReconnectDelayMs);
    setState(SatelliteConnectionState::WAITING);
}

/**
 * Housekeeping, every kTickIntervalMs on the esp_timer task: reconnects, timeouts,
 * keepalive pings, flushing the send buffer and freeing retired clients.
 */
void Satellite::tick()
{
    {
        Lock lock(_mutex);
        uint32_t now = millis();

        for (auto it = _retiredClients.begin(); it != _retiredClients.end();)
        {
            bool expired = it->disposed ? (now - it->disposedAt >= kRetiredClientGraceMs)
                                        : (now - it->retiredAt >= kRetiredClientMaxAgeMs && it->client->free());
            if (expired)
            {
                delete it->client;
                it = _retiredClients.erase(it);
            }
            else
            {
                ++it;
            }
        }

        switch (_state)
        {
        case SatelliteConnectionState::WAITING:
            if (static_cast<int32_t>(now - _reconnectAtMs) >= 0)
            {
                connectToCompanion();
            }
            break;

        case SatelliteConnectionState::CONNECTING:
            if (now - _stateSinceMs >= kConnectTimeoutMs)
            {
                ESP_LOGW(TAG, "Timed out connecting to Companion");
                retireClient();
                scheduleReconnect();
            }
            break;

        case SatelliteConnectionState::NEGOTIATING:
        case SatelliteConnectionState::READY:
            if (_state == SatelliteConnectionState::NEGOTIATING && now - _stateSinceMs >= kNegotiateTimeoutMs)
            {
                ESP_LOGW(TAG, "Timed out waiting for BEGIN/CAPS from Companion");
                retireClient();
                scheduleReconnect();
                break;
            }
            if (now - _lastRxMs >= _options.keepaliveTimeoutMs)
            {
                // Catches a half-open connection (Companion's host lost power, the access
                // point went away...) which TCP alone may not notice for minutes.
                ESP_LOGW(TAG, "Nothing received from Companion for %u ms, reconnecting", static_cast<unsigned>(now - _lastRxMs));
                retireClient();
                scheduleReconnect();
                break;
            }
            if (now - _lastPingMs >= kPingIntervalMs)
            {
                _lastPingMs = now;
                send("PING usat\n");
            }
            flushTx();
            break;

        case SatelliteConnectionState::IDLE:
        case SatelliteConnectionState::REJECTED:
            break;
        }

        if (_options.dispatch == SatelliteDispatchMode::LOOP && !_queue.empty() && !_loopWarningLogged &&
            now - _lastLoopMs >= kLoopWarningMs)
        {
            ESP_LOGW(TAG, "Satellite::loop() hasn't been called for %u ms - callbacks only run inside it (or use SatelliteDispatchMode::IMMEDIATE)",
                     static_cast<unsigned>(now - _lastLoopMs));
            _loopWarningLogged = true;
        }
    }

    if (_options.dispatch == SatelliteDispatchMode::IMMEDIATE)
    {
        drainQueue();
    }
}

void Satellite::onClientConnect(AsyncClient *client)
{
    {
        Lock lock(_mutex);
        if (client != _client)
        {
            // A retired client whose DNS lookup finished after we gave up on it
            client->close();
            return;
        }

        _rxBuffer.clear();
        _lastRxMs = millis();
        _lastPingMs = _lastRxMs;
        client->setNoDelay(true);
        setState(SatelliteConnectionState::NEGOTIATING);
    }

    if (_options.dispatch == SatelliteDispatchMode::IMMEDIATE)
    {
        drainQueue();
    }
}

void Satellite::onClientDisconnect(AsyncClient *client)
{
    {
        Lock lock(_mutex);
        uint32_t now = millis();

        if (client != _client)
        {
            for (RetiredClient &retired : _retiredClients)
            {
                if (retired.client == client && !retired.disposed)
                {
                    retired.disposed = true;
                    retired.disposedAt = now;
                }
            }
            return;
        }

        ESP_LOGW(TAG, "Disconnected from Companion");
        _client = nullptr;
        _retiredClients.push_back({client, now, true, now});
        resetRegistrations();
        _txBuffer.clear();

        if (_state != SatelliteConnectionState::REJECTED)
        {
            scheduleReconnect();
        }
    }

    if (_options.dispatch == SatelliteDispatchMode::IMMEDIATE)
    {
        drainQueue();
    }
}

void Satellite::onClientData(AsyncClient *client, const char *data, size_t length)
{
    {
        Lock lock(_mutex);
        if (client != _client)
        {
            return;
        }
        _lastRxMs = millis();
    }

    // _rxBuffer is only touched on the network task, so it needs no lock
    bool stopped = false;
    size_t dropped = _rxBuffer.push(data, length, [&](std::string &&line)
                                    {
                                        if (!stopped)
                                        {
                                            stopped = !handleLine(client, std::move(line));
                                        } });
    if (dropped > 0)
    {
        ESP_LOGW(TAG, "Skipped a message longer than %u bytes - increase SatelliteOptions::maxLineLength, or request smaller bitmaps",
                 static_cast<unsigned>(_rxBuffer.maxLineLength()));
    }

    if (_options.dispatch == SatelliteDispatchMode::IMMEDIATE)
    {
        drainQueue();
    }
}

/**
 * Handle one received line, on the network task.
 * @return false once the client is no longer current (eg. it was rejected), to stop processing
 */
bool Satellite::handleLine(AsyncClient *client, std::string &&line)
{
    if (line.empty())
    {
        return true;
    }

    // Answer a PING with its payload echoed back verbatim, without parsing it
    if (line.size() >= 4 && strncasecmp(line.c_str(), "PING", 4) == 0 && (line.size() == 4 || line[4] == ' '))
    {
        Lock lock(_mutex);
        if (client != _client)
        {
            return false;
        }
        send("PONG" + line.substr(4) + "\n");
        return true;
    }

    std::unique_ptr<Message> message(new (std::nothrow) Message(std::move(line)));
    if (!message)
    {
        ESP_LOGE(TAG, "Out of memory parsing a message");
        return true;
    }

    Lock lock(_mutex);
    if (client != _client)
    {
        return false;
    }

    if (message->is("PONG"))
    {
        return true; // only proves liveness, which _lastRxMs already recorded
    }
    if (message->is("BEGIN"))
    {
        handleBegin(*message);
        return client == _client;
    }
    if (message->is("CAPS"))
    {
        handleCaps(std::move(message));
        return true;
    }
    if (message->isOk() || message->isError())
    {
        handleResponse(std::move(message));
        return true;
    }
    if (message->is("ERROR"))
    {
        std::string text(message->params().get("MESSAGE").value_or("(no message)"));
        ESP_LOGE(TAG, "Error from Companion: %s", text.c_str());
        return true;
    }

    QueuedItem item;
    if (message->is("SUB-STATE"))
    {
        std::optional<std::string_view> subId = message->params().get("SUBID");
        if (!subId)
        {
            ESP_LOGW(TAG, "SUB-STATE without SUBID");
            return true;
        }
        item.kind = TargetKind::SUBSCRIPTION;
        item.target = std::string(*subId);
        item.coalesceKey = "S";
    }
    else if (std::optional<std::string_view> deviceId = message->params().get("DEVICEID"))
    {
        item.kind = TargetKind::SURFACE;
        item.target = std::string(*deviceId);
        if (message->is("KEY-STATE"))
        {
            // Only the latest state of a key matters, so a newer one replaces a queued one
            std::optional<std::string_view> control = message->params().get("CONTROLID");
            if (!control)
            {
                control = message->params().get("KEY");
            }
            item.coalesceKey = "K" + std::string(control.value_or(""));
        }
    }
    else
    {
        ESP_LOGW(TAG, "Unhandled message: %.*s", static_cast<int>(message->command().size()), message->command().data());
        return true;
    }

    item.message = std::move(message);
    enqueue(std::move(item));
    return true;
}

void Satellite::handleBegin(const Message &message)
{
    _companionVersion = toString(message.params().get("CompanionVersion").value_or("unknown"));
    _apiVersionText = toString(message.params().get("ApiVersion").value_or("unknown"));
    _apiVersion = ApiVersion::parse(_apiVersionText.c_str());

    if (!_apiVersion || !_apiVersion->atLeast(kMinimumApiVersion))
    {
        ESP_LOGE(TAG, "Companion Satellite API version '%s' is not supported (needs %u.%u.%u or later, ie. Companion 4.3+). Not reconnecting until the target changes",
                 _apiVersionText.c_str(), kMinimumApiVersion.major, kMinimumApiVersion.minor, kMinimumApiVersion.patch);
        _apiVersion.reset();
        setState(SatelliteConnectionState::REJECTED);
        retireClient();
        return;
    }

    ESP_LOGI(TAG, "Connected to Companion %s, Satellite API %s", _companionVersion.c_str(), _apiVersionText.c_str());
    // Surfaces are registered once CAPS has arrived, as the connection isn't ready until then
}

void Satellite::handleCaps(std::unique_ptr<Message> message)
{
    if (!_apiVersion)
    {
        ESP_LOGW(TAG, "CAPS received before BEGIN, ignoring");
        return;
    }

    _capsBitmapFormats.clear();
    std::string_view formats = message->params().get("BITMAP_FORMATS").value_or("");
    while (!formats.empty())
    {
        size_t comma = formats.find(',');
        std::string_view format = formats.substr(0, comma);
        if (!format.empty())
        {
            _capsBitmapFormats.push_back(toString(format));
        }
        formats = comma == std::string_view::npos ? std::string_view{} : formats.substr(comma + 1);
    }

    _caps = std::move(message);
    _reconnectDelayMs = kInitialReconnectDelayMs;

    ESP_LOGI(TAG, "Companion capabilities: subscriptions=%d nonsquare=%d rotaryAmount=%d bitmapFormats=%u",
             hasFeature(SatelliteFeature::Subscriptions), hasFeature(SatelliteFeature::NonSquareBitmaps),
             hasFeature(SatelliteFeature::RotaryAmount), static_cast<unsigned>(_capsBitmapFormats.size()));

    setState(SatelliteConnectionState::READY);
    registerAll();
}

void Satellite::registerAll()
{
    for (SatelliteSurface *surface : _surfaces)
    {
        send(surface->buildAddDeviceCommand());
    }

    if (_subscriptions.empty())
    {
        return;
    }
    if (!hasFeature(SatelliteFeature::Subscriptions))
    {
        ESP_LOGW(TAG, "Button subscriptions are disabled in Companion's settings - %u subscription(s) not registered",
                 static_cast<unsigned>(_subscriptions.size()));
        return;
    }
    for (SatelliteSubscription *subscription : _subscriptions)
    {
        sendAddSubscription(*subscription);
    }
}

/**
 * Handle a response (`<COMMAND> OK|ERROR ...`) to something we sent.
 */
void Satellite::handleResponse(std::unique_ptr<Message> message)
{
    const satellite_protocol::Params &params = message->params();
    bool ok = message->isOk();
    std::string command(message->command());
    std::string errorText(params.get("MESSAGE").value_or(""));

    if (message->is("ADD-SUB"))
    {
        // Companion's ADD-SUB errors don't carry the SUBID, but it answers in order, so the
        // response belongs to the oldest ADD-SUB still waiting for one.
        String subId;
        if (!_pendingAddSubs.empty())
        {
            subId = _pendingAddSubs.front();
            _pendingAddSubs.pop_front();
        }
        if (std::optional<std::string_view> echoed = params.get("SUBID"))
        {
            subId = toString(*echoed);
        }

        if (!ok)
        {
            ESP_LOGW(TAG, "Companion rejected subscription %s: %s", subId.c_str(), errorText.c_str());
        }

        for (SatelliteSubscription *subscription : _subscriptions)
        {
            if (subscription->_subId == subId)
            {
                subscription->_active = ok;
                QueuedItem item;
                item.kind = TargetKind::SUBSCRIPTION;
                item.target = subId.c_str();
                item.message = std::move(message);
                enqueue(std::move(item));
                break;
            }
        }
        return;
    }

    std::optional<std::string_view> deviceId = params.get("DEVICEID");
    if (!ok)
    {
        ESP_LOGW(TAG, "Companion rejected %s%s%.*s: %s", command.c_str(), deviceId ? " for " : "",
                 deviceId ? static_cast<int>(deviceId->size()) : 0, deviceId ? deviceId->data() : "", errorText.c_str());
    }

    if (!deviceId)
    {
        return;
    }

    bool isAddDevice = message->is("ADD-DEVICE");
    if (isAddDevice)
    {
        for (SatelliteSurface *surface : _surfaces)
        {
            if (std::string_view(surface->getID().c_str()) == *deviceId)
            {
                surface->_registered = ok;
            }
        }
    }

    // The surface hears about its registration, and about any errors
    if (isAddDevice || !ok)
    {
        QueuedItem item;
        item.kind = TargetKind::SURFACE;
        item.target = std::string(*deviceId);
        item.message = std::move(message);
        enqueue(std::move(item));
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool Satellite::send(std::string &&line)
{
    Lock lock(_mutex);
    if (line.empty())
    {
        return false; // a MessageBuilder that rejected its input
    }
    if (!_client || (_state != SatelliteConnectionState::NEGOTIATING && _state != SatelliteConnectionState::READY))
    {
        ESP_LOGD(TAG, "Not connected, dropping: %s", line.c_str());
        return false;
    }
    if (_txBuffer.size() + line.size() > kMaxTxBufferBytes)
    {
        ESP_LOGW(TAG, "Send buffer full, dropping: %.*s", static_cast<int>(std::min<size_t>(line.find(' '), 32)), line.c_str());
        return false;
    }

    ESP_LOGV(TAG, "Sending: %s", line.c_str());
    _txBuffer.append(line);
    flushTx();
    return true;
}

/**
 * Hand as much of the send buffer to the TCP stack as it has room for. Whatever doesn't
 * fit stays buffered (in order) until onAck or the next tick, so a long message or a
 * burst of them is never cut off part way through a line.
 */
void Satellite::flushTx()
{
    if (!_client || _txBuffer.empty())
    {
        return;
    }

    size_t queued = 0;
    while (queued < _txBuffer.size())
    {
        size_t added = _client->add(_txBuffer.data() + queued, _txBuffer.size() - queued);
        if (added == 0)
        {
            break;
        }
        queued += added;
    }

    if (queued > 0)
    {
        _client->send();
        _txBuffer.erase(0, queued);
    }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void Satellite::enqueue(QueuedItem &&item)
{
    item.bytes = sizeof(QueuedItem) + item.target.capacity() + item.coalesceKey.capacity() + (item.message ? item.message->memoryUsage() : 0);

    if (!item.coalesceKey.empty())
    {
        // Replace a queued update for the same key, unless something else for the same
        // target (eg. KEYS-CLEAR) has been queued since, which must stay in order.
        for (auto it = _queue.rbegin(); it != _queue.rend(); ++it)
        {
            if (it->kind != item.kind || it->target != item.target)
            {
                continue;
            }
            if (it->coalesceKey.empty())
            {
                break;
            }
            if (it->coalesceKey == item.coalesceKey)
            {
                _queuedBytes = _queuedBytes - it->bytes + item.bytes;
                *it = std::move(item);
                return;
            }
        }
    }

    if (item.kind != TargetKind::CONNECTION && _queuedBytes + item.bytes > _options.maxQueuedBytes)
    {
        _droppedUpdates++;
        if (_droppedUpdates == 1 || _droppedUpdates % 100 == 0)
        {
            ESP_LOGW(TAG, "Receive queue full (%u bytes), dropped %u update(s) - call loop() more often or raise SatelliteOptions::maxQueuedBytes",
                     static_cast<unsigned>(_queuedBytes), static_cast<unsigned>(_droppedUpdates));
        }
        return;
    }

    _queuedBytes += item.bytes;
    _queue.push_back(std::move(item));
}

void Satellite::purgeQueue(TargetKind kind, const String &target)
{
    for (auto it = _queue.begin(); it != _queue.end();)
    {
        if (it->kind == kind && it->target == target.c_str())
        {
            _queuedBytes -= it->bytes;
            it = _queue.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void Satellite::drainQueue()
{
    Lock dispatchLock(_dispatchMutex);

    size_t pending;
    {
        Lock lock(_mutex);
        _lastLoopMs = millis();
        _loopWarningLogged = false;
        pending = _queue.size();
    }

    // Only run what was queued on entry, so a busy connection can't keep us here forever
    while (pending-- > 0)
    {
        QueuedItem item;
        {
            Lock lock(_mutex);
            if (_queue.empty())
            {
                break;
            }
            item = std::move(_queue.front());
            _queue.pop_front();
            _queuedBytes -= item.bytes;
        }
        dispatch(item);
    }
}

void Satellite::dispatch(QueuedItem &item)
{
    switch (item.kind)
    {
    case TargetKind::CONNECTION:
        if (_onConnectionStateChange)
        {
            _onConnectionStateChange(item.state);
        }
        break;
    case TargetKind::SURFACE:
        dispatchToSurface(item.target, *item.message);
        break;
    case TargetKind::SUBSCRIPTION:
        dispatchToSubscription(item.target, *item.message);
        break;
    }
}

void Satellite::dispatchToSurface(const std::string &deviceId, const Message &message)
{
    SatelliteSurface *target = nullptr;
    {
        Lock lock(_mutex);
        for (SatelliteSurface *surface : _surfaces)
        {
            if (deviceId == surface->getID().c_str())
            {
                target = surface;
                break;
            }
        }
    }

    if (target)
    {
        target->handleMessage(message);
    }
    else
    {
        ESP_LOGD(TAG, "Message for unknown device %s", deviceId.c_str());
    }
}

void Satellite::dispatchToSubscription(const std::string &subId, const Message &message)
{
    SatelliteSubscription *target = nullptr;
    {
        Lock lock(_mutex);
        for (SatelliteSubscription *subscription : _subscriptions)
        {
            if (subId == subscription->_subId.c_str())
            {
                target = subscription;
                break;
            }
        }
    }

    if (target)
    {
        target->handleMessage(message);
    }
}

// ---------------------------------------------------------------------------
// Surfaces and subscriptions
// ---------------------------------------------------------------------------

bool Satellite::add(SatelliteSurface &surface)
{
    Lock lock(_mutex);
    for (SatelliteSurface *existing : _surfaces)
    {
        if (existing->getID() == surface.getID())
        {
            ESP_LOGE(TAG, "Surface with ID %s already exists", surface.getID().c_str());
            return false;
        }
    }

    _surfaces.push_back(&surface);
    if (_state == SatelliteConnectionState::READY)
    {
        send(surface.buildAddDeviceCommand());
    }
    return true;
}

void Satellite::remove(SatelliteSurface &surface)
{
    Lock lock(_mutex);
    if (surface._registered)
    {
        send(satellite_protocol::MessageBuilder("REMOVE-DEVICE").deviceId(surface.getID().c_str()).finish());
        surface._registered = false;
    }
    _surfaces.remove(&surface);
    purgeQueue(TargetKind::SURFACE, surface.getID());
}

void Satellite::add(SatelliteSubscription &subscription)
{
    Lock lock(_mutex);
    _subscriptions.push_back(&subscription);
    if (_state == SatelliteConnectionState::READY)
    {
        if (hasFeature(SatelliteFeature::Subscriptions))
        {
            sendAddSubscription(subscription);
        }
        else
        {
            ESP_LOGW(TAG, "Button subscriptions are disabled in Companion's settings - %s not registered", subscription._subId.c_str());
        }
    }
}

void Satellite::remove(SatelliteSubscription &subscription)
{
    Lock lock(_mutex);
    // Also remove one whose ADD-SUB hasn't been answered yet, as it may still succeed
    bool pending = std::find(_pendingAddSubs.begin(), _pendingAddSubs.end(), subscription._subId) != _pendingAddSubs.end();
    if (subscription._active || pending)
    {
        send(satellite_protocol::MessageBuilder("REMOVE-SUB").str("SUBID", subscription._subId.c_str()).finish());
        subscription._active = false;
    }
    _subscriptions.remove(&subscription);
    purgeQueue(TargetKind::SUBSCRIPTION, subscription._subId);
}

void Satellite::sendAddSubscription(SatelliteSubscription &subscription)
{
    if (send(subscription.buildAddCommand()))
    {
        _pendingAddSubs.push_back(subscription._subId);
    }
}
