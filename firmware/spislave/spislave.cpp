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
uint8_t rxbyte[4];
uint16_t txbyte;

uint8_t last_cmd = 0;

inline void select_columns(uint8_t w8)
{
    gpio_put_masked(0xff00, w8 << 8);
}

// Boneheaded SPI slave on rp2040 doesn't allow more than one word
// per CS assertion, so we must be smart and push everything in 16-bit frame
//
// The known workaround for this is to use SPI mode 3, but it has drawbacks.
void __time_critical_func(reset_spi)()
{
    spi_init(SPI_PORT, 1000*1000);
    spi_set_format(SPI_PORT, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST); 
    spi_set_slave(SPI_PORT, true);
}

// Because rp2040 spi slave is completely broken and it hogs MISO wire
// rely on MISO function switch in the interrupt. this introduces
// a delay between CS and the first bit readable by master.
//
// Luckily Espressif engineers are not forked in the head and there is
// a beautiful feature that lets compensate for for this nonsense,
// spi_device_interface_config_t.cs_ena_pretrans lets CS to set up before
// transmission starts, so we're golden.
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
    txbyte = (modkeys << 8) | rows;
    
    spi_get_hw(SPI_PORT)->dr = (uint32_t)txbyte; // prime up tx buffer
                                                 //
    while (!spi_is_readable(SPI_PORT)) 
        tight_loop_contents();
    rxbyte[2] = gpio_get(PIN_CS);

    *(uint16_t *)(&rxbyte[0]) = spi_get_hw(SPI_PORT)->dr;

    last_cmd = rxbyte[1];
    switch (last_cmd) {
        case 0xe5:
            // column select
            select_columns(rxbyte[0]);
            break;
        case 0xe6:
            // send rows (primed)
            break;
        case 0xe7:
            // read modkeys (primed)
            break;
        case 0xe8:
            // set rus/lat
            gpio_put(PIN_INDRUS, (rxbyte[0] >> PC_BIT_INDRUS) & 1);
            break;
    }
    rxbyte[3] = gpio_get(PIN_CS);

    for (size_t i = 0; i < 4; ++i) {
        if (debug_i < debug.size()) {
            debug[debug_i++] = rxbyte[i];
        }
    }
}


// status reg is found once in prepare_soup()
static io_ro_32 *status_reg_cs;

void prepare_soup()
{
    io_irq_ctrl_hw_t *irq_ctrl_base = get_core_num() ? &iobank0_hw->proc1_irq_ctrl : &iobank0_hw->proc0_irq_ctrl;
    status_reg_cs = &irq_ctrl_base->ints[PIN_CS / 8];
}

void __time_critical_func(miso_soup)(void)
{
    // see prepare_soup()
    // io_irq_ctrl_hw_t *irq_ctrl_base = get_core_num() ? &iobank0_hw->proc1_irq_ctrl : &iobank0_hw->proc0_irq_ctrl;
    // io_ro_32 *status_reg_cs = &irq_ctrl_base->ints[PIN_CS / 8];

    uint events = (*status_reg_cs >> 4 * (PIN_CS % 8)) & 0xf;
    if (events) {
        if (events & GPIO_IRQ_EDGE_FALL) {
            gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
        }
        else if (events & GPIO_IRQ_EDGE_RISE) {
            gpio_set_function(PIN_MISO, GPIO_FUNC_SIO);
        }
        gpio_acknowledge_irq(PIN_CS, events);
    }
}

void __time_critical_func(core1_task)()
{

    reset_spi();

    for(;;) {
        spi_exchange();

        // if we receive strange crap, fix it with a reset
        if (last_cmd != 0xe5 && last_cmd != 0xe6 && last_cmd != 0xe7 && last_cmd != 0xe8) {
            printf("last_cmd=%02x, reset\n", last_cmd);
            reset_spi();
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
    gpio_set_function(PIN_MISO, GPIO_FUNC_SIO);
    //gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);


    // manually switch off MISO
    gpio_set_irq_enabled(PIN_CS,  GPIO_IRQ_EDGE_FALL,true);
    gpio_set_irq_enabled(PIN_CS,  GPIO_IRQ_EDGE_RISE, true);
    irq_set_exclusive_handler(IO_IRQ_BANK0, miso_soup);

    prepare_soup();
    irq_set_enabled(IO_IRQ_BANK0, true);

    reset_spi();

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
