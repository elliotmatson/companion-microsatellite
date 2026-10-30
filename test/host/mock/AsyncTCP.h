// A scripted stand-in for ESP32Async's AsyncClient, mimicking the behaviour the library
// relies on: close() calls onDisconnect synchronously when there is a connection,
// add() only accepts as much as space() allows, callbacks carry the client pointer,
// and received data arrives as TCP-segment-sized pbufs that must be acked or freed.
#pragma once

#include <Arduino.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// Just enough of lwIP's pbuf for received data: the library must hand each one back
// through ackPacket() or pbuf_free(), and livePbufs() catches any it leaks.
struct pbuf
{
    pbuf *next = nullptr;
    void *payload = nullptr;
    uint16_t len = 0;
    std::string storage;
    static int &live()
    {
        static int count = 0;
        return count;
    }
};
inline uint8_t pbuf_free(pbuf *p)
{
    pbuf::live()--;
    delete p;
    return 1;
}

class AsyncClient;

typedef std::function<void(void *, AsyncClient *)> AcConnectHandler;
typedef std::function<void(void *, AsyncClient *, size_t len, uint32_t time)> AcAckHandler;
typedef std::function<void(void *, AsyncClient *, int8_t error)> AcErrorHandler;
typedef std::function<void(void *, AsyncClient *, void *data, size_t len)> AcDataHandler;
typedef std::function<void(void *, AsyncClient *, struct pbuf *pb)> AcPacketHandler;

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
    void onPacket(AcPacketHandler cb, void * = nullptr) { _onPacket = cb; }
    void ackPacket(pbuf *pb)
    {
        if (!pb)
            return;
        ackedBytes += pb->len;
        pbuf_free(pb);
    }

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
        // Like AsyncTCP's _recv: one callback per TCP segment, touching the object again
        // after each, so a client deleted from inside its own callback shows up under
        // AddressSanitizer. With onPacket, the library owns (and must ack or free) each pbuf.
        for (size_t offset = 0; offset < data.size(); offset += kSegment)
        {
            size_t n = std::min(kSegment, data.size() - offset);
            if (_onPacket)
            {
                pbuf *pb = new pbuf();
                pbuf::live()++;
                pb->storage.assign(data, offset, n);
                pb->payload = pb->storage.data();
                pb->len = static_cast<uint16_t>(n);
                _onPacket(nullptr, this, pb);
            }
            else if (_onData)
            {
                _onData(nullptr, this, const_cast<char *>(data.data() + offset), n);
            }
            bytesReceived += n;
        }
    }
    /** @brief Received bytes the library hasn't acked yet - what keeps the sender's TCP window shut */
    size_t unackedBytes() const { return bytesReceived - ackedBytes; }
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
    size_t ackedBytes = 0;
    static constexpr size_t kSegment = 1436;
    int closeCount = 0;

private:
    AcConnectHandler _onConnect, _onDisconnect;
    AcAckHandler _onAck;
    AcErrorHandler _onError;
    AcDataHandler _onData;
    AcPacketHandler _onPacket;
    std::string _pending;
    bool _hasPcb = false;
    bool _connected = false;
};
