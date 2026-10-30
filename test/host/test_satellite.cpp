// Host-side behaviour tests for Satellite, SatelliteSurface and SatelliteSubscription.
//
// Builds the real library sources against small stand-ins for the Arduino core and
// AsyncTCP (test/host/mock), then plays Companion's side of the conversation: it feeds
// lines into the mock client, fires the housekeeping timer, and checks what the library
// sends back and which callbacks run. Built with AddressSanitizer, so lifetime bugs
// (eg. deleting a client from inside its own callback) fail the run.

#include <AsyncTCP.h>
#include <Preferences.h>
#include <Ticker.h>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include "satellite-protocol.hpp"

// Reach into Satellite for the timer and client - test only. Every other header is
// included above, so only the library's own classes are affected.
#define private public
#include "companion-satellite.hpp"
#undef private

#include <ArduinoJson.h>

#include <cstdio>
#include <string>
#include <vector>

uint32_t g_mockMillis = 1000;
bool g_mockVerbose = false;
MockEsp ESP;

void Satellite::beginWebConfig() {}
void Satellite::endWebConfig() {}
void Satellite::beginMdns() {}

static int failures = 0;
static int checks = 0;
static const char *currentTest = "";

#define CHECK(cond, ...)                                                              \
    do                                                                                \
    {                                                                                 \
        checks++;                                                                     \
        if (!(cond))                                                                  \
        {                                                                             \
            failures++;                                                               \
            printf("FAIL [%s] %s:%d: %s - ", currentTest, __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                                      \
            printf("\n");                                                             \
        }                                                                             \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static SatelliteOptions testOptions(SatelliteDispatchMode mode = SatelliteDispatchMode::LOOP)
{
    SatelliteOptions options;
    options.dispatch = mode;
    options.webConfig = false;
    options.mdns = false;
    options.maxLineLength = 4096;
    return options;
}

static AsyncClient *currentClient(Satellite &satellite)
{
    return satellite._client;
}

/** Advance time, firing the 250ms housekeeping tick along the way */
static void advance(Satellite &satellite, uint32_t ms)
{
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 250)
    {
        g_mockMillis += 250;
        satellite._ticker.fire();
    }
}

static std::vector<std::string> takeLines(AsyncClient *client)
{
    std::vector<std::string> lines;
    size_t start = 0;
    size_t end;
    while ((end = client->written.find('\n', start)) != std::string::npos)
    {
        lines.push_back(client->written.substr(start, end - start));
        start = end + 1;
    }
    client->written.erase(0, start);
    return lines;
}

static bool hasLineStarting(const std::vector<std::string> &lines, const std::string &prefix, std::string *found = nullptr)
{
    for (const std::string &line : lines)
    {
        if (line.compare(0, prefix.size(), prefix) == 0)
        {
            if (found)
                *found = line;
            return true;
        }
    }
    return false;
}

static int countLinesStarting(const std::vector<std::string> &lines, const std::string &prefix)
{
    int count = 0;
    for (const std::string &line : lines)
        count += line.compare(0, prefix.size(), prefix) == 0;
    return count;
}

/** Extract and decode a quoted base64 parameter from a sent line */
static std::string decodedParam(const std::string &line, const std::string &key)
{
    std::string marker = " " + key + "=\"";
    size_t start = line.find(marker);
    if (start == std::string::npos)
        return "";
    start += marker.size();
    size_t end = line.find('"', start);
    return satellite_protocol::base64Decode(std::string_view(line).substr(start, end - start)).value_or("<bad base64>");
}

static const char *kBegin = "BEGIN CompanionVersion=4.3.0 ApiVersion=1.15.0\n";
static const char *kCapsAll = "CAPS SUBSCRIPTIONS=1 NONSQUARE=1 BITMAP_FORMATS=\"rgb,png,webp\" ROTARY_AMOUNT=1\n";

/** Connect and negotiate, returning the client */
static AsyncClient *connectReady(Satellite &satellite, const char *caps = kCapsAll)
{
    AsyncClient *client = currentClient(satellite);
    if (!client)
        return nullptr;
    client->simulateConnect();
    client->simulateData(kBegin);
    client->simulateData(caps);
    return client;
}

static void ackDeviceAdds(AsyncClient *client, const std::vector<std::string> &lines)
{
    for (const std::string &line : lines)
    {
        if (line.compare(0, 11, "ADD-DEVICE ") == 0)
        {
            size_t start = line.find("DEVICEID=\"") + 10;
            std::string id = line.substr(start, line.find('"', start) - start);
            client->simulateData("ADD-DEVICE OK DEVICEID=\"" + id + "\"\n");
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testNegotiationAndRegistration()
{
    currentTest = "negotiation";
    Satellite satellite("test-negotiation");
    SatelliteSurfaceConfig config;
    config.totalKeys = 4;
    config.keysPerRow = 2;
    config.bitmapSize = 72;
    config.bitmapFormat = "png";
    config.canChangePageLabel = "Let it \"page\"";
    SatelliteSurface surface(satellite, "Surface 1", config, {{"in1", SatelliteSurfaceVariableType::VARIABLE_INPUT, "In", "desc"}});

    std::vector<SatelliteConnectionState> states;
    satellite.setOnConnectionStateChange([&](SatelliteConnectionState state)
                                         { states.push_back(state); });
    int registered = 0;
    surface.setOnRegistered([&]()
                            { registered++; });

    satellite.setTarget("companion.local", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = currentClient(satellite);
    CHECK(client != nullptr, "client created");
    CHECK(client->host_ == "companion.local" && client->port_ == 16622, "target %s:%u", client->host_.c_str(), client->port_);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::CONNECTING, "connecting");

    client->simulateConnect();
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::NEGOTIATING, "negotiating");

    client->simulateData(kBegin);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::NEGOTIATING, "still negotiating until CAPS");
    CHECK(satellite.getSatelliteApiVersion() == "1.15.0", "api version");
    CHECK(satellite.getCompanionVersion() == "4.3.0", "companion version");
    CHECK(!hasLineStarting(takeLines(client), "ADD-DEVICE"), "no ADD-DEVICE before CAPS");

    client->simulateData(kCapsAll);
    CHECK(satellite.isReady(), "ready after CAPS");
    CHECK(satellite.supportsSubscriptions() && satellite.supportsNonsquareButtons() && satellite.supportsRotaryAmount(), "caps features");

    std::vector<std::string> lines = takeLines(client);
    std::string add;
    CHECK(hasLineStarting(lines, "ADD-DEVICE DEVICEID=\"usat:", &add), "ADD-DEVICE sent");
    CHECK(add.find("BITMAP_FORMAT=\"png\"") != std::string::npos, "negotiated png: %s", add.c_str());
    CHECK(add.find("BITMAPS=72") != std::string::npos, "bitmap size");
    CHECK(add.find("CAN_CHANGE_PAGE=\"Let it \\\"page\\\"\"") != std::string::npos, "label escaped: %s", add.c_str());
    CHECK(decodedParam(add, "VARIABLES").find("\"type\":\"input\"") != std::string::npos, "variables");

    CHECK(!surface.isRegistered(), "not registered until OK");
    ackDeviceAdds(client, lines);
    CHECK(surface.isRegistered(), "registered after OK");

    CHECK(registered == 0 && states.empty(), "callbacks wait for loop()");
    satellite.loop();
    CHECK(registered == 1, "onRegistered ran in loop()");
    CHECK(states.size() == 3 && states[0] == SatelliteConnectionState::CONNECTING && states[1] == SatelliteConnectionState::NEGOTIATING &&
              states[2] == SatelliteConnectionState::READY,
          "state sequence (%zu states)", states.size());
}

static void testKeyStateAndCoalescing()
{
    currentTest = "key-state";
    Satellite satellite("test-keystate");
    SatelliteSurfaceConfig config;
    config.totalKeys = 4;
    SatelliteSurface surface(satellite, "Keys", config);
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));
    satellite.loop();
    std::string id = surface.getID().c_str();

    struct Seen
    {
        uint16_t index;
        std::string text;
        std::string bitmap;
        int r;
        bool pressed;
        uint8_t fontSize;
    };
    std::vector<Seen> seen;
    std::vector<std::string> order;
    surface.setOnKeyStateChange([&](const SatelliteControlState &state)
                                {
                                    Seen s{state.index, state.text.value_or("").c_str(), "", state.keyColor ? state.keyColor->r : -1,
                                           state.pressed.value_or(false), state.fontSize.value_or(99)};
                                    if (state.bitmap)
                                    {
                                        uint8_t buffer[16];
                                        size_t n = state.bitmap->decode(buffer, sizeof(buffer));
                                        s.bitmap.assign(reinterpret_cast<char *>(buffer), n);
                                    }
                                    seen.push_back(s);
                                    order.push_back("key" + std::to_string(state.index)); });
    surface.setOnKeysClear([&]()
                           { order.push_back("clear"); });

    // TEXT "Play", bitmap "abc", escaped DEVICEID like Companion sends
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=1 TYPE=\"BUTTON\" BITMAP=\"YWJj\" COLOR=\"#ff0000\" TEXTCOLOR=\"#000000\" TEXT=\"UGxheQ==\" FONT_SIZE=\"auto\" PRESSED=1\n");
    satellite.loop();
    CHECK(seen.size() == 1, "one callback");
    if (!seen.empty())
    {
        CHECK(seen[0].index == 1 && seen[0].text == "Play" && seen[0].bitmap == "abc" && seen[0].r == 255 && seen[0].pressed && seen[0].fontSize == 0,
              "fields: index=%u text=%s bitmap=%s r=%d font=%u", seen[0].index, seen[0].text.c_str(), seen[0].bitmap.c_str(), seen[0].r, seen[0].fontSize);
    }

    // Three updates to key 2 before loop() runs: only the newest is delivered
    seen.clear();
    order.clear();
    for (const char *text : {"MQ==", "Mg==", "Mw=="})
    {
        client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=2 TEXT=\"" + text + "\"\n");
    }
    satellite.loop();
    CHECK(seen.size() == 1 && seen[0].text == "3", "coalesced to the latest (%zu callbacks)", seen.size());

    // ...but never across a KEYS-CLEAR, which must stay in order
    order.clear();
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=0 TEXT=\"MQ==\"\n");
    client->simulateData("KEYS-CLEAR DEVICEID=\"" + id + "\"\n");
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=0 TEXT=\"Mg==\"\n");
    satellite.loop();
    CHECK(order.size() == 3 && order[0] == "key0" && order[1] == "clear" && order[2] == "key0", "order kept around KEYS-CLEAR (%zu)", order.size());

    // Messages for an unknown device are ignored
    client->simulateData("KEY-STATE DEVICEID=\"someone-else\" KEY=0 TEXT=\"MQ==\"\n");
    seen.clear();
    satellite.loop();
    CHECK(seen.empty(), "unknown device ignored");
}

static void testSurfaceMessages()
{
    currentTest = "surface-messages";
    Satellite satellite("test-messages");
    SatelliteSurfaceConfig config;
    config.pincodeSupport = SatelliteSurfacePincodeSupport::FULL;
    SatelliteSurface surface(satellite, "Messages", config);
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));
    std::string id = surface.getID().c_str();

    int brightness = -1;
    std::string variableId, variableValue, configJson, errorCommand, errorMessage;
    SatelliteLockedState locked;
    surface.setOnBrightnessSet([&](uint8_t value)
                               { brightness = value; });
    surface.setOnVariableSet([&](const String &id, const String &value)
                             { variableId = id.c_str(); variableValue = value.c_str(); });
    surface.setOnLockedStateSet([&](const SatelliteLockedState &state)
                                { locked = state; });
    surface.setOnDeviceConfig([&](const String &json)
                              { configJson = json.c_str(); });
    surface.setOnError([&](const String &command, const String &message)
                       { errorCommand = command.c_str(); errorMessage = message.c_str(); });

    client->simulateData("BRIGHTNESS DEVICEID=\"" + id + "\" VALUE=150\n");
    client->simulateData("VARIABLE-VALUE DEVICEID=\"" + id + "\" VARIABLE=\"greeting\" VALUE=\"aGkgdGhlcmU=\"\n");
    client->simulateData("LOCKED-STATE DEVICEID=\"" + id + "\" LOCKED=1 CHARACTER_COUNT=3 ROTATION=-90\n");
    client->simulateData("DEVICE-CONFIG DEVICEID=\"" + id + "\" CONFIG=\"eyJhIjoxfQ==\"\n");
    client->simulateData("KEY-PRESS ERROR DEVICEID=\"" + id + "\" MESSAGE=\"Invalid \\\"KEY\\\"\"\n");
    satellite.loop();

    CHECK(brightness == 100, "brightness clamped (%d)", brightness);
    CHECK(variableId == "greeting" && variableValue == "hi there", "variable %s=%s", variableId.c_str(), variableValue.c_str());
    CHECK(locked.locked && locked.characterCount == 3 && locked.rotation == -90, "locked state");
    CHECK(configJson == "{\"a\":1}", "config %s", configJson.c_str());
    CHECK(errorCommand == "KEY-PRESS" && errorMessage == "Invalid \"KEY\"", "error %s: %s", errorCommand.c_str(), errorMessage.c_str());
}

static void testSending()
{
    currentTest = "sending";
    Satellite satellite("test-sending");
    SatelliteSurfaceConfig config;
    config.totalKeys = 4;
    SatelliteSurface surface(satellite, "Send", config);
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = currentClient(satellite);

    // Nothing is sent before the surface is registered
    surface.buttonAction(0, SatelliteAction::PRESSED);
    client->simulateConnect();
    client->simulateData(kBegin);
    client->simulateData("CAPS SUBSCRIPTIONS=0 BITMAP_FORMATS=\"rgb\"\n"); // no ROTARY_AMOUNT
    std::vector<std::string> lines = takeLines(client);
    CHECK(!hasLineStarting(lines, "KEY-PRESS"), "no KEY-PRESS before registration");
    ackDeviceAdds(client, lines);

    surface.buttonAction(1, SatelliteAction::PRESSED);
    surface.buttonAction(1, SatelliteAction::RELEASED);
    surface.buttonAction(9, SatelliteAction::PRESSED); // out of range
    surface.rotate(2, 3);
    surface.rotate(2, -1);
    surface.setVariable("v", "x y\"z");
    surface.changePage(SatellitePageDirection::PREVIOUS);
    surface.pressPincodeKey(7);
    surface.reportFirmwareUpdateInfo("");
    lines = takeLines(client);

    std::string id = surface.getID().c_str();
    CHECK(hasLineStarting(lines, "KEY-PRESS DEVICEID=\"" + id + "\" KEY=1 PRESSED=1"), "press");
    CHECK(hasLineStarting(lines, "KEY-PRESS DEVICEID=\"" + id + "\" KEY=1 PRESSED=0"), "release");
    CHECK(countLinesStarting(lines, "KEY-PRESS") == 2, "out of range key not sent");
    CHECK(countLinesStarting(lines, "KEY-ROTATE DEVICEID=\"" + id + "\" KEY=2 DIRECTION=1") == 3, "3 single steps without ROTARY_AMOUNT");
    CHECK(countLinesStarting(lines, "KEY-ROTATE DEVICEID=\"" + id + "\" KEY=2 DIRECTION=-1") == 1, "one step back");
    std::string variableLine;
    CHECK(hasLineStarting(lines, "SET-VARIABLE-VALUE", &variableLine) && decodedParam(variableLine, "VALUE") == "x y\"z", "variable value");
    CHECK(hasLineStarting(lines, "CHANGE-PAGE DEVICEID=\"" + id + "\" DIRECTION=0"), "page previous = 0");
    CHECK(hasLineStarting(lines, "PINCODE-KEY DEVICEID=\"" + id + "\" KEY=7"), "pincode");
    CHECK(hasLineStarting(lines, "FIRMWARE-UPDATE-INFO DEVICEID=\"" + id + "\" UPDATE_URL=\"\""), "clear update url");

    // With ROTARY_AMOUNT a rotation is one message, split at Companion's 100 step limit
    client->simulateRemoteClose();
    advance(satellite, 2000);
    client = connectReady(satellite);
    CHECK(client != nullptr, "reconnected");
    if (!client)
        return;
    ackDeviceAdds(client, takeLines(client));
    surface.rotate(2, 7);
    surface.rotate(2, -250);
    lines = takeLines(client);
    CHECK(countLinesStarting(lines, "KEY-ROTATE") == 4, "1 + 3 rotate messages (%d)", countLinesStarting(lines, "KEY-ROTATE"));
    CHECK(hasLineStarting(lines, "KEY-ROTATE DEVICEID=\"" + id + "\" KEY=2 DIRECTION=7 "), "7 steps in one");
    CHECK(countLinesStarting(lines, "KEY-ROTATE DEVICEID=\"" + id + "\" KEY=2 DIRECTION=-100") == 2 &&
              hasLineStarting(lines, "KEY-ROTATE DEVICEID=\"" + id + "\" KEY=2 DIRECTION=-50"),
          "-250 as -100,-100,-50");
}

static void testSendBuffering()
{
    currentTest = "send-buffering";
    Satellite satellite("test-buffering");
    SatelliteSurfaceConfig config;
    config.totalKeys = 32;
    SatelliteSurface surface(satellite, "Buffered", config);
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));

    // A tiny TCP send buffer: nothing may be cut off part way through a line
    client->spaceAvailable = 10;
    for (int i = 0; i < 20; i++)
    {
        surface.buttonAction(static_cast<uint16_t>(i), SatelliteAction::PRESSED);
    }
    CHECK(client->written.size() == 10, "only what fits was handed over (%zu)", client->written.size());
    for (int i = 0; i < 200 && !satellite._txBuffer.empty(); i++)
    {
        client->simulateAck(10);
    }
    std::vector<std::string> lines = takeLines(client);
    int pressCount = countLinesStarting(lines, "KEY-PRESS DEVICEID=");
    CHECK(pressCount == 20, "all 20 presses arrived intact and in order (%d)", pressCount);
    for (int i = 0; i < pressCount && i < 20; i++)
    {
        CHECK(lines[i].find(" KEY=" + std::to_string(i) + " ") != std::string::npos, "line %d in order: %s", i, lines[i].c_str());
    }

    // The buffer is bounded rather than growing without limit
    client->spaceAvailable = 0;
    for (int i = 0; i < 2000; i++)
    {
        surface.buttonAction(0, SatelliteAction::PRESSED);
    }
    CHECK(satellite._txBuffer.size() <= 16384, "tx buffer bounded (%zu)", satellite._txBuffer.size());
}

static void testVersionRejected()
{
    currentTest = "version-rejected";
    Satellite satellite("test-rejected");
    satellite.setTarget("old-companion", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = currentClient(satellite);
    client->simulateConnect();

    // Rejecting closes the client from inside its own onData - which must not delete it
    // (the mock touches the client after the callback, so ASan would catch that)
    client->simulateData("BEGIN CompanionVersion=3.4.0 ApiVersion=1.7.1\nCAPS SUBSCRIPTIONS=1\n");
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::REJECTED, "rejected");
    CHECK(currentClient(satellite) == nullptr, "client retired");
    CHECK(AsyncClient::all().size() == 1, "retired client not deleted yet");

    advance(satellite, 60000);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::REJECTED, "no retry while rejected");
    CHECK(AsyncClient::all().empty(), "retired client deleted later (%zu left)", AsyncClient::all().size());

    // Changing the target tries again
    satellite.setTarget("new-companion", 16622);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::CONNECTING, "connecting to new target");
    CHECK(currentClient(satellite) && currentClient(satellite)->host_ == "new-companion", "new host");

    // A missing or invalid ApiVersion is rejected too
    currentClient(satellite)->simulateConnect();
    currentClient(satellite)->simulateData("BEGIN CompanionVersion=x\n");
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::REJECTED, "missing ApiVersion rejected");
}

static void testKeepaliveAndReconnect()
{
    currentTest = "keepalive";
    Satellite satellite("test-keepalive");
    SatelliteSurface surface(satellite, "KA");
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));
    CHECK(surface.isRegistered(), "registered");

    // Pings go out every 2s, and answers keep the connection alive
    for (int i = 0; i < 5; i++)
    {
        advance(satellite, 2000);
        client->simulateData("PONG usat\n");
    }
    std::vector<std::string> lines = takeLines(client);
    CHECK(countLinesStarting(lines, "PING") >= 4, "pings sent (%d)", countLinesStarting(lines, "PING"));
    CHECK(satellite.isReady(), "still ready while PONGs arrive");

    // Companion goes silent (eg. lost power): reconnect without waiting for TCP to notice
    advance(satellite, 6500);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::WAITING, "gave up on a silent connection");
    CHECK(!surface.isRegistered(), "registration reset");

    advance(satellite, 1000);
    AsyncClient *next = currentClient(satellite);
    CHECK(next != nullptr && next != client, "new client after backoff");
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::CONNECTING, "connecting again");

    // Connection attempts that never complete time out, with growing backoff
    advance(satellite, 10000);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::WAITING, "connect timed out");
    advance(satellite, 1000);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::WAITING, "backoff doubled to 2s");
    advance(satellite, 1250);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::CONNECTING, "retry after 2s");

    // Re-registered on the new connection, and the backoff resets once ready
    client = connectReady(satellite);
    lines = takeLines(client);
    CHECK(hasLineStarting(lines, "ADD-DEVICE"), "surface re-registered after reconnect");
    CHECK(satellite._reconnectDelayMs == 1000, "backoff reset");

    // A synchronous connect() failure still retries
    AsyncClient::connectSucceeds() = false;
    client->simulateRemoteClose();
    advance(satellite, 1000);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::WAITING, "waiting after failed connect()");
    AsyncClient::connectSucceeds() = true;
    advance(satellite, 2000);
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::CONNECTING, "retrying after failed connect()");

    // Answering Companion's PING with its payload
    client = currentClient(satellite);
    client->simulateConnect();
    client->simulateData("PING abc 123\n");
    lines = takeLines(client);
    CHECK(hasLineStarting(lines, "PONG abc 123"), "PONG echoes payload");
}

static void testOversizedLine()
{
    currentTest = "oversized";
    Satellite satellite("test-oversized");
    SatelliteSurface surface(satellite, "Big");
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions()); // maxLineLength 4096
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));
    std::string id = surface.getID().c_str();

    std::vector<std::string> texts;
    surface.setOnKeyStateChange([&](const SatelliteControlState &state)
                                { texts.push_back(state.text.value_or("").c_str()); });

    std::string huge = "KEY-STATE DEVICEID=\"" + id + "\" KEY=0 BITMAP=\"" + std::string(10000, 'A') + "\" TEXT=\"MQ==\"\n";
    // Arrives in TCP-sized chunks, followed by a normal line in the same chunk as its end
    std::string stream = huge + "KEY-STATE DEVICEID=\"" + id + "\" KEY=1 TEXT=\"Mg==\"\n";
    for (size_t pos = 0; pos < stream.size(); pos += 1460)
    {
        client->simulateData(stream.substr(pos, 1460));
    }
    satellite.loop();
    CHECK(texts.size() == 1 && texts[0] == "2", "oversized line skipped, next one delivered (%zu)", texts.size());
    CHECK(satellite.isReady(), "connection kept");
}

static void testSubscriptions()
{
    currentTest = "subscriptions";
    Satellite satellite("test-subs");
    SatelliteSubscriptionConfig config;
    config.bitmapSize = 72;
    config.bitmapFormat = "webp";
    SatelliteSubscription good(satellite, "1/0/0", config);
    SatelliteSubscription bad(satellite, "99/99/99");

    std::vector<std::string> goodTexts;
    std::string badError;
    good.setOnStateChange([&](const SatelliteControlState &state)
                          { goodTexts.push_back(state.text.value_or("").c_str()); });
    bad.setOnError([&](const String &message)
                   { badError = message.c_str(); });

    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = connectReady(satellite);
    std::vector<std::string> lines = takeLines(client);
    std::string addGood;
    CHECK(hasLineStarting(lines, "ADD-SUB SUBID=\"" + std::string(good.getID().c_str()) + "\" LOCATION=\"1/0/0\"", &addGood), "ADD-SUB sent");
    CHECK(addGood.find("BITMAP=72") != std::string::npos && addGood.find("BITMAP_FORMAT=\"webp\"") != std::string::npos, "%s", addGood.c_str());
    CHECK(countLinesStarting(lines, "ADD-SUB") == 2, "both subscriptions sent");

    // Companion answers in order; its errors carry no SUBID
    client->simulateData("ADD-SUB OK SUBID=\"" + std::string(good.getID().c_str()) + "\"\n");
    client->simulateData("SUB-STATE SUBID=\"" + std::string(good.getID().c_str()) + "\" TYPE=\"BUTTON\" TEXT=\"aGk=\" PRESSED=0\n");
    client->simulateData("ADD-SUB ERROR MESSAGE=\"Invalid LOCATION\"\n");
    satellite.loop();
    CHECK(good.isActive() && !bad.isActive(), "activity");
    CHECK(goodTexts.size() == 1 && goodTexts[0] == "hi", "SUB-STATE delivered");
    CHECK(badError == "Invalid LOCATION", "error routed to the right subscription (%s)", badError.c_str());

    good.action(SatelliteAction::PRESSED);
    good.rotate(-3);
    bad.action(SatelliteAction::PRESSED); // not active - not sent
    lines = takeLines(client);
    CHECK(hasLineStarting(lines, "SUB-PRESS SUBID=\"" + std::string(good.getID().c_str()) + "\" PRESSED=1"), "SUB-PRESS");
    CHECK(hasLineStarting(lines, "SUB-ROTATE SUBID=\"" + std::string(good.getID().c_str()) + "\" DIRECTION=-3"), "SUB-ROTATE with amount");
    CHECK(countLinesStarting(lines, "SUB-PRESS") == 1, "inactive subscription doesn't send");

    // Re-subscribed after a reconnect
    client->simulateRemoteClose();
    CHECK(!good.isActive(), "inactive after disconnect");
    advance(satellite, 1000);
    client = connectReady(satellite);
    lines = takeLines(client);
    CHECK(countLinesStarting(lines, "ADD-SUB") == 2, "re-subscribed after reconnect");

    // Moving a subscription
    client->simulateData("ADD-SUB OK SUBID=\"" + std::string(good.getID().c_str()) + "\"\n");
    client->simulateData("ADD-SUB ERROR MESSAGE=\"Invalid LOCATION\"\n");
    good.setLocation("2/1/1");
    lines = takeLines(client);
    CHECK(hasLineStarting(lines, "REMOVE-SUB SUBID=\"" + std::string(good.getID().c_str()) + "\""), "REMOVE-SUB on move");
    CHECK(hasLineStarting(lines, "ADD-SUB SUBID=\"" + std::string(good.getID().c_str()) + "\" LOCATION=\"2/1/1\""), "ADD-SUB at new location");

    // Moving one whose ADD-SUB hasn't been answered yet still removes it first
    client->simulateData("ADD-SUB OK SUBID=\"" + std::string(good.getID().c_str()) + "\"\n");
    good.setLocation("3/0/0");
    lines = takeLines(client);
    CHECK(countLinesStarting(lines, "REMOVE-SUB") == 1 && countLinesStarting(lines, "ADD-SUB") == 1, "remove then add");
    good.setLocation("4/0/0"); // the 3/0/0 ADD-SUB is still pending
    lines = takeLines(client);
    CHECK(hasLineStarting(lines, "REMOVE-SUB SUBID=\"" + std::string(good.getID().c_str()) + "\""), "REMOVE-SUB for a pending subscription");

    // Disabled in Companion's settings: nothing is sent
    client->simulateRemoteClose();
    advance(satellite, 2000);
    client = connectReady(satellite, "CAPS SUBSCRIPTIONS=0\n");
    lines = takeLines(client);
    CHECK(countLinesStarting(lines, "ADD-SUB") == 0, "no ADD-SUB when disabled");
}

static void testLayoutAndLeds()
{
    currentTest = "layout";
    Satellite satellite("test-layout");
    SatelliteSurfaceConfig config;
    config.bitmapFormat = "png";
    config.layout = SatelliteLayout{
        {
            {.name = "default", .bitmapWidth = 96, .bitmapHeight = 48, .text = true},
            {.name = "ring", .colors = true, .ledSegments = 4, .ledsMode = SatelliteLedsMode::FULL_RING},
        },
        {
            {.id = "0/0", .row = 0, .column = 0},
            {.id = "1/0", .row = 1, .column = 0, .stylePreset = "ring"},
            {.id = "bad id!", .row = 2, .column = 0},
        },
    };
    SatelliteSurface surface(satellite, "Layout", config);
    satellite.setTarget("c", 16622);
    satellite.begin(testOptions());
    AsyncClient *client = connectReady(satellite);
    std::vector<std::string> lines = takeLines(client);
    std::string add;
    CHECK(hasLineStarting(lines, "ADD-DEVICE", &add), "ADD-DEVICE");
    CHECK(add.find("KEYS_TOTAL") == std::string::npos, "no grid fields with a layout");

    JsonDocument manifest;
    CHECK(deserializeJson(manifest, decodedParam(add, "LAYOUT_MANIFEST")) == DeserializationError::Ok, "manifest is JSON");
    CHECK(manifest["stylePresets"]["default"]["bitmap"]["w"] == 96 && manifest["stylePresets"]["default"]["bitmap"]["h"] == 48, "bitmap size");
    CHECK(manifest["stylePresets"]["default"]["text"] == true, "text");
    CHECK(manifest["stylePresets"]["ring"]["colors"] == "hex", "colors");
    CHECK(manifest["stylePresets"]["ring"]["leds"]["segments"] == 4 && manifest["stylePresets"]["ring"]["leds"]["mode"] == "full-ring", "leds");
    CHECK(manifest["controls"]["1/0"]["row"] == 1 && manifest["controls"]["1/0"]["stylePreset"] == "ring", "control");
    CHECK(manifest["controls"].size() == 2, "invalid control id dropped");
    ackDeviceAdds(client, lines);
    std::string id = surface.getID().c_str();

    uint16_t index = 999;
    std::string controlId;
    std::vector<uint8_t> leds;
    surface.setOnKeyStateChange([&](const SatelliteControlState &state)
                                {
                                    index = state.index;
                                    controlId = state.controlId.c_str();
                                    if (state.leds)
                                    {
                                        leds.resize(state.leds->decodedLength());
                                        leds.resize(state.leds->decode(leds.data(), leds.size()));
                                    } });
    // 4 segments of R,G,B: red, green, blue, white
    std::string ledBytes("\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12);
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" CONTROLID=\"1/0\" LEDS=\"" + satellite_protocol::base64Encode(ledBytes) + "\"\n");
    satellite.loop();
    CHECK(index == 1 && controlId == "1/0", "control index %u id %s", index, controlId.c_str());
    CHECK(leds.size() == 12 && leds[0] == 0xff && leds[4] == 0xff && leds[8] == 0xff && leds[11] == 0xff, "led bytes (%zu)", leds.size());

    surface.buttonAction(1, SatelliteAction::PRESSED);
    surface.rotate(1, 2);
    surface.controlAction("0/0", SatelliteAction::RELEASED);
    lines = takeLines(client);
    CHECK(hasLineStarting(lines, "KEY-PRESS DEVICEID=\"" + id + "\" CONTROLID=\"1/0\" PRESSED=1"), "press by index uses CONTROLID");
    CHECK(hasLineStarting(lines, "KEY-ROTATE DEVICEID=\"" + id + "\" CONTROLID=\"1/0\" DIRECTION=2"), "rotate by CONTROLID");
    CHECK(hasLineStarting(lines, "KEY-PRESS DEVICEID=\"" + id + "\" CONTROLID=\"0/0\" PRESSED=0"), "controlAction");

    // A png data url bitmap
    SatelliteEncodedData dataUrl(std::string_view("data:image/png;base64,iVBORw0KGgo="));
    CHECK(dataUrl.isDataUrl() && dataUrl.mimeType() == "image/png" && dataUrl.base64() == "iVBORw0KGgo=", "data url parts");
}

static void testImmediateDispatchAndRemoval()
{
    currentTest = "immediate";
    Satellite satellite("test-immediate");
    std::vector<SatelliteConnectionState> states;
    satellite.setOnConnectionStateChange([&](SatelliteConnectionState state)
                                         { states.push_back(state); });
    auto *surface = new SatelliteSurface(satellite, "Temp");
    int keyStates = 0;
    surface->setOnKeyStateChange([&](const SatelliteControlState &)
                                 { keyStates++; });

    satellite.setTarget("c", 16622);
    satellite.begin(testOptions(SatelliteDispatchMode::IMMEDIATE));
    AsyncClient *client = connectReady(satellite);
    CHECK(!states.empty() && states.back() == SatelliteConnectionState::READY, "state callbacks without loop()");
    ackDeviceAdds(client, takeLines(client));
    std::string id = surface->getID().c_str();
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=0 TEXT=\"MQ==\"\n");
    CHECK(keyStates == 1, "key state delivered immediately");

    delete surface;
    std::vector<std::string> lines = takeLines(client);
    CHECK(hasLineStarting(lines, "REMOVE-DEVICE DEVICEID=\"" + id + "\""), "REMOVE-DEVICE on destruction");
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=0 TEXT=\"MQ==\"\n");
    CHECK(keyStates == 1, "no callbacks after destruction");
}

static void testQueueBound()
{
    currentTest = "queue-bound";
    Satellite satellite("test-queue");
    SatelliteSurfaceConfig config;
    config.totalKeys = 200;
    SatelliteSurface surface(satellite, "Q", config);
    SatelliteOptions options = testOptions();
    options.maxQueuedBytes = 8192;
    satellite.setTarget("c", 16622);
    satellite.begin(options);
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));
    std::string id = surface.getID().c_str();

    // loop() isn't being called: different keys can't be merged, so the queue must cap out
    for (int i = 0; i < 200; i++)
    {
        client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=" + std::to_string(i) + " TEXT=\"" + std::string(100, 'A') + "\"\n");
    }
    CHECK(satellite._queuedBytes <= 8192, "queue bounded (%zu bytes)", satellite._queuedBytes);
    CHECK(satellite._droppedUpdates > 0, "updates dropped");
    int delivered = 0;
    surface.setOnKeyStateChange([&](const SatelliteControlState &)
                                { delivered++; });
    satellite.loop();
    CHECK(delivered > 0 && delivered < 200, "delivered what fit (%d)", delivered);
    CHECK(satellite._queuedBytes == 0, "accounting returns to zero (%zu)", satellite._queuedBytes);
}

static void testSavedTarget()
{
    currentTest = "saved-target";
    {
        Satellite satellite("test-saved");
        satellite.begin(testOptions());
        CHECK(satellite.getConnectionState() == SatelliteConnectionState::IDLE, "idle with no target");
        CHECK(currentClient(satellite) == nullptr, "no client without a target");
        satellite.setTarget("saved-host", 1234);
    }
    {
        Satellite satellite("test-saved");
        satellite.begin(testOptions());
        CHECK(satellite.getTargetHostname() == "saved-host" && satellite.getTargetPort() == 1234, "target restored from preferences");
        CHECK(currentClient(satellite) && currentClient(satellite)->host_ == "saved-host", "connects to saved target");
    }
}

// The config API's POST handling (companion-satellite-web.cpp routes /api/host, /api/port and
// /api/config POSTs here). On the device, POSTs used to fall through to the web server's
// not-found handler and answer 404 even though the target had been changed.
static void testConfigApi()
{
    currentTest = "config-api";
    Satellite satellite("test-config-api");
    satellite.begin(testOptions());

    Satellite::ConfigApiResponse response = satellite.handleConfigPost("/api/config", "{\"host\":\"companion.lan\",\"port\":16622}");
    CHECK(response.status == 200, "config POST answers 200 (got %d)", response.status);
    CHECK(satellite.getTargetHostname() == "companion.lan" && satellite.getTargetPort() == 16622, "config POST sets the target");
    CHECK(currentClient(satellite) && currentClient(satellite)->host_ == "companion.lan", "config POST connects");

    response = satellite.handleConfigPost("/api/config", "{\"port\":1234}");
    CHECK(response.status == 200 && satellite.getTargetHostname() == "companion.lan" && satellite.getTargetPort() == 1234,
          "config POST without host keeps the host");

    response = satellite.handleConfigPost("/api/host", "  other-host\n");
    CHECK(response.status == 200 && satellite.getTargetHostname() == "other-host", "plain-text host POST");
    response = satellite.handleConfigPost("/api/port", "{\"port\":4321}");
    CHECK(response.status == 200 && satellite.getTargetPort() == 4321, "JSON port POST");

    CHECK(satellite.handleConfigPost("/api/config", "not json").status == 400, "invalid JSON rejected");
    CHECK(satellite.handleConfigPost("/api/config", "{\"port\":0}").status == 400, "port 0 rejected");
    CHECK(satellite.handleConfigPost("/api/port", "99999").status == 400, "out of range port rejected");
    CHECK(satellite.handleConfigPost("/api/host", "").status == 400, "empty host rejected");
    CHECK(satellite.handleConfigPost("/api/other", "{}").status == 404, "unknown path is 404");
    CHECK(satellite.getTargetHostname() == "other-host" && satellite.getTargetPort() == 4321, "rejected POSTs leave the target alone");
}

// A large message that can't be buffered for lack of heap is skipped, and the session carries on
// (on a CYD, a burst of 26KB KEY-STATE bitmaps aborted the device from inside the line buffer).
static void testReceiveOutOfMemory()
{
    currentTest = "receive-out-of-memory";
    Satellite satellite("test-oom");
    SatelliteSurfaceConfig config;
    config.totalKeys = 4;
    SatelliteSurface surface(satellite, "Keys", config);
    satellite.setTarget("c", 16622);
    SatelliteOptions options = testOptions();
    options.maxLineLength = 65536; // so only the memory check can skip the big line
    satellite.begin(options);
    AsyncClient *client = connectReady(satellite);
    ackDeviceAdds(client, takeLines(client));
    satellite.loop();
    std::string id = surface.getID().c_str();

    std::vector<uint16_t> seen;
    surface.setOnKeyStateChange([&](const SatelliteControlState &state)
                                { seen.push_back(state.index); });

    satellite._rxBuffer.setAllocationCheck([](size_t bytes)
                                           { return bytes <= 4096; });
    std::string bigBitmap(20000, 'A');
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=1 BITMAP=\"" + bigBitmap + "\"\n");
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=2 TEXT=\"MQ==\"\n");
    satellite.loop();
    CHECK(satellite.getConnectionState() == SatelliteConnectionState::READY, "still connected after the skipped message");
    CHECK(seen.size() == 1 && seen[0] == 2, "big update skipped, small one delivered (%zu)", seen.size());

    satellite._rxBuffer.setAllocationCheck(nullptr);
    seen.clear();
    client->simulateData("KEY-STATE DEVICEID=\"" + id + "\" KEY=1 BITMAP=\"" + bigBitmap + "\"\n");
    satellite.loop();
    CHECK(seen.size() == 1 && seen[0] == 1, "big updates get through once memory is back");
}

int main(int argc, char **argv)
{
    g_mockVerbose = argc > 1 && std::string(argv[1]) == "-v";
    setvbuf(stdout, nullptr, _IONBF, 0);

    testNegotiationAndRegistration();
    testKeyStateAndCoalescing();
    testSurfaceMessages();
    testSending();
    testSendBuffering();
    testVersionRejected();
    testKeepaliveAndReconnect();
    testOversizedLine();
    testSubscriptions();
    testLayoutAndLeds();
    testImmediateDispatchAndRemoval();
    testQueueBound();
    testSavedTarget();
    testConfigApi();
    testReceiveOutOfMemory();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
