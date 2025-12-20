/**
 * @file main.c
 * @brief Meshtastic brownout recovery watchdog (MSP430G2553)
 *
 * Monitors supply voltage and triggers reset pulse when brownout recovery pattern
 * is detected (voltage drop → recovery → stable). First evaluation cycle always
 * pulses to ensure clean startup.
 *
 * Hardware:
 * - Uses internal ADC (VCC/2 channel) with 2.5V reference for voltage measurement
 * - Open-drain output on PULSE_PIN_BIT (pull LOW during pulse, Hi-Z otherwise)
 * - VLO (~12kHz) drives Timer_A for periodic wakeup from LPM3
 * - All unused pins configured as inputs with pull-downs to minimize leakage
 *
 * Power: <1µA average (LPM3 with brief ADC measurements and pulse events)
 */

#include <msp430.h>
#include <stdint.h>

/* ============================================================================
 * CONFIGURATION
 * ============================================================================ */

#define PULSE_INTERVAL_MIN      (60 * 12) /* Evaluation window */
#define BASE_PERIOD_S           (30u)     /* Wakeup interval */
#define PULSE_MS                (500u)    /* Reset pulse duration */

/* Voltage thresholds (hysteresis prevents oscillation) */
#define VCC_LOW_THRESHOLD_MV    (2900u) /* Below = brownout detected */
#define VCC_STABLE_THRESHOLD_MV (3100u) /* Above = recovery confirmed */

/* Pin assignments */
#define PULSE_PIN_BIT           (BIT4) /* P1.4 → Meshtastic reset */
#define DBG_PIN_BIT             (BIT3) /* P1.3 → Debug UART TX (~57600 baud) */

/* Clock configuration */
#define ACLK_VLO_HZ             (12000u) /* VLO nominal frequency (±10% variation) */
#define TIMER_DIV               (8u)     /* Timer_A divider */
#define TIMER_HZ                (ACLK_VLO_HZ / TIMER_DIV)
#define CCR0_30S                ((unsigned int)((unsigned long)BASE_PERIOD_S * (unsigned long)TIMER_HZ - 1u))

/* ============================================================================
 * IMPLEMENTATION
 * ============================================================================ */

/* ---------------- Forward decls ---------------- */
static void clocks_init(void);
static void gpio_init_lowpower(void);
static void timerA_init_30s(void);
static void delay_ms(unsigned int ms);
static void do_pulse(void);
static void dbg_print(const uint8_t* data, uint8_t data_length);
static unsigned long adc10_read_vcc_mv(void);

static void clocks_init(void) {
    BCSCTL3 |= LFXT1S_2; /* VLO for ACLK (runs in LPM3) */
    if (CALBC1_1MHZ != 0xFF) {
        BCSCTL1 = CALBC1_1MHZ;
        DCOCTL  = CALDCO_1MHZ;
    }
}

/* ---------------- GPIO ---------------- */

static void gpio_init_lowpower(void) {
    /* -------- Phase 1: all pins Hi-Z (no forcing) -------- */
    /* Ensure all pins are plain GPIO (no alternate functions) */
    P1SEL  = 0x00;
    P1SEL2 = 0x00;
    P2SEL  = 0x00;
    P2SEL2 = 0x00;

    /* Configure as inputs with no internal pulls */
    P1DIR = 0x00;
    P2DIR = 0x00;
    P1REN = 0x00;
    P2REN = 0x00;
    /* P1OUT / P2OUT values don’t matter while DIR = 0 */

    /* -------- Phase 2: low-leakage for all except PULSE -------- */
    const uint8_t KEEP_HIZ_P1  = (PULSE_PIN_BIT | DBG_PIN_BIT); /* add | DBG_PIN_BIT if needed */
    const uint8_t P1_PULL_MASK = (uint8_t)(~KEEP_HIZ_P1);       /* pins that will get pull-downs */

    /* For pins with REN = 1, OUT = 0 means pull-down.
       Leave PULSE pin untouched (REN = 0 ⇒ true Hi-Z). */
    P1OUT &= KEEP_HIZ_P1; /* ensure OUT = 0 on all except PULSE */
    P2OUT  = 0x00;        /* all P2 pulls are down */

    P1REN |= P1_PULL_MASK; /* enable pull-downs on all but PULSE */
    P2REN |= 0xFF;         /* enable pull-downs on all P2 pins */

    /* DIR remains 0 → all pins are inputs (no driving). */
}

static void timerA_init_30s(void) {
    TACTL    = TASSEL_1 | ID_3 | TACLR;
    TACCR0   = CCR0_30S;
    TACCTL0  = CCIE;
    TACTL   |= MC_1; /* UP mode: counts 0→CCR0 */
}

static void delay_ms(unsigned int ms) {
    BCSCTL1 = CALBC1_1MHZ;
    DCOCTL  = CALDCO_1MHZ;
    while (ms--) {
        __delay_cycles(1000);
    }
}

static void do_pulse(void) {
    P1OUT &= ~PULSE_PIN_BIT;
    P1DIR |= PULSE_PIN_BIT; /* Open-drain: Hi-Z → LOW */
    delay_ms(PULSE_MS);
    P1DIR &= ~PULSE_PIN_BIT; /* Back to Hi-Z (pull-up restores HIGH) */
}

static void dbg_print(const uint8_t* data, uint8_t len) {
    const unsigned int BIT_CYCLES = 17; /* ~57600 baud @ 1MHz */
    const unsigned int IDLE_GUARD = 2;
    uint8_t i, b;

    P1OUT |= DBG_PIN_BIT;
    P1DIR |= DBG_PIN_BIT;
    __delay_cycles(IDLE_GUARD * BIT_CYCLES);

    for (i = 0; i < len; i++) {
        b              = data[i];
        uint16_t frame = (uint16_t)(b << 1) | 0x200u; /* 8N1: start=0, stop=1 */

        uint8_t k;
        for (k = 0; k < 10; k++) {
            if (frame & 0x01) {
                P1OUT |= DBG_PIN_BIT;
            } else {
                P1OUT &= ~DBG_PIN_BIT;
            }
            __delay_cycles(BIT_CYCLES);
            frame >>= 1;
        }

        P1OUT |= DBG_PIN_BIT;
        __delay_cycles(BIT_CYCLES);
    }

    P1OUT |= DBG_PIN_BIT;
    __delay_cycles(IDLE_GUARD * BIT_CYCLES);
    __delay_cycles(200);
    P1DIR &= ~DBG_PIN_BIT;
}

static unsigned long adc10_read_vcc_mv(void) {
    ADC10CTL1 = INCH_11; /* VCC/2 channel */
    ADC10CTL0 = SREF_1 | REF2_5V | ADC10SHT_2 | ADC10ON | REFON;
    __delay_cycles(3000);
    ADC10CTL0 |= ENC | ADC10SC;
    while (ADC10CTL1 & ADC10BUSY);
    unsigned int adc  = ADC10MEM;
    ADC10CTL0        &= ~(ENC | ADC10ON | REFON);

    /* VCC = (adc * 5000mV) / 1023 (VCC/2 against 2.5V ref → full scale = 5V) */
    unsigned long vcc_mv = ((unsigned long)adc * 5000ul + 511ul) / 1023ul;
    if (vcc_mv > 65535ul) {
        vcc_mv = 65535ul;
    }
    return vcc_mv;
}

static volatile unsigned char tick_30s = 0;

#pragma vector = TIMER0_A0_VECTOR
__interrupt void TIMER0_A0_ISR(void) {
    tick_30s = 1;
    __bic_SR_register_on_exit(LPM3_bits); /* Prevent immediate re-sleep */
}

/* Hysteresis pattern detection: vcc_low_seen (sticky) + vcc_enough_now (transient)
 * Pulse when: first_window OR (drop seen AND recovered) */
int main(void) {
    WDTCTL = WDTPW | WDTHOLD;
    clocks_init();
    gpio_init_lowpower();
    timerA_init_30s();
    __enable_interrupt();

    unsigned long elapsed_sec     = 0;
    unsigned char first_window    = 1;
    unsigned char vcc_low_seen    = 0; /* Sticky: cleared only on pulse */
    unsigned long vcc_lowest_seen = 4000;
    unsigned char how_many_pulses = 0;

    for (;;) {
        __bis_SR_register(LPM3_bits | GIE);

        if (!tick_30s) {
            continue;
        }
        tick_30s = 0;

        unsigned long vcc_mv_now     = adc10_read_vcc_mv();
        unsigned char vcc_enough_now = 0;

        if (vcc_mv_now < VCC_LOW_THRESHOLD_MV) {
            vcc_low_seen = 1;
        } else if (vcc_mv_now >= VCC_STABLE_THRESHOLD_MV) {
            vcc_enough_now = 1;
        }

        if (vcc_mv_now < vcc_lowest_seen) {
            vcc_lowest_seen = vcc_mv_now;
        }

        elapsed_sec += BASE_PERIOD_S;
        if (elapsed_sec >= (unsigned long)(PULSE_INTERVAL_MIN * 60ul)) {
            if (first_window || (vcc_low_seen && vcc_enough_now)) {
                do_pulse();
                vcc_low_seen = 0; /* Clear on pulse → hysteresis */
                how_many_pulses++;
            }

            first_window = 0;
            elapsed_sec  = 0;
            /* Note: vcc_low_seen persists if no pulse (unstable recovery) */
        }
    }
}
