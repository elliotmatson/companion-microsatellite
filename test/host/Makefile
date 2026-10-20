# Host-side tests, run on a desktop machine rather than an ESP32.
#
#   make            # fetches ArduinoJson on first run, builds and runs everything
#   make ARDUINOJSON=/path/to/ArduinoJson/src
#
# - test_protocol: the codec (src/satellite-protocol.cpp), including Companion's own
#   conformance corpus
# - test_satellite: Satellite/SatelliteSurface/SatelliteSubscription against stand-ins
#   for the Arduino core and AsyncTCP (mock/), playing Companion's side of the protocol
#
# satellite-protocol-corpus.json is copied from bitfocus/companion
# (satellite-protocol/assets). To refresh it:
#   make update-corpus

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-maybe-uninitialized -Wno-missing-field-initializers -fsanitize=address,undefined -fno-sanitize-recover=all
ARDUINOJSON ?= build/ArduinoJson/src
CORPUS_URL = https://raw.githubusercontent.com/bitfocus/companion/main/satellite-protocol/assets/satellite-protocol-corpus.json

SRC_DIR = ../../src
PROTOCOL_SRC = $(SRC_DIR)/satellite-protocol.cpp
LIBRARY_SRC = $(PROTOCOL_SRC) $(SRC_DIR)/companion-satellite.cpp $(SRC_DIR)/companion-surface.cpp $(SRC_DIR)/companion-subscription.cpp
HEADERS = $(wildcard $(SRC_DIR)/*.hpp) $(wildcard mock/*.h)

# The mocks stand in for the Arduino String class, so let ArduinoJson use it
MOCK_FLAGS = -Imock -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 \
	-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0

.PHONY: test clean update-corpus

test: build/test_protocol build/test_satellite
	./build/test_protocol satellite-protocol-corpus.json
	LSAN_OPTIONS=suppressions=lsan.supp:print_suppressions=0 ./build/test_satellite

build/test_protocol: $(PROTOCOL_SRC) test_protocol.cpp $(HEADERS) | $(ARDUINOJSON)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) -isystem $(ARDUINOJSON) $(PROTOCOL_SRC) test_protocol.cpp -o $@

build/test_satellite: $(LIBRARY_SRC) test_satellite.cpp $(HEADERS) | $(ARDUINOJSON)
	$(CXX) $(CXXFLAGS) $(MOCK_FLAGS) -I$(SRC_DIR) -isystem $(ARDUINOJSON) $(LIBRARY_SRC) test_satellite.cpp -o $@

build/ArduinoJson/src:
	mkdir -p build
	git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson.git build/ArduinoJson

update-corpus:
	curl -fsSL -o satellite-protocol-corpus.json $(CORPUS_URL)

clean:
	rm -rf build/test_protocol build/test_satellite
