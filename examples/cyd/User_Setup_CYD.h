// TFT_eSPI setup for the ESP32-2432S028R "Cheap Yellow Display" (resistive
// touch, ILI9341 panel). See the top of cyd.ino for how to install it - it
// has to reach the TFT_eSPI library's own code, not just the sketch.

#define USER_SETUP_LOADED 1

// Some CYD batches look better with the alternative init sequence: swap this
// for ILI9341_2_DRIVER if colours are off.
#define ILI9341_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 320

// Display SPI bus (the touch controller is on a separate bus, see cyd.ino)
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH
#define USE_HSPI_PORT

#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000

#define LOAD_GLCD
