// A scripted stand-in for ESP32Async's AsyncClient, mimicking the behaviour the library
// relies on: close() calls onDisconnect synchronously when there is a connection,
// add() only accepts as much as space() allows, and callbacks carry the client pointer.
#pragma once

#include <Arduino.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

class AsyncClient;

typedef std::function<void(void *, AsyncClient *)> AcConnectHandler;
typedef std::function<void(void *, AsyncClient *, size_t len, uint32_t time)> AcAckHandler;
typedef std::function<void(void *, AsyncClient *, int8_t error)> AcErrorHandler;
typedef std::function<void(void *, AsyncClient *, void *data, size_t len)> AcDataHandler;

class AsyncClient
{
public:
    AsyncClient() { all().push_back(this); }
    ~AsyncClient()
    {
        auto &list = all();
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
    }

    void onConnect(AcConnectHandler cb, void * = nullptr) { _onConnect = cb; }
    void onDisconnect(AcConnectHandler cb, void * = nullptr) { _onDisconnect = cb; }
    void onAck(AcAckHandler cb, void * = nullptr) { _onAck = cb; }
    void onError(AcErrorHandler cb, void * = nullptr) { _onError = cb; }
    void onData(AcDataHandler cb, void * = nullptr) { _onData = cb; }

    bool connect(const char *host, uint16_t port)
    {
        host_ = host;
        port_ = port;
        _hasPcb = connectSucceeds();
        return connectSucceeds();
    }

    void close()
    {
        closeCount++;
        if (_hasPcb)
        {
            _hasPcb = false;
            _connected = false;
            if (_onDisconnect)
                _onDisconnect(nullptr, this);
        }
    }

    size_t space() const { return _connected ? spaceAvailable : 0; }
    size_t add(const char *data, size_t size, uint8_t = 0)
    {
        size_t n = std::min(size, space());
        _pending.append(data, n);
        spaceAvailable -= n;
        return n;
    }
    bool send()
    {
        written += _pending;
        _pending.clear();
        return true;
    }
    bool free() const { return !_hasPcb; }
    bool connected() const { return _connected; }
    void setNoDelay(bool) {}
    static const char *errorToString(int8_t) { return "mock error"; }

    // --- test controls ---
    static std::vector<AsyncClient *> &all()
    {
        static std::vector<AsyncClient *> clients;
        return clients;
    }
    static bool &connectSucceeds()
    {
        static bool value = true;
        return value;
    }

    void simulateConnect()
    {
        _hasPcb = true;
        _connected = true;
        if (_onConnect)
            _onConnect(nullptr, this);
    }
    void simulateData(const std::string &data)
    {
        // Like AsyncTCP's _recv, touch the object again after the callback returns, so
        // a client deleted from inside its own onData shows up under AddressSanitizer.
        if (_onData)
            _onData(nullptr, this, const_cast<char *>(data.data()), data.size());
        bytesReceived += data.size();
    }
    void simulateRemoteClose()
    {
        _hasPcb = false;
        _connected = false;
        if (_onDisconnect)
            _onDisconnect(nullptr, this);
    }
    void simulateAck(size_t length)
    {
        spaceAvailable += length;
        if (_onAck)
            _onAck(nullptr, this, length, 0);
    }

    std::string host_;
    uint16_t port_ = 0;
    std::string written;
    size_t spaceAvailable = 5744;
    size_t bytesReceived = 0;
    int closeCount = 0;

private:
    AcConnectHandler _onConnect, _onDisconnect;
    AcAckHandler _onAck;
    AcErrorHandler _onError;
    AcDataHandler _onData;
    std::string _pending;
    bool _hasPcb = false;
    bool _connected = false;
};
