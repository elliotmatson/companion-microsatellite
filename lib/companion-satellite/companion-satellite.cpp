/**
 * @file companion-satellite.cpp
 * @brief Implementation of the Companion Satellite API library
 * @author Elliot Matson
 * @version 1.0
 * @date November 2025
 */

#include "companion-satellite.hpp"

#include <algorithm>
#include <AsyncTCP.h>
#include <base64.h>
#include <MD5Builder.h>
#include "ArduinoJson.h"
#include "ESPAsyncWebServer.h"
#include "mbedtls/base64.h"
#include "mdns.h"

static bool isTokenBoundary(char c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

static bool parseApiBoolToken(const String &value)
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

static bool parseApiSemver(const String &version, uint16_t &major, uint16_t &minor, uint16_t &patch)
{
    const char *ptr = version.c_str();
    char *end = nullptr;

    unsigned long parsedMajor = strtoul(ptr, &end, 10);
    if (end == ptr || *end != '.')
    {
        return false;
    }

    ptr = end + 1;
    unsigned long parsedMinor = strtoul(ptr, &end, 10);
    if (end == ptr)
    {
        return false;
    }

    unsigned long parsedPatch = 0;
    if (*end == '.')
    {
        ptr = end + 1;
        parsedPatch = strtoul(ptr, &end, 10);
        if (end == ptr)
        {
            return false;
        }
    }

    major = static_cast<uint16_t>(parsedMajor);
    minor = static_cast<uint16_t>(parsedMinor);
    patch = static_cast<uint16_t>(parsedPatch);
    return true;
}

static bool isVersionAtLeast(uint16_t major, uint16_t minor, uint16_t patch, uint16_t minMajor, uint16_t minMinor, uint16_t minPatch)
{
    if (major != minMajor)
    {
        return major > minMajor;
    }
    if (minor != minMinor)
    {
        return minor > minMinor;
    }
    return patch >= minPatch;
}

static uint8_t clampColorComponent(long value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

/**
 * @brief Parse a KEY-STATE/SUB-STATE TYPE value, shared by surfaces and subscriptions
 */
SatelliteSurfaceKeyType parseSatelliteKeyType(const String &typeStr)
{
    if (typeStr == "PAGEUP")
    {
        return SatelliteSurfaceKeyType::PAGEUP;
    }
    if (typeStr == "PAGEDOWN")
    {
        return SatelliteSurfaceKeyType::PAGEDOWN;
    }
    if (typeStr == "PAGENUM")
    {
        return SatelliteSurfaceKeyType::PAGENUM;
    }
    return SatelliteSurfaceKeyType::BUTTON;
}

/**
 * @brief Parse a hex (#RRGGBB) or css rgb(r,g,b) color string, shared by surfaces and subscriptions
 */
std::optional<color> parseSatelliteColor(const String &value)
{
    if (value.startsWith("#") && value.length() == 7)
    {
        color c;
        c.r = strtoul(value.substring(1, 3).c_str(), nullptr, 16);
        c.g = strtoul(value.substring(3, 5).c_str(), nullptr, 16);
        c.b = strtoul(value.substring(5, 7).c_str(), nullptr, 16);
        return c;
    }

    if (value.startsWith("rgb(") && value.endsWith(")"))
    {
        int firstComma = value.indexOf(',');
        int secondComma = value.indexOf(',', firstComma + 1);
        if (firstComma != -1 && secondComma != -1)
        {
            color c;
            c.r = clampColorComponent(value.substring(4, firstComma).toInt());
            c.g = clampColorComponent(value.substring(firstComma + 1, secondComma).toInt());
            c.b = clampColorComponent(value.substring(secondComma + 1, value.length() - 1).toInt());
            return c;
        }
    }

    return std::nullopt;
}

ESP_EVENT_DEFINE_BASE(SATELLITE_EVENTS);

/**
 * @brief Convert satellite event ID to human-readable text
 *
 * Utility function to convert event IDs to readable strings for debugging
 * and logging purposes.
 */
const char *satelliteEventToText(int32_t event_id)
{
    switch (event_id)
    {
    case SATELLITE_EVENT_READY:
        return "READY";
    case SATELLITE_EVENT_CONNECTED:
        return "CONNECTED";
    case SATELLITE_EVENT_PING_SENT:
        return "PING_SENT";
    case SATELLITE_EVENT_PONG_RECEIVED:
        return "PONG_RECEIVED";
    case SATELLITE_EVENT_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN_EVENT";
    }
}

/**
 * @brief Constructor for Satellite
 *
 * Initializes the device with a unique name based on the chip MAC address
 * and creates the web server for configuration.
 */
Satellite::Satellite(const char *deviceName, uint16_t apiPort)
{
    _apiPort = apiPort;
    _server = new AsyncWebServer(apiPort);
    // get chip ID using ESP.getEfuseMac();
    char chipId[18];
    snprintf(chipId, sizeof(chipId), "%llx", ESP.getEfuseMac());
    _deviceName = String(deviceName) + " (" + String(chipId) + ")";

    _hostname = String(DEFAULT_HOSTNAME) + "-" + String(chipId);
}

/**
 * @brief Destructor for Satellite
 *
 * Disconnects from Companion, stops mDNS, and cleans up the web server.
 */
Satellite::~Satellite()
{
    disconnectFromCompanion();

    mdns_free();
    _server->end();
    delete _server;
    _server = nullptr;
}

/**
 * @brief Initialize the satellite
 *
 * Sets up preferences, web server endpoints, mDNS service, and attempts
 * to connect to Companion using stored configuration.
 */
void Satellite::begin()
{
    // Initialize default event loop (likely already initialized by Arduino core, but just in case)
    esp_err_t eventLoopErr = esp_event_loop_create_default();
    if (eventLoopErr != ESP_OK && eventLoopErr != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE("Satellite", "Failed to create default event loop: %d", eventLoopErr);
    }
    // Initialize preferences
    // md5 hash deviceName to get a unique namespace
    MD5Builder md5;
    md5.begin();
    md5.add(_deviceName);
    md5.calculate();
    String prefsNamespace = "usat" + md5.toString().substring(0, 8);

    _prefs.begin(prefsNamespace.c_str(), false);
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _targetHostname = _prefs.getString("host", "");
    _targetPort = _prefs.getUInt("port", 0);
    xSemaphoreGiveRecursive(_mutex);

    // Setup http API
    _server->on("/api/host", HTTP_GET, [&](AsyncWebServerRequest *request)
                { request->send(200, "text/plain", getTargetHostname()); });
    _server->on("/api/port", HTTP_GET, [&](AsyncWebServerRequest *request)
                { request->send(200, "text/plain", String(getTargetPort())); });
    _server->on("/api/config", HTTP_GET, [&](AsyncWebServerRequest *request)
                {
        String json = "{\"host\": \"" + getTargetHostname() + "\", \"port\": " + String(getTargetPort()) + "}";
        request->send(200, "application/json", json); });

    _server->onRequestBody([&](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
                           {
        if (request->method() != HTTP_POST)
        {
            return;
        }

        if (index == 0)
        {
            String *body = new String();
            body->reserve(total);
            request->_tempObject = body;
        }

        String *body = static_cast<String *>(request->_tempObject);
        if (!body)
        {
            request->send(500, "text/plain", "Internal Error");
            return;
        }

        body->concat(reinterpret_cast<const char *>(data), len);
        if (index + len != total)
        {
            return;
        }

        String payload = *body;
        delete body;
        request->_tempObject = nullptr;

        if (request->url() == "/api/host")
        {
            if (request->contentType() == "application/json")
            {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, payload);
                if (!error && doc["host"].is<String>())
                {
                    setTargetHostname(doc["host"].as<String>());
                }
                else
                {
                    request->send(400, "text/plain", "Invalid JSON");
                    return;
                }
            }
            else
            {
                setTargetHostname(payload);
            }
            request->send(200, "text/plain", "OK");
        }
        else if (request->url() == "/api/port")
        {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (!error && doc["port"].is<uint16_t>())
            {
                setTargetPort(doc["port"].as<uint16_t>());
            }
            else
            {
                // Assume plain text
                setTargetPort(payload.toInt());
            }
            request->send(200, "text/plain", "OK");
        }
        else if (request->url() == "/api/config")
        {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (!error)
            {
                if (doc["host"].is<String>())
                {
                    setTargetHostname(doc["host"].as<String>());
                }
                if (doc["port"].is<uint16_t>())
                {
                    setTargetPort(doc["port"].as<uint16_t>());
                }
                request->send(200, "text/plain", "OK");
            }
            else
            {
                request->send(400, "text/plain", "Invalid JSON");
            }
        }
        else
        {
            request->send(404, "text/plain", "Not Found");
        } });

    _server->begin();

    // Setup mDNS
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE("mdns", "MDNS Init failed: %d", err);
    }

    // set hostname
    mdns_hostname_set(_hostname.c_str());
    // set default instance
    mdns_instance_name_set(_deviceName.c_str());

    mdns_txt_item_t serviceTxtData[1] = {
        {"restEnabled", "true"}};
    mdns_service_add(NULL, "_companion-satellite", "_tcp", _apiPort, serviceTxtData, 1);

    Satellite *eventSource = this;
    esp_event_post(SATELLITE_EVENTS, SATELLITE_EVENT_READY, &eventSource, sizeof(eventSource), portMAX_DELAY);

    // connect to companion
    connectToCompanion();
}

/**
 * @brief Disconnect from Companion server
 *
 * Safely closes the TCP connection, sends QUIT command, and stops timers.
 */
void Satellite::disconnectFromCompanion()
{
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (_client)
    {
        if (_client->connected())
        {
            ESP_LOGI("Satellite", "Connected to companion, disconnecting");
            _client->write("QUIT\n");
            _client->close();
        }
        delete _client;
        _client = nullptr;
    }
    xSemaphoreGiveRecursive(_mutex);

    if (_keepaliveTicker.active())
    {
        _keepaliveTicker.detach();
    }
}

/**
 * @brief Arm the reconnect timer, unless reconnection is permanently disabled
 *
 * Shared by the onDisconnect handler and by connectToCompanion() itself, for
 * when the initial (synchronous) AsyncClient::connect() call fails and no
 * onDisconnect callback will ever fire for that attempt.
 */
void Satellite::scheduleReconnect()
{
    if (lockedRead(_apiVersionRejected))
    {
        ESP_LOGE("Satellite", "Reconnect disabled until host/port changes because server API is below 1.10.0");
        return;
    }

    // try to reconnect every 5s using _reconnectTicker
    _reconnectTicker.attach(5, [&]()
                            {
                                ESP_LOGI("Satellite", "Reconnecting to companion...");
                                connectToCompanion(); });
}

/**
 * @brief Connect to Companion server
 *
 * Creates a new TCP connection to the configured Companion server.
 * Sets up event handlers for connection, disconnection, data reception,
 * and error handling. Automatically registers all surfaces upon connection.
 */
void Satellite::connectToCompanion()
{
    if (lockedRead(_apiVersionRejected))
    {
        ESP_LOGE("Satellite", "Connection blocked: remote API version is below required 1.10.0");
        return;
    }

    disconnectFromCompanion();

    String targetHostname = getTargetHostname();
    uint16_t targetPort = getTargetPort();
    if (targetHostname.length() == 0 || targetPort == 0)
    {
        ESP_LOGE("Satellite", "Target hostname or port not set");
        return;
    }

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _client = new AsyncClient();
    if (!_client)
    {
        ESP_LOGE("Satellite", "Failed to create client");
        xSemaphoreGiveRecursive(_mutex);
        return;
    }

    _client->onConnect([&](void *arg, AsyncClient *client)
                       {
                           ESP_LOGI("Satellite", "Connected to companion");
                           xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
                           _isConnected = true;
                           _apiNegotiated = false;
                           _satelliteApiVersionValid = false;
                           _capsSubscriptions = false;
                           _capsNonsquare = false;
                           _capsBitmapFormats.clear();
                           xSemaphoreGiveRecursive(_mutex);
                           Satellite *eventSource = this;
                           esp_event_post(SATELLITE_EVENTS, SATELLITE_EVENT_CONNECTED, &eventSource, sizeof(eventSource), portMAX_DELAY);
                           // Cancel any existing reconnect attempts
                           _reconnectTicker.detach();
                           // Start the keepalive timer
                           _keepaliveTicker.attach(2.0, [&]()
                                                   {
                                                    // Send keepalive message
                                                    send("PING usat-" + String(millis()) + "\n");
                                                    Satellite *eventSource = this;
                                                    esp_event_post(SATELLITE_EVENTS, SATELLITE_EVENT_PING_SENT, &eventSource, sizeof(eventSource), portMAX_DELAY); });
                           // Surfaces are registered after BEGIN once ApiVersion is validated.
                       });
    _client->onDisconnect([&](void *arg, AsyncClient *client)
                          {
                            ESP_LOGW("Satellite", "Disconnected from companion");
                            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
                            _isConnected = false;
                            _apiNegotiated = false;
                            xSemaphoreGiveRecursive(_mutex);
                            Satellite *eventSource = this;
                            esp_event_post(SATELLITE_EVENTS, SATELLITE_EVENT_DISCONNECTED, &eventSource, sizeof(eventSource), portMAX_DELAY);
                            // Stop the keepalive timer
                            _keepaliveTicker.detach();
                            scheduleReconnect(); });
    _client->onError([](void *arg, AsyncClient *client, int8_t error)
                     { ESP_LOGE("Satellite", "Connection error: %d", error); });
    _client->onData([&](void *arg, AsyncClient *client, void *data, size_t len)
                    { receiveCallback(arg, client, data, len); });

    if (!_client->connect(targetHostname.c_str(), targetPort))
    {
        ESP_LOGE("Satellite", "Failed to connect to %s:%d", targetHostname.c_str(), targetPort);
        delete _client;
        _client = nullptr;
        xSemaphoreGiveRecursive(_mutex);
        // connect() failing synchronously never triggers onDisconnect, so we
        // must arm the retry ourselves or the satellite would be stuck.
        scheduleReconnect();
        return;
    }
    xSemaphoreGiveRecursive(_mutex);
}

/**
 * @brief Send a message to Companion
 *
 * Thread-safe method to send data over the TCP connection.
 * Only sends if client is connected.
 */
void Satellite::send(const String &message)
{
    ESP_LOGD("Satellite", "Sending: %s", message.c_str());
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    if (_client && _client->connected())
    {
        _client->write(message.c_str(), message.length());
    }
    else
    {
        ESP_LOGW("Satellite", "Not connected to companion, cannot send message");
    }
    xSemaphoreGiveRecursive(_mutex);
}

/**
 * @brief Add a button subscription with simple style flags (API >= 1.10.0)
 *
 * Validates required fields and protocol-safe string inputs before sending.
 * This implementation intentionally supports only simple style parameters,
 * and does not send advanced STYLE payloads.
 */
void Satellite::addSubscription(const String &subId, const String &location, std::optional<uint16_t> bitmapSize,
                                std::optional<String> bitmapFormat,
                                std::optional<bool> colors,
                                std::optional<bool> text,
                                std::optional<bool> textStyle)
{
    if (!lockedRead(_apiNegotiated))
    {
        ESP_LOGW("Satellite", "Cannot ADD-SUB before API negotiation is complete");
        return;
    }

    if (!isSafeApiTokenValue(subId))
    {
        ESP_LOGW("Satellite", "Invalid ADD-SUB SUBID (must be non-empty and must not contain quotes/newlines)");
        return;
    }

    if (!isSafeApiTokenValue(location))
    {
        ESP_LOGW("Satellite", "Invalid ADD-SUB LOCATION (must be non-empty and must not contain quotes/newlines)");
        return;
    }

    String command;
    command.reserve(64 + subId.length() + location.length());
    command = "ADD-SUB SUBID=\"" + subId + "\" LOCATION=\"" + location + "\"";

    if (bitmapSize.has_value())
    {
        // Per the API, 0 explicitly means "do not stream bitmaps" - any other
        // value is the requested square pixel size, passed through as-is.
        command += " BITMAP=" + String(bitmapSize.value());
    }

    if (bitmapFormat.has_value())
    {
        appendBitmapFormatParam(command, "ADD-SUB", bitmapFormat.value());
    }

    if (colors.has_value())
    {
        command += " COLORS=";
        command += colors.value() ? "1" : "0";
    }

    if (text.has_value())
    {
        command += " TEXT=";
        command += text.value() ? "1" : "0";
    }

    if (textStyle.has_value())
    {
        command += " TEXT_STYLE=";
        command += textStyle.value() ? "1" : "0";
    }

    command += "\n";
    send(command);
}

/**
 * @brief Remove a button subscription by id (API >= 1.10.0)
 */
void Satellite::removeSubscription(const String &subId)
{
    if (!lockedRead(_apiNegotiated))
    {
        ESP_LOGW("Satellite", "Cannot REMOVE-SUB before API negotiation is complete");
        return;
    }

    if (!isSafeApiTokenValue(subId))
    {
        ESP_LOGW("Satellite", "Invalid REMOVE-SUB SUBID");
        return;
    }

    send("REMOVE-SUB SUBID=\"" + subId + "\"\n");
}

/**
 * @brief Send a subscribed control press/release or rotate event (API >= 1.10.0)
 *
 * Mirrors SatelliteSurface::buttonAction: PRESSED/RELEASED dispatch to
 * SUB-PRESS, RIGHT/LEFT dispatch to SUB-ROTATE.
 */
void Satellite::subscriptionAction(const String &subId, SatelliteAction action)
{
    if (!lockedRead(_apiNegotiated))
    {
        ESP_LOGW("Satellite", "Cannot send subscription action before API negotiation is complete");
        return;
    }

    if (!isSafeApiTokenValue(subId))
    {
        ESP_LOGW("Satellite", "Invalid subscription action SUBID");
        return;
    }

    switch (action)
    {
    case SatelliteAction::PRESSED:
        send("SUB-PRESS SUBID=\"" + subId + "\" PRESSED=1\n");
        break;
    case SatelliteAction::RELEASED:
        send("SUB-PRESS SUBID=\"" + subId + "\" PRESSED=0\n");
        break;
    case SatelliteAction::RIGHT:
        send("SUB-ROTATE SUBID=\"" + subId + "\" DIRECTION=1\n");
        break;
    case SatelliteAction::LEFT:
        send("SUB-ROTATE SUBID=\"" + subId + "\" DIRECTION=-1\n");
        break;
    default:
        ESP_LOGE("Satellite", "Invalid subscription action: %d", static_cast<int>(action));
        return;
    }
}

/**
 * @brief Callback for receiving data from Companion
 *
 * Processes incoming messages from Companion and handles various command types:
 * - PING: Responds with PONG for keepalive
 * - PONG: Posts SATELLITE_EVENT_PONG_RECEIVED event
 * - BEGIN: Validates ApiVersion and registers queued surfaces
 * - CAPS: Records server capabilities (subscriptions, non-square, bitmap formats)
 * - ERROR: Logs error messages from Companion
 * - SUB-STATE: Builds a SubscriptionState and invokes onSubscriptionState
 * - Device-specific messages: Routes messages containing DEVICEID to appropriate surfaces
 *
 * Messages are processed line by line, with timing measurements for performance monitoring.
 */
void Satellite::receiveCallback(void *arg, AsyncClient *client, void *data, size_t len)
{
    if (len == 0)
    {
        return;
    }

    if (_rxBuffer.length() + len > MAX_RX_BUFFER_SIZE)
    {
        ESP_LOGW("Satellite", "RX buffer exceeded %u bytes, dropping buffered data", static_cast<unsigned>(MAX_RX_BUFFER_SIZE));
        _rxBuffer = "";
    }

    // Buffer TCP chunks and process complete lines only.
    _rxBuffer.concat(static_cast<const char *>(data), len);

    int end = _rxBuffer.indexOf('\n');
    while (end != -1)
    {
        String received = _rxBuffer.substring(0, end);
        _rxBuffer.remove(0, end + 1);
        if (received.endsWith("\r"))
        {
            received.remove(received.length() - 1);
        }

        unsigned long startTime = micros();
        ESP_LOGD("Satellite", "Received data: %s", received.c_str());
        if (received.startsWith("PING"))
        {
            String payload = "";
            if (received.length() > 5 && received.charAt(4) == ' ')
            {
                payload = received.substring(5);
            }

            if (payload.length() > 0)
            {
                send("PONG " + payload + "\n");
            }
            else
            {
                send("PONG\n");
            }
        }
        else if (received.startsWith("PONG"))
        {
            Satellite *eventSource = this;
            esp_event_post(SATELLITE_EVENTS, SATELLITE_EVENT_PONG_RECEIVED, &eventSource, sizeof(eventSource), portMAX_DELAY);
        }
        else if (received.startsWith("BEGIN"))
        {
            std::vector<ApiParam> tokens = tokenizeApiParams(received);

            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

            ApiParam versionParam = findApiParam(tokens, "CompanionVersion");
            if (versionParam.val.has_value())
            {
                _companionVersion = versionParam.val.value();
            }

            ApiParam apiVersionParam = findApiParam(tokens, "ApiVersion");
            if (apiVersionParam.val.has_value())
            {
                _satelliteApiVersion = apiVersionParam.val.value();
                _satelliteApiVersionValid = parseApiSemver(_satelliteApiVersion, _satelliteApiMajor, _satelliteApiMinor, _satelliteApiPatch);
            }

            if (!_satelliteApiVersionValid)
            {
                ESP_LOGE("Satellite", "Invalid or missing ApiVersion from Companion BEGIN: '%s'", _satelliteApiVersion.c_str());
                _apiVersionRejected = true;
                xSemaphoreGiveRecursive(_mutex);
                disconnectFromCompanion();
                return;
            }

            if (!isVersionAtLeast(_satelliteApiMajor, _satelliteApiMinor, _satelliteApiPatch, 1, 10, 0))
            {
                ESP_LOGE("Satellite", "Companion Satellite ApiVersion %s is below minimum supported 1.10.0", _satelliteApiVersion.c_str());
                _apiVersionRejected = true;
                xSemaphoreGiveRecursive(_mutex);
                disconnectFromCompanion();
                return;
            }

            _apiVersionRejected = false;
            _apiNegotiated = true;

            ESP_LOGI("Satellite", "Connected to Companion version %s, Satellite API version %s", _companionVersion.c_str(), _satelliteApiVersion.c_str());

            // Register all known surfaces only after we validate protocol compatibility.
            for (const auto &surface : _surfaces)
            {
                sendAddCommand(*surface);
            }

            xSemaphoreGiveRecursive(_mutex);
        }
        else if (received.startsWith("CAPS"))
        {
            ESP_LOGI("Satellite", "Server capabilities: %s", received.c_str());

            std::vector<ApiParam> tokens = tokenizeApiParams(received);

            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

            ApiParam subscriptionsParam = findApiParam(tokens, "SUBSCRIPTIONS");
            if (subscriptionsParam.val.has_value())
            {
                _capsSubscriptions = parseApiBoolToken(subscriptionsParam.val.value());
            }

            ApiParam nonsquareParam = findApiParam(tokens, "NONSQUARE");
            if (nonsquareParam.val.has_value())
            {
                _capsNonsquare = parseApiBoolToken(nonsquareParam.val.value());
            }

            ApiParam bitmapFormatsParam = findApiParam(tokens, "BITMAP_FORMATS");
            _capsBitmapFormats.clear();
            if (bitmapFormatsParam.val.has_value())
            {
                String formats = bitmapFormatsParam.val.value();
                int start = 0;
                while (start <= formats.length())
                {
                    int comma = formats.indexOf(',', start);
                    String token = comma == -1 ? formats.substring(start) : formats.substring(start, comma);
                    token.trim();
                    if (token.length() > 0)
                    {
                        _capsBitmapFormats.push_back(token);
                    }

                    if (comma == -1)
                    {
                        break;
                    }
                    start = comma + 1;
                }
            }

            xSemaphoreGiveRecursive(_mutex);
        }
        else if (received.startsWith("ERROR"))
        {
            String errorMsg = received.substring(received.indexOf("MESSAGE=") + 8);
            errorMsg.replace("\"", "");
            ESP_LOGE("Satellite", "Error from Companion: %s", errorMsg.c_str());
        }
        else if (received.indexOf(" ERROR ") != -1)
        {
            ESP_LOGE("Satellite", "Command error from Companion: %s", received.c_str());
        }
        else if (received.startsWith("SUB-STATE"))
        {
            std::vector<ApiParam> tokens = tokenizeApiParams(received);
            ApiParam subIdParam = findApiParam(tokens, "SUBID");
            if (!subIdParam.val.has_value())
            {
                ESP_LOGW("Satellite", "SUB-STATE missing SUBID: %s", received.c_str());
            }
            else
            {
                SubscriptionState state;
                state.subId = subIdParam.val.value();

                ApiParam typeParam = findApiParam(tokens, "TYPE");
                state.type = parseSatelliteKeyType(typeParam.val.value_or("BUTTON"));

                ApiParam locationParam = findApiParam(tokens, "LOCATION");
                if (locationParam.val.has_value())
                {
                    state.location = locationParam.val.value();
                }

                ApiParam bitmapParam = findApiParam(tokens, "BITMAP");
                if (bitmapParam.val.has_value())
                {
                    state.bitmap = bitmapParam.val.value();
                }

                ApiParam colorParam = findApiParam(tokens, "COLOR");
                if (colorParam.val.has_value())
                {
                    state.keyColor = parseSatelliteColor(colorParam.val.value());
                }

                ApiParam textColorParam = findApiParam(tokens, "TEXTCOLOR");
                if (textColorParam.val.has_value())
                {
                    state.textColor = parseSatelliteColor(textColorParam.val.value());
                }

                ApiParam textParam = findApiParam(tokens, "TEXT", true);
                if (textParam.val.has_value())
                {
                    state.text = textParam.val.value();
                }

                ApiParam fontSizeParam = findApiParam(tokens, "FONT_SIZE");
                if (fontSizeParam.val.has_value())
                {
                    state.fontSize = static_cast<uint8_t>(fontSizeParam.val.value().toInt());
                }

                ApiParam pressedParam = findApiParam(tokens, "PRESSED");
                if (pressedParam.val.has_value())
                {
                    state.pressed = parseApiBoolToken(pressedParam.val.value());
                }

                if (onSubscriptionState)
                {
                    onSubscriptionState(state);
                }
            }
        }
        else if (received.indexOf("DEVICEID=") != -1)
        {
            // Likely a device-specific message, like "KEY-STATE DEVICEID=00000 KEY=0 BITMAP=abcabcabc COLOR=#00ff00 TEXT="abcabcabc""
            // get device ID from message
            ESP_LOGD("Satellite", "Device-specific message: %s", received.c_str());
            int deviceIdStart = received.indexOf("DEVICEID=") + 9;
            int deviceIdEnd = received.indexOf(' ', deviceIdStart);
            if (deviceIdEnd == -1)
            {
                deviceIdEnd = received.length();
            }
            String deviceID = received.substring(deviceIdStart, deviceIdEnd);
            deviceID.replace("\"", "");
            // find surface with this ID
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
            for (const auto &surface : _surfaces)
            {
                if (surface->getID() == deviceID)
                {
                    surface->handleMessage(received);
                    break;
                }
            }
            xSemaphoreGiveRecursive(_mutex);
        }
        else
        {
            ESP_LOGW("Satellite", "Unknown message received: %s", received.c_str());
        }
        end = _rxBuffer.indexOf('\n');
        unsigned long endTime = micros();
        ESP_LOGD("Satellite", "Processed message in %lu microseconds", endTime - startTime);
    }
}

/**
 * @brief Send an ADD-DEVICE command for a surface
 *
 * Constructs and sends a complete ADD-DEVICE command to Companion including
 * all surface configuration parameters, capabilities, and variable definitions.
 * The command includes surface layout, feature support flags, pincode support
 * level, and a base64-encoded JSON array of associated variables.
 */
void Satellite::sendAddCommand(SatelliteSurface &surface)
{
    ESP_LOGI("Satellite", "Adding surface: %s", surface.getID().c_str());
    const SatelliteSurfaceConfig &config = surface.getConfig();
    const std::vector<SatelliteSurfaceVariable> &variables = surface.getVariables();

    String addDeviceCommand;
    addDeviceCommand.reserve(256 + surface.getID().length() + surface.getName().length());
    addDeviceCommand = "ADD-DEVICE DEVICEID=" + surface.getID() +
                       " PRODUCT_NAME=\"" + surface.getName() +
                       "\" KEYS_TOTAL=" + String(config.totalKeys) +
                       " KEYS_PER_ROW=" + String(config.keysPerRow) +
                       " BITMAPS=" + (config.sendBitmaps ? "true" : "false") +
                       " COLORS=" + (config.sendColors ? "true" : "false") +
                       " TEXT=" + (config.sendText ? "true" : "false") +
                       " TEXT_STYLE=" + String(config.sendTextStyle ? "true" : "false") +
                       " BRIGHTNESS=" + String(config.supportsBrightness ? "true" : "false");

    if (config.sendBitmaps && config.bitmapFormat.has_value())
    {
        appendBitmapFormatParam(addDeviceCommand, "ADD-DEVICE", config.bitmapFormat.value());
    }

    // CAN_CHANGE_PAGE is a label string for the checkbox Companion shows in the
    // surface settings panel - its mere presence enables the feature, so only
    // send it when a real label was provided (see SatelliteSurfaceConfig::canChangePageLabel).
    if (config.canChangePageLabel.has_value() && config.canChangePageLabel->length() > 0)
    {
        if (isSafeApiTokenValue(config.canChangePageLabel.value()))
        {
            addDeviceCommand += " CAN_CHANGE_PAGE=\"" + config.canChangePageLabel.value() + "\"";
        }
        else
        {
            ESP_LOGW("Satellite", "Invalid CAN_CHANGE_PAGE label (must not contain quotes/newlines), omitting field");
        }
    }

    // Pincode support
    switch (config.pincodeSupport)
    {
    case SatelliteSurfacePincodeSupport::PARTIAL:
        addDeviceCommand += " PINCODE_LOCK=PARTIAL";
        break;
    case SatelliteSurfacePincodeSupport::FULL:
        addDeviceCommand += " PINCODE_LOCK=FULL";
        break;
    default:
        break;
    }

    // Add variables. This should be a base64 encoded json array describing any input or output variables
    // test if there are variables
    if (!variables.empty())
    {
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();
        for (const auto &var : variables)
        {
            JsonObject varObj = array.add<JsonObject>();
            varObj["id"] = var.id;
            varObj["type"] = (var.type == SatelliteSurfaceVariableType::VARIABLE_INPUT) ? "input" : "output";
            varObj["name"] = var.name;
            varObj["description"] = var.description;
        }
        String jsonString;
        jsonString.reserve(measureJson(doc));
        serializeJson(doc, jsonString);
        addDeviceCommand += " VARIABLES=" + base64::encode(jsonString);
    }

    if (config.configFieldsJson.has_value() && config.configFieldsJson->length() > 0)
    {
        JsonDocument configFieldsDoc;
        DeserializationError cfgErr = deserializeJson(configFieldsDoc, config.configFieldsJson.value());
        if (cfgErr)
        {
            ESP_LOGW("Satellite", "CONFIG_FIELDS is not valid JSON, omitting field: %s", cfgErr.c_str());
        }
        else if (!configFieldsDoc.is<JsonArray>())
        {
            ESP_LOGW("Satellite", "CONFIG_FIELDS must be a JSON array, omitting field");
        }
        else
        {
            addDeviceCommand += " CONFIG_FIELDS=\"" + base64::encode(config.configFieldsJson.value()) + "\"";
        }
    }

    addDeviceCommand += "\n";
    send(addDeviceCommand);
}

/**
 * @brief Add a surface to this satellite
 *
 * Registers a surface with this satellite and sends ADD-DEVICE command
 * to Companion if connected. Prevents duplicate surface IDs.
 *
 * @return True if successfully added, false if ID already exists
 */
bool Satellite::add(SatelliteSurface &surface)
{
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    // check if ID exists
    for (const auto &existingSurface : _surfaces)
    {
        if (existingSurface->getID() == surface.getID())
        {
            ESP_LOGE("Satellite", "Surface with ID %s already exists", surface.getID().c_str());
            xSemaphoreGiveRecursive(_mutex);
            return false;
        }
    }

    _surfaces.push_back(&surface);
    bool readyForAdd = _client && _client->connected() && _apiNegotiated;

    xSemaphoreGiveRecursive(_mutex);

    if (readyForAdd)
    {
        sendAddCommand(surface);
    }
    else
    {
        ESP_LOGW("Satellite", "Surface queued until Companion connection and API negotiation complete");
    }

    return true;
}

/**
 * @brief Remove a surface from this satellite
 *
 * Sends REMOVE-DEVICE command to Companion and removes the surface
 * from the internal list.
 */
void Satellite::remove(SatelliteSurface &surface)
{
    send("REMOVE-DEVICE DEVICEID=" + surface.getID() + "\n");
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _surfaces.remove(&surface);
    xSemaphoreGiveRecursive(_mutex);
}

/**
 * @brief Append a BITMAP_FORMAT="..." parameter if valid and advertised by CAPS
 *
 * Shared by ADD-DEVICE (sendAddCommand) and ADD-SUB (addSubscription). Omits
 * the parameter (rather than aborting the whole command) when the value is
 * unsafe or wasn't advertised, matching the protocol's documented fallback to
 * "rgb" for an omitted/unrecognized BITMAP_FORMAT.
 */
void Satellite::appendBitmapFormatParam(String &command, const char *commandName, const String &requestedFormat) const
{
    if (!isSafeApiTokenValue(requestedFormat))
    {
        ESP_LOGW("Satellite", "Invalid %s BITMAP_FORMAT value, omitting parameter", commandName);
        return;
    }

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    bool formatAdvertised = _capsBitmapFormats.empty() ||
                            std::find(_capsBitmapFormats.begin(), _capsBitmapFormats.end(), requestedFormat) != _capsBitmapFormats.end();
    xSemaphoreGiveRecursive(_mutex);

    if (!formatAdvertised)
    {
        ESP_LOGW("Satellite", "%s BITMAP_FORMAT '%s' not advertised by server CAPS, omitting parameter", commandName, requestedFormat.c_str());
        return;
    }

    command += " BITMAP_FORMAT=\"" + requestedFormat + "\"";
}

/**
 * @brief Parse all KEY=VALUE tokens out of a Satellite API message line in a single pass
 *
 * Replaces repeatedly re-scanning the (potentially large, eg. bitmap-carrying)
 * message string once per field: callers tokenize a line once and then look
 * up as many fields as they need via findApiParam.
 */
std::vector<Satellite::ApiParam> Satellite::tokenizeApiParams(const String &data) const
{
    std::vector<ApiParam> tokens;
    const int n = data.length();
    int pos = 0;

    while (pos < n)
    {
        while (pos < n && isTokenBoundary(data.charAt(pos)))
        {
            pos++;
        }
        if (pos >= n)
        {
            break;
        }

        int idStart = pos;
        while (pos < n && data.charAt(pos) != '=' && !isTokenBoundary(data.charAt(pos)))
        {
            pos++;
        }

        if (pos >= n || data.charAt(pos) != '=')
        {
            // Bare word with no '=' (eg. the leading command name) - skip it.
            while (pos < n && !isTokenBoundary(data.charAt(pos)))
            {
                pos++;
            }
            continue;
        }

        String id = data.substring(idStart, pos);
        pos++; // skip '='

        String value;
        if (pos < n && data.charAt(pos) == '"')
        {
            pos++;
            int valueStart = pos;
            while (pos < n && data.charAt(pos) != '"')
            {
                pos++;
            }
            value = data.substring(valueStart, pos);
            if (pos < n)
            {
                pos++; // skip closing quote
            }
        }
        else
        {
            int valueStart = pos;
            while (pos < n && !isTokenBoundary(data.charAt(pos)))
            {
                pos++;
            }
            value = data.substring(valueStart, pos);
        }

        ApiParam param;
        param.id = id;
        param.val = value;
        tokens.push_back(param);
    }

    return tokens;
}

/**
 * @brief Look up a single field by name within a previously tokenized message
 *
 * Supports optional base64 decoding of just the matched value.
 */
Satellite::ApiParam Satellite::findApiParam(const std::vector<ApiParam> &tokens, const char *id, bool decodeBase64) const
{
    ApiParam result;
    for (const auto &token : tokens)
    {
        if (token.id.has_value() && token.id.value() == id)
        {
            result = token;
            break;
        }
    }

    if (decodeBase64 && result.val.has_value())
    {
        const String &value = result.val.value();
        size_t outputLen = 0;
        int decodeSizeResult = mbedtls_base64_decode(nullptr, 0, &outputLen, (const unsigned char *)value.c_str(), value.length());
        if ((decodeSizeResult == 0 || decodeSizeResult == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) && outputLen > 0)
        {
            uint8_t *decodedBuffer = new uint8_t[outputLen + 1];
            size_t decodedLength = 0;
            int decodeResult = mbedtls_base64_decode(decodedBuffer, outputLen, &decodedLength, (const unsigned char *)value.c_str(), value.length());
            if (decodeResult == 0)
            {
                decodedBuffer[decodedLength] = '\0';
                result.val = String((char *)decodedBuffer, decodedLength);
            }
            delete[] decodedBuffer;
        }
    }

    return result;
}

/**
 * @brief Compare negotiated ApiVersion to a minimum version requirement
 */
bool Satellite::isApiVersionAtLeast(uint16_t major, uint16_t minor, uint16_t patch) const
{
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    bool valid = _satelliteApiVersionValid;
    uint16_t apiMajor = _satelliteApiMajor;
    uint16_t apiMinor = _satelliteApiMinor;
    uint16_t apiPatch = _satelliteApiPatch;
    xSemaphoreGiveRecursive(_mutex);

    if (!valid)
    {
        return false;
    }

    return isVersionAtLeast(apiMajor, apiMinor, apiPatch, major, minor, patch);
}
