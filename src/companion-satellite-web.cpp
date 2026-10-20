/**
 * @file companion-satellite-web.cpp
 * @brief REST config API and mDNS advertisement for Satellite
 *
 * The REST API matches the one Companion's surface discovery uses to point a satellite
 * at itself (a JSON POST of {"host", "port"} to /api/config). Both it and mDNS are
 * optional, see SatelliteOptions.
 */

#include "companion-satellite.hpp"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <mdns.h>

[[maybe_unused]] static const char *TAG = "Satellite";

/** @brief Largest request body accepted by the config API */
static constexpr size_t kMaxBodyBytes = 512;

static String configJson(const String &host, uint16_t port)
{
    JsonDocument doc;
    doc["host"] = host;
    doc["port"] = port;
    String json;
    serializeJson(doc, json);
    return json;
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

void Satellite::beginWebConfig()
{
    _server = new AsyncWebServer(_apiPort);

    _server->on("/api/host", HTTP_GET, [this](AsyncWebServerRequest *request)
                { request->send(200, "text/plain", getTargetHostname()); });
    _server->on("/api/port", HTTP_GET, [this](AsyncWebServerRequest *request)
                { request->send(200, "text/plain", String(getTargetPort())); });
    _server->on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request)
                { request->send(200, "application/json", configJson(getTargetHostname(), getTargetPort())); });

    _server->onRequestBody([this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
                           {
        if (request->method() != HTTP_POST)
        {
            return;
        }
        if (total > kMaxBodyBytes)
        {
            if (index == 0)
            {
                request->send(413, "text/plain", "Request too large");
            }
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
        payload.trim();

        JsonDocument doc;
        bool isJson = deserializeJson(doc, payload) == DeserializationError::Ok && doc.is<JsonObject>();
        String url = request->url();

        if (url == "/api/host")
        {
            String host = isJson ? doc["host"].as<String>() : payload;
            if (host.length() == 0 || host == "null")
            {
                request->send(400, "text/plain", "Missing host");
                return;
            }
            setTargetHostname(host);
        }
        else if (url == "/api/port")
        {
            uint16_t port = 0;
            bool valid = isJson ? parsePort(doc["port"], port) : (payload.toInt() > 0 && payload.toInt() <= 65535);
            if (!isJson && valid)
            {
                port = static_cast<uint16_t>(payload.toInt());
            }
            if (!valid)
            {
                request->send(400, "text/plain", "Invalid port");
                return;
            }
            setTargetPort(port);
        }
        else if (url == "/api/config")
        {
            if (!isJson)
            {
                request->send(400, "text/plain", "Invalid JSON");
                return;
            }
            String host = doc["host"].is<const char *>() ? doc["host"].as<String>() : getTargetHostname();
            uint16_t port = getTargetPort();
            if (!doc["port"].isNull() && !parsePort(doc["port"], port))
            {
                request->send(400, "text/plain", "Invalid port");
                return;
            }
            setTarget(host, port);
        }
        else
        {
            request->send(404, "text/plain", "Not Found");
            return;
        }
        request->send(200, "text/plain", "OK"); });

    _server->begin();
}

void Satellite::endWebConfig()
{
    if (_server)
    {
        _server->end();
        delete _server;
        _server = nullptr;
    }
}

void Satellite::beginMdns()
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "mDNS init failed: %d", err);
        return;
    }

    mdns_hostname_set(_hostname.c_str());
    mdns_instance_name_set(_deviceName.c_str());

    mdns_txt_item_t txt[] = {
        {"restEnabled", _options.webConfig ? "true" : "false"},
    };
    mdns_service_add(nullptr, "_companion-satellite", "_tcp", _apiPort, txt, sizeof(txt) / sizeof(txt[0]));
}
