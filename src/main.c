/**
 * @file main.c
 * @brief Meshtastic Watcher — Ultra-low-power 24 h conditional pulse (MSP430G2553)
 *
 * Behavior
 * - MCU stays in LPM3 nearly all the time.
 * - Every ~30 s (Timer_A interrupt):
 *   - Wake briefly.
 *   - Power up ADC10 + 2.5 V ref, measure VCC using internal VCC/2 (INCH_11).
 *   - If VCC < 3000 mV, set a “low VCC seen” flag.
 *   - Power down ADC, reference, and DCO immediately.
 * - After 12 h:
 *   - Emit a LOW pulse if (first window) or (low VCC seen during the window).
 *   - Reset the flag and start a new 12 h window.
 *
 * Power design
 * - LPM3 between interrupts; CPU + DCO off; ACLK active (VLO).
 * - ADC and reference active only for ~3 ms per 30 s tick (<0.01% duty cycle).
 * - Pulse uses DCO only during 500 ms active-low drive.
 * - All unused pins driven low to minimize leakage.
 */

#include <msp430.h>
#include <stdint.h>

/* ---------------- Config ---------------- */
#define PULSE_INTERVAL_MIN      (60 * 12) /* 12 h */
#define PULSE_MS                (500u)
#define PULSE_PIN_BIT           (BIT4)
#define DBG_PIN_BIT             (BIT3)
#define VCC_LOW_THRESHOLD_MV    (2900u)
#define VCC_STABLE_THRESHOLD_MV (3100u)

/* Timer_A ~30 s tick */
#define ACLK_VLO_HZ             (12000u)
#define TIMER_DIV               (8u)
#define TIMER_HZ                (ACLK_VLO_HZ / TIMER_DIV)
#define BASE_PERIOD_S           (30u)
#define CCR0_30S                ((unsigned int)((unsigned long)BASE_PERIOD_S * (unsigned long)TIMER_HZ - 1u))

/* ---------------- Forward decls ---------------- */
static void clocks_init(void);
static void gpio_init_lowpower(void);
static void timerA_init_30s(void);
static void delay_ms(unsigned int ms);
static void do_pulse(void);
static void dbg_print(const uint8_t* data, uint8_t data_length);
static unsigned long adc10_read_vcc_mv(void);

/* ---------------- Clocks ---------------- */
static void clocks_init(void) {
    BCSCTL3 |= LFXT1S_2;       /* ACLK = VLO (~12 kHz) */
    if (CALBC1_1MHZ != 0xFF) { /* DCO = 1 MHz for brief active use */
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

/* ---------------- Timer_A (ACLK ~12 kHz) ---------------- */
static void timerA_init_30s(void) {
    TACTL    = TASSEL_1 | ID_3 | TACLR; /* ACLK /8 */
    TACCR0   = CCR0_30S;
    TACCTL0  = CCIE;
    TACTL   |= MC_1; /* up mode */
}

/* ---------------- Delay ---------------- */
static void delay_ms(unsigned int ms) {
    /* Temporarily enable DCO for timing */
    BCSCTL1 = CALBC1_1MHZ;
    DCOCTL  = CALDCO_1MHZ;
    while (ms--) {
        __delay_cycles(1000);
    }
    /* DCO will be off again when entering LPM3 */
}

/* ---------------- Pulse (active-LOW, open-drain style) ---------------- */
static void do_pulse(void) {
    P1OUT &= ~PULSE_PIN_BIT;
    P1DIR |= PULSE_PIN_BIT;
    delay_ms(PULSE_MS);
    P1DIR &= ~PULSE_PIN_BIT; /* Hi-Z again */
}

/* ---------------- Debug pulse at startup ---------------- */

/* Bit-banged UART TX (8N1) on DBG_PIN_BIT.
   ~57 600 baud @ 1 MHz DCO, with clean pre/post idle and no phantom byte. */
static void dbg_print(const uint8_t* data, uint8_t len) {
    const unsigned int BIT_CYCLES = 17; /* ≈57 600 baud @ 1 MHz DCO */
    const unsigned int IDLE_GUARD = 2;  /* ~idle guard before/after */

    uint8_t i, b;

    /* ---- Prepare line ---- */
    P1OUT |= DBG_PIN_BIT;                    /* drive HIGH early */
    P1DIR |= DBG_PIN_BIT;                    /* make it output */
    __delay_cycles(IDLE_GUARD * BIT_CYCLES); /* allow line to stabilize HIGH */

    /* ---- Transmit ---- */
    for (i = 0; i < len; i++) {
        b              = data[i];
        uint16_t frame = (uint16_t)(b << 1) | 0x200u; /* start=0, stop=1 */

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

        /* Stop bit spacing */
        P1OUT |= DBG_PIN_BIT;
        __delay_cycles(BIT_CYCLES);
    }

    /* ---- Keep line HIGH longer to ensure analyzer sees idle ---- */
    P1OUT |= DBG_PIN_BIT;
    __delay_cycles(IDLE_GUARD * BIT_CYCLES);

    /* ---- NEW: Leave pin HIGH briefly before going Hi-Z ---- */
    __delay_cycles(200);   /* few µs to let pull-up take over */
    P1DIR &= ~DBG_PIN_BIT; /* input (Hi-Z), but OUT bit remains HIGH */

    /* Do NOT clear P1OUT; keep bit=1 so future output starts HIGH cleanly */
}

/* ---------------- ADC10: VCC measurement ----------------
 * Uses VCC/2 (INCH_11) with internal 2.5 V ref.
 * Enables ADC and ref only during conversion; then fully powers them down.
 */
static unsigned long adc10_read_vcc_mv(void) {
    ADC10CTL1 = INCH_11;
    ADC10CTL0 = SREF_1 | REF2_5V | ADC10SHT_2 | ADC10ON | REFON;
    __delay_cycles(3000); /* allow reference to settle */
    ADC10CTL0 |= ENC | ADC10SC;
    while (ADC10CTL1 & ADC10BUSY);
    unsigned int adc      = ADC10MEM;
    ADC10CTL0            &= ~(ENC | ADC10ON | REFON);
    /* VCC[mV] = ADC * 5000 / 1023 */
    unsigned long vcc_mv  = ((unsigned long)adc * 5000ul + 511ul) / 1023ul;
    if (vcc_mv > 65535ul) {
        vcc_mv = 65535ul;
    }
    return vcc_mv;
}

/* ---- Globals used between ISR and main ---- */
static volatile unsigned char tick_30s = 0;

/* ---- ISR: signal tick and wake CPU from LPM3 ---- */
#pragma vector = TIMER0_A0_VECTOR
__interrupt void TIMER0_A0_ISR(void) {
    tick_30s = 1;
    __bic_SR_register_on_exit(LPM3_bits); /* wake main */
}

/* ---- Main loop version (work in main) ---- */
int main(void) {
    WDTCTL = WDTPW | WDTHOLD;
    clocks_init();
    gpio_init_lowpower();
    timerA_init_30s();
    __enable_interrupt();

    unsigned long elapsed_sec     = 0;
    unsigned char first_window    = 1;
    unsigned char vcc_low_seen    = 0;
    unsigned long vcc_lowest_seen = 4000; /* 32-bit to avoid overflow */
    unsigned char how_many_pulses = 0;

    for (;;) {
        /* Sleep until a 30 s tick arrives */
        __bis_SR_register(LPM3_bits | GIE);

        if (!tick_30s) {
            continue; /* spurious wake guard */
        }
        tick_30s = 0;

        /* Per-tick local; resets every 30 s */
        unsigned char vcc_enough_now = 0;

        unsigned long vcc_mv_now = adc10_read_vcc_mv();
        if (vcc_mv_now < VCC_LOW_THRESHOLD_MV) {
            vcc_low_seen = 1;
        } else if (vcc_mv_now >= VCC_STABLE_THRESHOLD_MV) {
            vcc_enough_now = 1;
        }

        if (vcc_mv_now < vcc_lowest_seen) {
            vcc_lowest_seen = vcc_mv_now; /* keep track of lowest VCC seen */
        }

        elapsed_sec += BASE_PERIOD_S;
        if (elapsed_sec >= (unsigned long)(PULSE_INTERVAL_MIN * 60ul)) {
            if (first_window || (vcc_low_seen && vcc_enough_now)) {
                do_pulse();
                vcc_low_seen = 0; /* clear only when we actually pulse */
                how_many_pulses++;
            }
            first_window = 0;
            elapsed_sec  = 0; /* start next 24 h window */
            /* Important: do not clear vcc_low_seen here if we didn't pulse */
        }
    }
}
