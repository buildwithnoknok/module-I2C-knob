/*
 * noknok Knob Module Firmware  v1.0
 * CH32V003J4M6 (SOP-8)  |  Stack: cnlohr/ch32fun
 *
 * ── Hardware ─────────────────────────────────────────────────────────────
 *   PA1 (pin 2)  Encoder A (CLK)
 *   PA2 (pin 1)  Encoder B (DT)
 *   PD6 (pin 7)  Encoder push button (active LOW, internal pull-up)
 *   PC1 (pin 3)  I2C SDA
 *   PC2 (pin 4)  I2C SCL
 *
 * ── Enumeration protocol ─────────────────────────────────────────────────
 *   Identical to noknok Buzzer v3.1.
 *   Staging address : 0x7F
 *   Runtime address : assigned by Conductor (0x08–0x77)
 *
 *   1. Conductor reads 10 bytes from 0x7F:
 *        [UID 0..7]   64-bit hardware UID
 *        [0x02]       MODULE_TYPE (knob)
 *        [CRC8]       CRC8 of bytes 0–8
 *
 *   2. Conductor writes to 0x7F:
 *        [0x1D, new_addr]   ASSIGN — module switches to new_addr
 *
 * ── Normal operation protocol ────────────────────────────────────────────
 *   I2C address: assigned at runtime
 *
 *   Pico READS 4 bytes:
 *     [posH, posL]   signed 16-bit position, big-endian
 *     [delta]        signed 8-bit change since last read (auto-cleared)
 *     [btn_state]    0x01 if button currently pressed, 0x00 if released
 *
 *   Pico WRITES:
 *     [0x10]              RESET — set position to 0
 *     [0x11, posH, posL]  SET POSITION — set position to given value
 */

#include "ch32fun.h"
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ENUM_ADDR       0x7F
#define MODULE_TYPE     0x02    /* 0x02 = Rotary Encoder / Knob */
#define REG_ASSIGN_ADDR 0x1D
#define CMD_RESET_POS   0x10
#define CMD_SET_POS     0x11

#define UID_ADDR        ((volatile uint8_t*)0x1FFFF7E8)
#define UID_LEN         8

/* Button debounce and rotation-lockout times in ms */
#define BTN_DEBOUNCE_MS         50
#define BTN_ROTATION_LOCKOUT_MS 150


/* ═══════════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DEV_BOOT_WAITING,
    DEV_ENUM_READY,
    DEV_ASSIGNING,
    DEV_ASSIGNED,
} DeviceState;

static volatile DeviceState dev_state = DEV_BOOT_WAITING;
static volatile uint32_t    ms_tick   = 0;
static volatile uint8_t     new_addr  = 0;

/* Encoder / button */
static volatile int16_t  enc_position = 0;
static volatile int8_t   enc_delta    = 0;   /* cleared when Pico reads */
static volatile uint8_t  btn_state    = 0;   /* 1=pressed, 0=released */
static uint8_t  btn_raw_last = 0;
static uint32_t btn_last_change_ms = 0;

/* Encoder Gray-code state — lower 2 bits = [A, B] last reading */
static uint8_t enc_last_ab = 0;

/* Timestamp of last detected rotation — used to lock out button during turns */
static uint32_t last_rotation_ms = 0;

/* Lookup table: enc_table[prev<<2 | curr] → +1, -1, or 0 */
static const int8_t enc_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

/* I2C receive */
#define RX_BUF_SIZE 8
static volatile uint8_t rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_len    = 0;
static volatile uint8_t cmd_ready = 0;

/* I2C transmit */
#define TX_BUF_SIZE 10
static volatile uint8_t tx_buf[TX_BUF_SIZE];
static volatile uint8_t tx_len = 0;
static volatile uint8_t tx_idx = 0;

/* Flag: delta was sent to Pico — clear it in main loop */
static volatile uint8_t delta_sent = 0;

/* Backoff */
static uint32_t backoff_ms;
static uint32_t enum_ready_start_ms = 0;


/* ═══════════════════════════════════════════════════════════════════════════
 * CRC8  (polynomial 0x07)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * BACKOFF TIMER  —  proper FNV-1a hash of 8 UID bytes
 *   Identical algorithm to Buzzer v3.1.
 *   calc_backoff()   : range 300–2799 ms
 *   calc_rebackoff() : range 50–549 ms (short, for collision recovery)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t fnv_hash(uint32_t h)
{
    volatile uint8_t *uid = UID_ADDR;
    for (uint8_t i = 0; i < UID_LEN; i++) {
        h ^= uid[i];
        h *= 16777619UL;
    }
    return h;
}

static void calc_backoff(void)
{
    backoff_ms = (fnv_hash(2166136261UL) % 2500) + 300;
}

static uint32_t calc_rebackoff_ms(void)
{
    return ms_tick + (fnv_hash(ms_tick) % 500) + 50;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * BUILD UID RESPONSE  —  10 bytes: [UID 8 bytes] + [MODULE_TYPE] + [CRC8]
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_uid_response(void)
{
    volatile uint8_t *uid = UID_ADDR;
    for (uint8_t i = 0; i < UID_LEN; i++)
        tx_buf[i] = uid[i];
    tx_buf[8] = MODULE_TYPE;
    tx_buf[9] = crc8((const uint8_t*)tx_buf, 9);
    tx_len = 10;
    tx_idx = 0;
}

/* ── Build 4-byte data response for DEV_ASSIGNED reads ─────────────────────
 *   [posH, posL, delta, btn_state]
 *   Sets delta_sent so main loop can clear enc_delta after this is read.
 */
static void build_data_response(void)
{
    int16_t pos = enc_position;
    int8_t  dlt = enc_delta;
    tx_buf[0] = (uint8_t)((pos >> 8) & 0xFF);
    tx_buf[1] = (uint8_t)(pos & 0xFF);
    tx_buf[2] = (uint8_t)dlt;
    tx_buf[3] = btn_state;
    tx_len = 4;
    tx_idx = 0;
    delta_sent = 1;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * GPIO INIT  —  encoder inputs and button
 * ═══════════════════════════════════════════════════════════════════════════ */

static void encoder_gpio_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOD;

    /* PA1, PA2: input with pull-up (CNF=10, MODE=00) */
    GPIOA->CFGLR &= ~((0xF << (1 * 4)) | (0xF << (2 * 4)));
    GPIOA->CFGLR |=  ((0x8 << (1 * 4)) | (0x8 << (2 * 4)));
    GPIOA->BSHR   =  (1 << 1) | (1 << 2);   /* enable pull-ups */

    /* PD6: input with pull-up (button, active LOW) */
    GPIOD->CFGLR &= ~(0xF << (6 * 4));
    GPIOD->CFGLR |=  (0x8 << (6 * 4));
    GPIOD->BSHR   =  (1 << 6);              /* enable pull-up */
}

/* Read encoder A and B as 2-bit value [A=bit1, B=bit0] */
static inline uint8_t read_enc_ab(void)
{
    uint8_t a = (GPIOA->INDR >> 1) & 1;
    uint8_t b = (GPIOA->INDR >> 2) & 1;
    return (a << 1) | b;
}

/* Read button: 1=pressed (pin LOW), 0=released */
static inline uint8_t read_btn_raw(void)
{
    return ((GPIOD->INDR >> 6) & 1) ? 0 : 1;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * TIM2  —  1 ms tick
 * ═══════════════════════════════════════════════════════════════════════════ */

static void tim2_init(void)
{
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
    TIM2->PSC      = 47;
    TIM2->ATRLR    = 999;
    TIM2->DMAINTENR = TIM_IT_Update;
    TIM2->SWEVGR   = TIM_UG;
    TIM2->CTLR1   |= TIM_CEN;
    NVIC_EnableIRQ(TIM2_IRQn);
}

void TIM2_IRQHandler(void) __attribute__((interrupt));
void TIM2_IRQHandler(void) { TIM2->INTFR = 0; ms_tick++; }


/* ═══════════════════════════════════════════════════════════════════════════
 * I2C SLAVE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void i2c_slave_init(uint8_t addr)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

    GPIOC->CFGLR &= ~(0xF << (1 * 4)); GPIOC->CFGLR |= (0xF << (1 * 4));
    GPIOC->CFGLR &= ~(0xF << (2 * 4)); GPIOC->CFGLR |= (0xF << (2 * 4));

    I2C1->CTLR1 |=  I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    I2C1->CTLR2  = 48;
    I2C1->CKCFGR = 240;
    I2C1->OADDR1 = ((uint16_t)addr << 1);

    I2C1->CTLR2 |= I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITERREN;
    I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_PE;

    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
}

static void i2c_switch_addr(uint8_t addr)
{
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    I2C1->OADDR1 = ((uint16_t)addr << 1);
    I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_PE;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * I2C EVENT ISR
 * ═══════════════════════════════════════════════════════════════════════════ */

void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void)
{
    uint32_t star1 = I2C1->STAR1;
    uint32_t star2 = I2C1->STAR2;

    if (star1 & I2C_STAR1_ADDR)
    {
        rx_len = 0;
        I2C1->CTLR1 |= I2C_CTLR1_ACK;

        if (star2 & I2C_STAR2_TRA)
        {
            if (dev_state == DEV_ENUM_READY)
            {
                build_uid_response();
                I2C1->DATAR = tx_buf[tx_idx++];
            }
            else
            {
                build_data_response();
                I2C1->DATAR = tx_buf[tx_idx++];
            }
        }
        return;
    }

    if (star1 & I2C_STAR1_RXNE)
    {
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (rx_len < RX_BUF_SIZE) rx_buf[rx_len++] = b;
        return;
    }

    if (star1 & I2C_STAR1_TXE)
    {
        I2C1->DATAR = (tx_idx < tx_len) ? tx_buf[tx_idx++] : 0x00;
        return;
    }

    if (star1 & I2C_STAR1_STOPF)
    {
        I2C1->CTLR1 |= I2C_CTLR1_PE;

        if (dev_state == DEV_ENUM_READY)
        {
            if (rx_len == 2 && rx_buf[0] == REG_ASSIGN_ADDR)
            {
                new_addr  = rx_buf[1];
                dev_state = DEV_ASSIGNING;
            }
        }
        else if (dev_state == DEV_ASSIGNED)
        {
            if (rx_len > 0) cmd_ready = 1;
        }

        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return;
    }
}

void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void)
{
    I2C1->STAR1 &= ~(I2C_STAR1_BERR | I2C_STAR1_ARLO |
                     I2C_STAR1_AF   | I2C_STAR1_OVR);
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * COMMAND PROCESSOR
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_command(void)
{
    uint8_t cmd = rx_buf[0];

    if (cmd == CMD_RESET_POS)
    {
        enc_position = 0;
        enc_delta    = 0;
    }
    else if (cmd == CMD_SET_POS && rx_len >= 3)
    {
        int16_t new_pos = ((int16_t)rx_buf[1] << 8) | rx_buf[2];
        enc_delta    += (new_pos - enc_position);
        enc_position  = new_pos;
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * ENCODER POLL  —  called from main loop on every iteration
 *   Quadrature decoding via 4-entry Gray-code lookup table.
 *   Updates enc_position and enc_delta atomically relative to interrupts.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_encoder(void)
{
    uint8_t ab  = read_enc_ab();
    uint8_t idx = (enc_last_ab << 2) | ab;
    int8_t  dir = enc_table[idx];

    if (dir != 0)
    {
        __disable_irq();
        enc_position += dir;
        enc_delta    += dir;
        __enable_irq();
        last_rotation_ms = ms_tick;   /* start rotation lockout window */
    }

    enc_last_ab = ab;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * BUTTON POLL  —  debounced, called from main loop
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_button(void)
{
    uint8_t  raw = read_btn_raw();
    uint32_t now = ms_tick;

    /* Ignore button changes while the knob is turning or just finished turning.
     * Rotation physically stresses the switch pin and causes false triggers. */
    if ((now - last_rotation_ms) < BTN_ROTATION_LOCKOUT_MS)
    {
        btn_raw_last = raw;   /* stay in sync so no phantom edge on unlock */
        return;
    }

    if (raw != btn_raw_last)
    {
        btn_raw_last       = raw;
        btn_last_change_ms = now;
    }

    /* Commit only after the signal has been stable for the debounce period */
    if ((now - btn_last_change_ms) >= BTN_DEBOUNCE_MS)
        btn_state = raw;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    SystemInit();
    tim2_init();
    encoder_gpio_init();

    /* Seed initial encoder state so first poll produces no phantom step */
    enc_last_ab = read_enc_ab();

    calc_backoff();

    __enable_irq();

    while (1)
    {
        uint32_t now = ms_tick;

        /* ── Enumeration state machine ─────────────────────────── */

        if (dev_state == DEV_BOOT_WAITING && now >= backoff_ms)
        {
            enum_ready_start_ms = now;
            i2c_slave_init(ENUM_ADDR);
            dev_state = DEV_ENUM_READY;
        }

        if (dev_state == DEV_ENUM_READY && (now - enum_ready_start_ms) > 200)
        {
            /* Not assigned within 200 ms — likely a collision. Re-backoff. */
            I2C1->CTLR1 &= ~I2C_CTLR1_PE;
            backoff_ms = calc_rebackoff_ms();
            dev_state  = DEV_BOOT_WAITING;
        }

        if (dev_state == DEV_ASSIGNING)
        {
            i2c_switch_addr(new_addr);
            dev_state = DEV_ASSIGNED;
        }

        /* ── Normal operation ──────────────────────────────────── */

        poll_encoder();
        poll_button();

        if (dev_state == DEV_ASSIGNED)
        {
            if (delta_sent)
            {
                /* Pico has read the delta — clear it atomically */
                __disable_irq();
                delta_sent = 0;
                enc_delta  = 0;
                __enable_irq();
            }

            if (cmd_ready)
            {
                cmd_ready = 0;
                process_command();
            }
        }
    }
}
