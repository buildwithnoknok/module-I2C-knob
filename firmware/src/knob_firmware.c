/*
 * noknok Knob Module Firmware  v1.5
 * CH32V003J4M6 (SOP-8)  |  Stack: cnlohr/ch32fun
 *
 * ── Hardware ─────────────────────────────────────────────────────────────
 *   PA2          Encoder A (active LOW, internal pull-up)
 *   PD6/PA1      Encoder B (active LOW, internal pull-up; read as PA1 via GPIOA)
 *   PC4          Encoder push button S2 (active LOW; S1 and C are grounded)
 *   PC1          I2C SDA
 *   PC2          I2C SCL
 *
 * ── Encoder counting ─────────────────────────────────────────────────────
 *   EXTI interrupts on both edges of PA1 and PA2 (EXTI1 + EXTI2, port A).
 *   Gray-code lookup table → ±1 per quadrature transition.
 *   A standard EC11 with 4 pulses/detent gives 4 counts per click.
 *
 * ── Button behaviour ─────────────────────────────────────────────────────
 *   50 ms debounce. Suppressed for 150 ms after any rotation (mechanical
 *   coupling between shaft and SW pin causes false triggers during turns).
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
 *     [btn_state]    0x01 if button pressed, 0x00 if released
 *                    Always 0x00 within 150 ms of rotation.
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

/* Encoder */
static volatile int16_t  enc_position    = 0;
static volatile int8_t   enc_delta       = 0;
static volatile uint32_t last_rotation_ms = 0;
static volatile uint8_t  enc_last_ab     = 0;
static volatile int8_t   enc_sub         = 0;  /* sub-step: 2 raw transitions = 1 count */

/* Lookup table: enc_table[(prev<<2)|curr] → +1, -1, or 0 */
static const int8_t enc_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

/* Button */
static volatile uint8_t  btn_state        = 0;
static          uint8_t  btn_raw_last      = 0;
static          uint32_t btn_last_change_ms = 0;

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
 * BACKOFF TIMER  —  proper FNV-1a, identical to Buzzer v3.1
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
 * BUILD RESPONSES
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

static void build_data_response(void)
{
    int16_t pos = enc_position;

    /* Read enc_delta and clear it atomically here in the ISR.
     * Clearing here (not in the main loop) prevents a race where encoder
     * transitions arriving between this read and a deferred main-loop clear
     * would be silently discarded. Any transitions after this point will
     * correctly accumulate into enc_delta for the next read. */
    int8_t dlt  = enc_delta;
    enc_delta   = 0;

    /* Suppress button during rotation lockout — shaft mechanics couple into SW */
    uint8_t btn = ((ms_tick - last_rotation_ms) >= BTN_ROTATION_LOCKOUT_MS)
                  ? btn_state : 0;
    tx_buf[0] = (uint8_t)((pos >> 8) & 0xFF);
    tx_buf[1] = (uint8_t)(pos & 0xFF);
    tx_buf[2] = (uint8_t)dlt;
    tx_buf[3] = btn;
    tx_len = 4;
    tx_idx = 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * GPIO INIT
 * ═══════════════════════════════════════════════════════════════════════════ */

static void encoder_gpio_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC;

    /* PA1 (ENC_B) and PA2 (ENC_A): input with pull-up */
    GPIOA->CFGLR &= ~((0xF << (1 * 4)) | (0xF << (2 * 4)));
    GPIOA->CFGLR |=  ((0x8 << (1 * 4)) | (0x8 << (2 * 4)));
    GPIOA->BSHR   =  (1 << 1) | (1 << 2);

    /* PC4: input with pull-up (button S2, active LOW; S1+C are grounded) */
    GPIOC->CFGLR &= ~(0xF << (4 * 4));
    GPIOC->CFGLR |=  (0x8 << (4 * 4));
    GPIOC->BSHR   =  (1 << 4);
}

static inline uint8_t read_enc_ab(void)
{
    /* A = PA2 (active low), B = PA1 (active low) — per Wagiminator reference.
     * Invert both so 1 means "encoder contact closed". */
    uint8_t a = !((GPIOA->INDR >> 2) & 1);   /* PA2 = ENC_A */
    uint8_t b = !((GPIOA->INDR >> 1) & 1);   /* PA1 = ENC_B */
    return (a << 1) | b;
}

static inline uint8_t read_btn_raw(void)
{
    return ((GPIOC->INDR >> 4) & 1) ? 0 : 1;   /* PC4, active LOW */
}


/* ═══════════════════════════════════════════════════════════════════════════
 * EXTI  —  hardware interrupts on both edges of PA1 (EXTI1) and PA2 (EXTI2)
 *   Catches every quadrature transition immediately, regardless of main loop
 *   speed. Both EXTI1 and EXTI2 share EXTI7_0_IRQn on CH32V003.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void exti_encoder_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO;

    /* Route EXTI1 and EXTI2 to port A (bits [3:2] and [5:4] = 00 = PA) */
    AFIO->EXTICR &= ~((0x3 << 2) | (0x3 << 4));

    /* Enable interrupts on both rising and falling edges for EXTI1 and EXTI2 */
    EXTI->INTENR |= (1 << 1) | (1 << 2);
    EXTI->RTENR  |= (1 << 1) | (1 << 2);
    EXTI->FTENR  |= (1 << 1) | (1 << 2);

    NVIC_EnableIRQ(EXTI7_0_IRQn);
}

void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void)
{
    if (EXTI->INTFR & ((1 << 1) | (1 << 2)))
    {
        uint8_t ab  = read_enc_ab();
        uint8_t idx = (enc_last_ab << 2) | ab;
        int8_t  dir = enc_table[idx];

        if (dir != 0)
        {
            last_rotation_ms = ms_tick;
            enc_sub += dir;
            if (enc_sub >= 2)      { enc_sub -= 2; enc_position++; enc_delta++; }
            else if (enc_sub <= -2){ enc_sub += 2; enc_position--; enc_delta--; }
        }

        enc_last_ab = ab;
        EXTI->INTFR = (1 << 1) | (1 << 2);   /* clear pending flags */
    }
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
 * BUTTON POLL  —  debounced, called from main loop
 *   During rotation lockout: force btn_state=0 and reset debounce tracker
 *   so the button must re-debounce from scratch after every turn ends.
 *   This prevents the SW pin's mechanical coupling from causing false presses.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_button(void)
{
    uint8_t  raw = read_btn_raw();
    uint32_t now = ms_tick;

    if ((now - last_rotation_ms) < BTN_ROTATION_LOCKOUT_MS)
    {
        /* Inside lockout: clear btn_state and reset debounce so the button
         * starts clean when the lockout window expires. */
        btn_state          = 0;
        btn_raw_last       = raw;
        btn_last_change_ms = now;
        return;
    }

    if (raw != btn_raw_last)
    {
        btn_raw_last       = raw;
        btn_last_change_ms = now;
    }

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

    /* Seed initial encoder state before enabling EXTI */
    enc_last_ab = read_enc_ab();

    exti_encoder_init();
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

        poll_button();

        if (dev_state == DEV_ASSIGNED)
        {
            if (cmd_ready)
            {
                cmd_ready = 0;
                process_command();
            }
        }
    }
}