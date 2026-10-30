// Host-side unit tests for src/satellite-protocol.cpp.
//
// Runs Companion's own conformance corpus (satellite-protocol-corpus.json, copied from
// bitfocus/companion satellite-protocol/assets) plus tests for the client-only parts
// (line buffering, versions/features, base64 and colors). Build and run with `make` in
// this directory; needs a C++17 compiler and ArduinoJson (see the Makefile).

#include "satellite-protocol.hpp"

#include <ArduinoJson.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace satellite_protocol;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                            \
    do                                                              \
    {                                                               \
        checks++;                                                   \
        if (!(cond))                                                \
        {                                                           \
            failures++;                                             \
            printf("FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

static std::string escapeForDisplay(std::string_view s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else
            out += c;
    }
    return out;
}

static Params parse(std::string &buffer)
{
    return Params::parseInPlace(buffer.data(), buffer.size());
}

static void runCorpus(const char *path)
{
    std::ifstream file(path);
    if (!file)
    {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return;
    }
    std::stringstream contents;
    contents << file.rdbuf();

    JsonDocument corpus;
    DeserializationError error = deserializeJson(corpus, contents.str());
    if (error)
    {
        printf("FAIL cannot parse corpus: %s\n", error.c_str());
        failures++;
        return;
    }

    for (JsonObject testCase : corpus["parse"].as<JsonArray>())
    {
        const char *name = testCase["name"];
        std::string line = testCase["line"].as<std::string>();
        Params params = parse(line);
        JsonObject expected = testCase["expected"];

        CHECK(params.size() == expected.size(), "parse '%s': expected %zu params, got %zu", name, expected.size(), params.size());
        for (JsonPair pair : expected)
        {
            const Param *param = params.find(pair.key().c_str());
            CHECK(param != nullptr, "parse '%s': missing key '%s'", name, pair.key().c_str());
            if (!param)
                continue;

            if (pair.value().is<bool>())
            {
                CHECK(param->isFlag, "parse '%s': key '%s' should be a flag", name, pair.key().c_str());
            }
            else
            {
                std::string expectedValue = pair.value().as<std::string>();
                CHECK(!param->isFlag && param->value == expectedValue, "parse '%s': key '%s' expected '%s' got '%s'", name,
                      pair.key().c_str(), escapeForDisplay(expectedValue).c_str(), escapeForDisplay(param->value).c_str());
            }
        }
    }

    for (JsonObject testCase : corpus["format"].as<JsonArray>())
    {
        const char *name = testCase["name"];
        MessageBuilder builder(testCase["message"].as<const char *>(), testCase["status"].isNull() ? "" : testCase["status"].as<const char *>());
        if (!testCase["deviceId"].isNull())
        {
            builder.deviceId(testCase["deviceId"].as<std::string>());
        }
        for (JsonPair arg : testCase["args"].as<JsonObject>())
        {
            if (arg.value().is<bool>())
                builder.boolean(arg.key().c_str(), arg.value().as<bool>());
            else if (arg.value().is<long long>())
                builder.num(arg.key().c_str(), arg.value().as<long long>());
            else
                builder.str(arg.key().c_str(), arg.value().as<std::string>());
        }
        CHECK(builder.ok(), "format '%s': unexpectedly invalid", name);
        std::string line = builder.finish();
        std::string expected = testCase["expected"].as<std::string>();
        CHECK(line == expected, "format '%s': expected '%s' got '%s'", name, escapeForDisplay(expected).c_str(), escapeForDisplay(line).c_str());
    }

    for (JsonObject testCase : corpus["formatErrors"].as<JsonArray>())
    {
        const char *name = testCase["name"];
        MessageBuilder builder("TEST");
        if (!testCase["deviceId"].isNull())
        {
            builder.deviceId(testCase["deviceId"].as<std::string>());
        }
        for (JsonPair arg : testCase["args"].as<JsonObject>())
        {
            builder.str(arg.key().c_str(), arg.value().as<std::string>());
        }
        CHECK(!builder.ok(), "formatErrors '%s': should have been rejected", name);
    }

    for (JsonVariant value : corpus["roundTrip"].as<JsonArray>())
    {
        std::string original = value.as<std::string>();
        std::string line = MessageBuilder("TEST").str("V", original).finish();
        // Strip "TEST " and the trailing " \n" to get just the parameters
        std::string body = line.substr(5, line.size() - 5 - 2);
        Params params = parse(body);
        std::optional<std::string_view> parsed = params.get("V");
        CHECK(parsed.has_value() && *parsed == original, "roundTrip '%s': got '%s'", escapeForDisplay(original).c_str(),
              parsed ? escapeForDisplay(*parsed).c_str() : "<missing>");
    }
}

static void testTruthiness()
{
    std::string line = "A=1 B=0 C=true D=FALSE E= F G=yes H=2 I=0.5 J=abc";
    Params p = parse(line);
    CHECK(p.isTruthy("A"), "A");
    CHECK(p.isFalsey("B") && !p.isTruthy("B"), "B");
    CHECK(p.isTruthy("C"), "C");
    CHECK(p.isFalsey("D"), "D");
    CHECK(p.isFalsey("E"), "E");
    CHECK(p.isTruthy("F"), "flag F");
    CHECK(p.isTruthy("G"), "G");
    CHECK(p.isTruthy("H"), "H");
    CHECK(!p.isTruthy("I") && !p.isFalsey("I"), "0.5 is neither");
    CHECK(!p.isTruthy("J") && !p.isFalsey("J"), "abc is neither");
    CHECK(p.isFalsey("MISSING"), "missing");
}

static void testMessage()
{
    satellite_protocol::Message ok(std::string("ADD-DEVICE OK DEVICEID=\"usat:abc\""));
    CHECK(ok.is("add-device"), "case-insensitive command");
    CHECK(ok.isOk() && !ok.isError(), "OK status");
    CHECK(ok.params().get("DEVICEID") == std::string_view("usat:abc"), "device id");

    satellite_protocol::Message error(std::string("KEY-PRESS ERROR DEVICEID=\"x\" MESSAGE=\"Device \\\"x\\\" not found\""));
    CHECK(error.isError(), "ERROR status");
    CHECK(error.params().get("MESSAGE") == std::string_view("Device \"x\" not found"), "escaped message");

    satellite_protocol::Message bare(std::string("PING"));
    CHECK(bare.is("PING") && bare.params().size() == 0, "bare command");

    satellite_protocol::Message repeated(std::string("X A=1 A=2"));
    CHECK(repeated.params().get("A") == std::string_view("2"), "last repeated key wins");
}

static void testBuilder()
{
    std::string line = MessageBuilder("KEY-ROTATE").deviceId("usat:1").num("KEY", 3).num("DIRECTION", -4).finish();
    CHECK(line == "KEY-ROTATE DEVICEID=\"usat:1\" KEY=3 DIRECTION=-4 \n", "%s", escapeForDisplay(line).c_str());

    MessageBuilder rawBad("X");
    rawBad.raw("A", "has space");
    CHECK(!rawBad.ok(), "raw with space rejected");

    std::string rawGood = MessageBuilder("X").raw("BITMAPS", "false").finish();
    CHECK(rawGood == "X BITMAPS=false \n", "%s", escapeForDisplay(rawGood).c_str());
}

static void testLineBuffer()
{
    LineBuffer buffer(16);
    std::vector<std::string> lines;
    auto collect = [&](std::string &&line)
    { lines.push_back(std::move(line)); };

    size_t dropped = buffer.push("AB", 2, collect);
    dropped += buffer.push("C\r\nDE\nF", 7, collect);
    CHECK(dropped == 0, "nothing dropped");
    CHECK(lines.size() == 2 && lines[0] == "ABC" && lines[1] == "DE", "split lines, CR stripped");

    lines.clear();
    dropped = buffer.push("\n", 1, collect);
    CHECK(lines.size() == 1 && lines[0] == "F", "partial line completed");

    // An oversized line is skipped, and parsing resumes after its newline
    lines.clear();
    std::string longLine(40, 'x');
    dropped = buffer.push(longLine.data(), 10, collect);
    dropped += buffer.push(longLine.data(), 10, collect);
    dropped += buffer.push(longLine.data(), 20, collect);
    dropped += buffer.push("yy\nOK\n", 6, collect);
    CHECK(dropped == 1, "one line dropped, got %zu", dropped);
    CHECK(lines.size() == 1 && lines[0] == "OK", "resynchronised after oversized line");

    // Oversized within a single chunk
    lines.clear();
    std::string chunk = std::string(30, 'z') + "\nNEXT\n";
    dropped = buffer.push(chunk.data(), chunk.size(), collect);
    CHECK(dropped == 1 && lines.size() == 1 && lines[0] == "NEXT", "oversized in one chunk");

    // Large lines are moved out rather than copied, and the buffer keeps working after
    LineBuffer big(100000);
    std::string bitmapLine = "KEY-STATE BITMAP=" + std::string(20000, 'A') + "\nPONG\n";
    lines.clear();
    big.push(bitmapLine.data(), bitmapLine.size(), collect);
    CHECK(lines.size() == 2 && lines[0].size() == 17 + 20000 && lines[1] == "PONG", "large line");

    // Growth is capped at the line limit rather than doubling past it
    LineBuffer capped(30000);
    lines.clear();
    capped.push(bitmapLine.data(), bitmapLine.size(), collect);
    CHECK(lines.size() == 2 && lines[0].capacity() <= 30000, "buffer growth capped at maxLineLength (capacity %zu)", lines[0].capacity());

    // When the memory isn't there, the line is skipped (the device would otherwise abort) and
    // parsing carries on after it. Fed in TCP-segment-sized chunks, like the real thing.
    static size_t allocationLimit;
    LineBuffer guarded(100000);
    guarded.setAllocationCheck([](size_t bytes)
                               { return bytes <= allocationLimit; });
    allocationLimit = 8192;
    lines.clear();
    std::string burst = bitmapLine + "PING x\n";
    size_t droppedTotal = 0;
    for (size_t offset = 0; offset < burst.size(); offset += 1436)
    {
        droppedTotal += guarded.push(burst.data() + offset, std::min<size_t>(1436, burst.size() - offset), collect);
    }
    CHECK(droppedTotal == 1 && guarded.takeOutOfMemoryDrops() == 1, "line skipped for lack of memory");
    CHECK(guarded.takeOutOfMemoryDrops() == 0, "out-of-memory count resets");
    CHECK(lines.size() == 2 && lines[0] == "PONG" && lines[1] == "PING x", "resynchronised after the skipped line");

    allocationLimit = 100000;
    lines.clear();
    guarded.push(bitmapLine.data(), bitmapLine.size(), collect);
    CHECK(lines.size() == 2 && lines[0].size() == 17 + 20000, "large lines get through again once memory is back");

    // The buffer never ends up bigger than what was checked (libstdc++'s reserve() rounds up
    // to double the old capacity, which once made the check pass and the allocation abort)
    static size_t largestChecked;
    LineBuffer exact(30000);
    exact.setAllocationCheck([](size_t bytes)
                             { largestChecked = std::max(largestChecked, bytes); return true; });
    largestChecked = 0;
    lines.clear();
    // Long enough that the last step is capped at the line limit, below double the old capacity
    std::string nearLimit = "KEY-STATE BITMAP=" + std::string(25000, 'A') + "\nPONG\n";
    for (size_t offset = 0; offset < nearLimit.size(); offset += 1436)
    {
        exact.push(nearLimit.data() + offset, std::min<size_t>(1436, nearLimit.size() - offset), collect);
    }
    CHECK(lines.size() == 2 && lines[0].capacity() + 1 <= largestChecked + 16,
          "allocations stay within what was checked (capacity %zu, checked %zu)", lines[0].capacity(), largestChecked);
}

static void testVersions()
{
    auto v = ApiVersion::parse("1.15.0");
    CHECK(v && v->major == 1 && v->minor == 15 && v->patch == 0, "parse 1.15.0");
    CHECK(ApiVersion::parse(" 1.2.3-beta.1 ").has_value(), "prerelease suffix");
    CHECK(ApiVersion::parse("1.2.3+build").has_value(), "build suffix");
    CHECK(!ApiVersion::parse("1.2").has_value(), "missing patch rejected");
    CHECK(!ApiVersion::parse("1.2.3x").has_value(), "junk suffix rejected");
    CHECK(!ApiVersion::parse("").has_value(), "empty rejected");
    CHECK(ApiVersion::parse("1.10.0")->atLeast(*ApiVersion::parse("1.9.9")), "1.10.0 >= 1.9.9");
    CHECK(!ApiVersion::parse("1.9.9")->atLeast(*ApiVersion::parse("1.10.0")), "1.9.9 < 1.10.0");

    std::string capsLine = "SUBSCRIPTIONS=0 NONSQUARE=1 BITMAP_FORMATS=\"rgb,png,webp\" ROTARY_AMOUNT=1";
    Params caps = parse(capsLine);
    ApiVersion v115 = *ApiVersion::parse("1.15.0");
    ApiVersion v113 = *ApiVersion::parse("1.13.0");

    CHECK(!isFeatureEnabled(Feature::Subscriptions, v115, &caps), "subscriptions disabled by CAPS");
    CHECK(isFeatureEnabled(Feature::NonSquareBitmaps, v115, &caps), "nonsquare");
    CHECK(isFeatureEnabled(Feature::RotaryAmount, v115, &caps), "rotary amount");
    CHECK(!isFeatureEnabled(Feature::RotaryAmount, v113, &caps), "rotary amount needs 1.14");
    CHECK(!isFeatureEnabled(Feature::NonSquareBitmaps, v115, nullptr), "no caps yet");
    CHECK(isFeatureEnabled(Feature::Leds, v113, nullptr), "leds has no caps flag");
    CHECK(!isFeatureEnabled(Feature::Appearance, v113, nullptr), "appearance needs 1.15");
}

static void testHelpers()
{
    auto c = parseColor("#ff8000");
    CHECK(c && c->r == 255 && c->g == 128 && c->b == 0, "hex color");
    c = parseColor("rgb(1, 2,300)");
    CHECK(c && c->r == 1 && c->g == 2 && c->b == 255, "rgb color clamped");
    CHECK(!parseColor("#ff80").has_value(), "short hex rejected");
    CHECK(!parseColor("rgb(1,2)").has_value(), "two components rejected");
    CHECK(!parseColor("#gg0000").has_value(), "bad hex rejected");

    CHECK(base64Encode(std::string_view("hello")) == "aGVsbG8=", "encode");
    CHECK(base64Encode(std::string_view("")) == "", "encode empty");
    CHECK(base64Decode("aGVsbG8=") == std::optional<std::string>("hello"), "decode");
    CHECK(base64Decode("aGVsbG8") == std::optional<std::string>("hello"), "decode unpadded");
    CHECK(!base64Decode("aGV*bG8=").has_value(), "decode rejects junk");
    uint8_t small[2];
    CHECK(!base64Decode("aGVsbG8=", small, sizeof(small)).has_value(), "decode rejects overflow");
    CHECK(base64DecodedLength("aGVsbG8=") == 5, "decoded length");

    // The decoder takes whole 4-character groups at a time for speed; it must behave exactly
    // like the plain character-at-a-time decoder, on junk, padding and full buffers too
    auto reference = [](std::string_view input, uint8_t *out, size_t capacity) -> std::optional<size_t>
    {
        auto value = [](char c)
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+' || c == '-')
                return 62;
            if (c == '/' || c == '_')
                return 63;
            return -1;
        };
        uint32_t accumulator = 0;
        int bits = 0;
        size_t written = 0;
        bool padding = false;
        for (char c : input)
        {
            if (c == '=')
            {
                padding = true;
                continue;
            }
            int v = value(c);
            if (v < 0 || padding)
                return std::nullopt;
            accumulator = (accumulator << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                if (written >= capacity)
                    return std::nullopt;
                out[written++] = static_cast<uint8_t>(accumulator >> bits);
            }
        }
        return written;
    };
    const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_";
    uint32_t seed = 12345;
    auto random = [&seed]()
    { seed = seed * 1103515245 + 12345; return (seed >> 8) & 0xffff; };
    int mismatches = 0;
    for (int round = 0; round < 3000; round++)
    {
        std::string input;
        size_t length = random() % 40;
        for (size_t i = 0; i < length; i++)
        {
            uint32_t r = random() % 100;
            input += r < 90 ? kAlphabet[random() % (sizeof(kAlphabet) - 1)] : (r < 95 ? '=' : static_cast<char>(random() % 256));
        }
        size_t capacity = random() % 32;
        uint8_t expected[32], actual[32];
        std::optional<size_t> want = reference(input, expected, capacity);
        std::optional<size_t> got = base64Decode(input, actual, capacity);
        if (want != got || (want && memcmp(expected, actual, *want) != 0))
        {
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "fast decoder matches the reference (%d mismatches)", mismatches);
}

int main(int argc, char **argv)
{
    const char *corpusPath = argc > 1 ? argv[1] : "satellite-protocol-corpus.json";
    setvbuf(stdout, nullptr, _IONBF, 0);

    runCorpus(corpusPath);
    testTruthiness();
    testMessage();
    testBuilder();
    testLineBuffer();
    testVersions();
    testHelpers();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
