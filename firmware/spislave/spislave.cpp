#include <cstdio>
#include <time.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "pico/multicore.h"

#include <array>

#define SPI_PORT spi0
#define PIN_MOSI 16 // slave input
#define PIN_CS   17 // select (input)
#define PIN_SCK  18 // clock
#define PIN_MISO 19 // slave output

#define MATRIX_ROW_PINS {0, 1, 2, 3, 4, 5, 6, 7}
#define MATRIX_COL_PINS {8, 9, 10, 11, 12, 13, 14, 15}

#define PIN_VVOD    28
#define PIN_SBROS   27

#define MODKEYS_LSB 20
#define PIN_SS      20 // PC5
#define PIN_US      21 // PC6
#define PIN_RUSLAT  22 // PC7
#define PIN_INDRUS  26 // LED output

#define PIN_MODKEYS_MASK  ((1<<(PIN_SS))|(1<<(PIN_US))|(1<<(PIN_RUSLAT))|(1<<(PIN_VVOD))|(1<<(PIN_SBROS)))
#define PIN_INDRUS_MASK (1<<(PIN_INDRUS))

constexpr int row_pins[8] = MATRIX_ROW_PINS;
constexpr int col_pins[8] = MATRIX_COL_PINS;

// modkey bits for portc
constexpr uint8_t PC_MODKEYS_LSB = 5;
constexpr uint8_t PC_BIT_SS = (1<<5);
constexpr uint8_t PC_BIT_US = (1<<6);
constexpr uint8_t PC_BIT_RUSLAT = (1<<7);
constexpr uint8_t PC_MODKEYS_MASK = (PC_BIT_SS | PC_BIT_US | PC_BIT_RUSLAT);

constexpr uint8_t PC_BIT_INDRUS = 3;
constexpr uint8_t PC_BIT_INDRUS_MASK = 1 << PC_BIT_INDRUS;

std::array<uint8_t, 256> debug;
volatile size_t debug_i = 0;
uint8_t txbyte, rxbyte[4];

inline void select_columns(uint8_t w8)
{
    gpio_put_masked(0xff00, w8 << 8);
}

void __time_critical_func(spi_exchange)()
{
    // prepare data
    uint32_t all = gpio_get_all();
    uint8_t rows = all & 0xff;

    // RUSLAT|US|SS|000|SBROS|VVOD
    uint8_t modkeys = ((all >> MODKEYS_LSB) & 7) << PC_MODKEYS_LSB;
    modkeys |= (1 & (all >> PIN_VVOD)) << 0;
    modkeys |= (1 & (all >> PIN_SBROS)) << 1;

    // 0: read command
    txbyte = 0;
    spi_write_read_blocking(SPI_PORT, &txbyte, &rxbyte[0], 1);

    //printf("%02x ", rxbyte[0]);

    // 1: send response
    switch (rxbyte[0]) {
        case 0xe5:
            // read column select
            spi_write_read_blocking(SPI_PORT, &txbyte, &rxbyte[1], 1);
            select_columns(rxbyte[1]);
            break;
        case 0xe6:
            // send rows
            spi_write_read_blocking(SPI_PORT, &rows, &rxbyte[1], 1);
            break;
        case 0xe7:
            // read modkeys
            spi_write_read_blocking(SPI_PORT, &modkeys, &rxbyte[1], 1);
            break;
        case 0xe8:
            // set rus/lat
            spi_write_read_blocking(SPI_PORT, &txbyte, &rxbyte[1], 1);
            gpio_put(PIN_INDRUS, (rxbyte[1] >> PC_BIT_INDRUS) & 1);
            break;
    }

    //putchar('2');

    // 3: flush response
    spi_write_read_blocking(SPI_PORT, &txbyte, &rxbyte[2], 1);
    //while ((spi_get_hw(SPI_PORT)->sr & SPI_SSPSR_TFE_BITS) == 0) // while transmit fifo not empty
    //    tight_loop_contents();
    //while (spi_is_readable(SPI_PORT)) {
    //    rxbyte[3] = (uint8_t)spi_get_hw(SPI_PORT)->dr;
    //}


    rxbyte[3] = 0x33;
    for (size_t i = 0; i < 4; ++i) {
        if (debug_i < debug.size()) {
            debug[debug_i++] = rxbyte[i];
        }
    }
}

void core1_task()
{
    spi_init(SPI_PORT, 6000*1000);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST); 
    spi_set_slave(SPI_PORT, true);
    for(;;) {
        spi_exchange();
        // if we receive strange crap, fix it with a reset
        if (rxbyte[0] != 0xe5 && rxbyte[0] != 0xe6 && rxbyte[0] != 0xe7 && rxbyte[0] != 0xe8) {
            spi_init(SPI_PORT, 1000*1000);
            spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST); 
            spi_set_slave(SPI_PORT, true);
        }
    }
}


int main()
{
    stdio_init_all();
    sleep_us(4000000);
    printf("v06c-mini-keyboard\n");

    // bits 8..15 are columns/outputs
    // bits 0..7 are rows/inputs
    // modkeys are separate inputs
    // indrus is rus led output (0 = on)
    gpio_init_mask(0xffff | PIN_MODKEYS_MASK | PIN_INDRUS_MASK);
    for (int i = 0; i < 8; ++i) {
        gpio_set_dir(i, GPIO_IN);
        gpio_pull_down(i);

        gpio_set_dir(i + 8, GPIO_OUT);
        gpio_put(i + 8, 1);
        gpio_set_drive_strength(i + 8, GPIO_DRIVE_STRENGTH_12MA);
    }

    // modkeys
    gpio_set_dir_in_masked(PIN_MODKEYS_MASK);
    gpio_set_dir(PIN_INDRUS, GPIO_OUT);

    // indrus
    gpio_put(PIN_INDRUS, 1); // off


    spi_init(SPI_PORT, 6000*1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // THIS LINE IS ABSOLUTELY KEY. Enables multi-byte transfers with one CS assert
    // Page 537 of the RP2040 Datasheet.
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST); 
    spi_set_slave(SPI_PORT, true);

    gpio_set_dir(PIN_CS, GPIO_IN);
    gpio_disable_pulls(PIN_CS);


    multicore_launch_core1(core1_task);

    // core0 has nothing to do, but we can print useful debug info
    for(;;) {
        if (debug_i == debug.size()) {
            ++debug_i;
            for (int i = 0; i < debug.size(); i += 4) {
                for (int n = 0; n < 4; ++n) {
                    printf("%02x ", debug[i + n]);
                }
                printf("\n");
            }
        }
    }

    return 0;
}
