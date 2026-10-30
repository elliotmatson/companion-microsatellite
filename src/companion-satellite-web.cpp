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

void Satellite::beginWebConfig()
{
    _server = new AsyncWebServer(_apiPort);

    _server->on("/api/host", HTTP_GET, [this](AsyncWebServerRequest *request)
                { request->send(200, "text/plain", getTargetHostname()); });
    _server->on("/api/port", HTTP_GET, [this](AsyncWebServerRequest *request)
                { request->send(200, "text/plain", String(getTargetPort())); });
    _server->on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request)
                { request->send(200, "application/json", configJson(getTargetHostname(), getTargetPort())); });

    // POST bodies arrive in chunks: collect them into a null-terminated buffer on the request,
    // then answer once the whole body is in. calloc/free because the request frees
    // _tempObject itself if the client goes away mid-body.
    auto collectBody = [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
    {
        if (total > kMaxBodyBytes)
        {
            return;
        }
        if (index == 0 && !request->_tempObject)
        {
            request->_tempObject = calloc(total + 1, 1);
        }
        if (request->_tempObject && index + len <= total)
        {
            memcpy(static_cast<uint8_t *>(request->_tempObject) + index, data, len);
        }
    };
    auto applyBody = [this](AsyncWebServerRequest *request)
    {
        if (request->contentLength() > kMaxBodyBytes)
        {
            request->send(413, "text/plain", "Request too large");
            return;
        }
        String body = request->_tempObject ? String(static_cast<const char *>(request->_tempObject)) : String();
        ConfigApiResponse response = handleConfigPost(request->url(), body);
        request->send(response.status, "text/plain", response.message);
    };
    for (const char *path : {"/api/host", "/api/port", "/api/config"})
    {
        _server->on(path, HTTP_POST, applyBody, nullptr, collectBody);
    }

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
