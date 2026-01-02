#include "ps2lib.h"
#include "scancodes.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "ps2lib.pio.h"

static PIO pio;
static uint sm;
static uint offset;

#define RING_BUFFER_SIZE 64
static uint8_t ring_buffer[RING_BUFFER_SIZE];
static volatile uint8_t head = 0;
static volatile uint8_t tail = 0;
static volatile bool is_sending = false;

static bool calculate_odd_parity(uint8_t byte)
{
    uint8_t count = 0;
    for (int i = 0; i < 8; i++)
    {
        if (byte & (1 << i))
            count++;
    }
    return (count & 1) ? 0 : 1;
}

static void process_send_queue()
{
    uint32_t save = save_and_disable_interrupts();

    if (head == tail)
    {
        is_sending = false;
        restore_interrupts(save);
        return;
    }

    if (!pio_sm_is_tx_fifo_full(pio, sm))
    {
        uint8_t byte = ring_buffer[tail];
        tail = (tail + 1) % RING_BUFFER_SIZE;

        bool parity = calculate_odd_parity(byte);
        uint32_t frame = ((uint32_t)byte << 1) | ((uint32_t)parity << 9) | (1 << 10);
        pio_sm_put(pio, sm, frame);
    }

    restore_interrupts(save);
}

void ps2lib_init()
{
    // Use PIO1 to avoid conflict with USB PIO (which uses PIO0)
    pio = pio1;
    offset = pio_add_program(pio, &ps2lib_program);
    sm = pio_claim_unused_sm(pio, true);

    pio_sm_config config = ps2lib_program_get_default_config(offset);

    // Configure pins
    sm_config_set_sideset_pins(&config, PS2_CLK_PIN);
    sm_config_set_out_pins(&config, PS2_DATA_PIN, 1);
    pio_gpio_init(pio, PS2_CLK_PIN);
    pio_gpio_init(pio, PS2_DATA_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, PS2_CLK_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PS2_DATA_PIN, 1, true);

    // Initial state
    gpio_put(PS2_CLK_PIN, 1);
    gpio_put(PS2_DATA_PIN, 1);

    // Clock divider (10kHz target)
    float div = (float)clock_get_hz(clk_sys) / (10000.0f * 17.0f);
    sm_config_set_clkdiv(&config, div);

    // Configure shifts
    sm_config_set_out_shift(&config, true, true, 11);

    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_enabled(pio, sm, true);
}

void ps2lib_send_byte(uint8_t byte)
{
    uint32_t save = save_and_disable_interrupts();
    uint8_t next_head = (head + 1) % RING_BUFFER_SIZE;
    if (next_head != tail)
    {
        ring_buffer[head] = byte;
        head = next_head;
        if (!is_sending)
            is_sending = true;
        process_send_queue();
    }
    restore_interrupts(save);
}

void ps2lib_press_key(uint8_t key)
{
    ps2lib_send_byte(key);
}

void ps2lib_release_key(uint8_t key)
{
    ps2lib_send_byte(PS2_KEY_BREAK);
    ps2lib_send_byte(key);
}

void ps2lib_press_extended(uint16_t key)
{
    if (key & 0xFF00)
    {
        ps2lib_send_byte((key >> 8) & 0xFF);
    }
    ps2lib_send_byte(key & 0xFF);
}

void ps2lib_release_extended(uint16_t key)
{
    if (key & 0xFF00)
    {
        ps2lib_send_byte((key >> 8) & 0xFF);
        ps2lib_send_byte(PS2_KEY_BREAK);
    }
    ps2lib_send_byte(PS2_KEY_BREAK);
    ps2lib_send_byte(key & 0xFF);
}

void ps2lib_send_combo(uint8_t modifier, uint8_t key)
{
    ps2lib_press_key(modifier);
    ps2lib_press_key(key);
    ps2lib_release_key(key);
    ps2lib_release_key(modifier);
}

void ps2lib_send_extended_combo(uint16_t modifier, uint8_t key)
{
    ps2lib_press_extended(modifier);
    ps2lib_press_key(key);
    ps2lib_release_key(key);
    ps2lib_release_extended(modifier);
}

bool ps2lib_is_busy()
{
    return is_sending;
}

void ps2lib_poll()
{
    if (is_sending)
        process_send_queue();
}
