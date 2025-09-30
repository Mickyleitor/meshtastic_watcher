/**
 * @file main.c
 * @brief Meshtastic Watcher — Minimal low-power pulse (MSP430G2553)
 *
 * @section what_it_does What it does
 * - Generates a LOW pulse on PULSE_PIN_BIT every @ref PULSE_INTERVAL_MIN minutes.
 * - Optimized for minimal energy; ~0.1 µA typical in LPM3 (excluding pulse time).
 *
 * @section how_it_does How it does
 * - Timer_A runs from ACLK = VLO (~12 kHz) and interrupts every ~30 s.
 *   The ISR accumulates 30 s ticks until the target interval elapses, then emits the pulse.
 * - Output uses open-drain behavior: idle Hi-Z; only driven LOW during the pulse by switching
 * PULSE_PIN_BIT to output-low.
 * - CPU remains in LPM3 between interrupts for low power.
 * - DCO (1 MHz) is only enabled to time the pulse with a simple busy-wait delay.
 * - All unused pins are configured as outputs driven LOW to minimize leakage.
 *
 * @section pins Pins
 * - OUTPUT -> PULSE_PIN_BIT  (active-LOW pulse; idle Hi-Z; open-drain style)
 * - GND    -> common ground with the target device
 *
 * @section build_config Build-time config
 * - @ref PULSE_INTERVAL_MIN : Minutes between pulses
 * - @ref PULSE_MS           : Pulse width in milliseconds.
 * - @ref PULSE_PIN_BIT      : Output pin bit mask
 * - @ref DBG_PIN_BIT        : Debug output pin bit mask (pulses on startup)
 *
 * @section notes Notes
 * - Assumes the target side provides a pull-up on the button GPIO.
 * - Timing uses VLO; expect drift vs. temperature and voltage.
 */

/* ---------------- Includes ---------------- */
#include <msp430.h>

/* ---------------- Defines ---------------- */
#define PULSE_INTERVAL_MIN (60 * 12) /* minutes between pulses */
#define PULSE_MS           (500u)    /* pulse duration in ms */
#define PULSE_PIN_BIT      (BIT4)    /* output pin: P1.4 */
#define DBG_PIN_BIT        (BIT3)    /* output pin: P1.3 */

/* Timer_A constants for ~30 s base period with VLO */
#define ACLK_VLO_HZ        (11805u) // VLO is ~12 kHz, measured to be 11.8 kHz
#define TIMER_DIV          (8u)
#define TIMER_HZ           (ACLK_VLO_HZ / TIMER_DIV)
#define BASE_PERIOD_S      (30u)
#define CCR0_30S           ((unsigned int)((unsigned long)BASE_PERIOD_S * (unsigned long)TIMER_HZ - 1u))

/* ---------------- Functions ---------------- */

/**
 * @brief Initialize system clocks.
 * - ACLK = VLO (~12 kHz) for Timer_A.
 * - DCO = 1 MHz used for delay_ms().
 */
static void clocks_init(void) {
    BCSCTL3 |= LFXT1S_2; /* ACLK = VLO */
    if (CALBC1_1MHZ != 0xFF) {
        BCSCTL1 = CALBC1_1MHZ;
        DCOCTL  = CALDCO_1MHZ;
    }
}

/**
 * @brief Initialize GPIO for low power.
 * - All unused pins set as outputs = 0.
 * - Pulse pin starts in Hi-Z (input); prepared LOW when driven.
 */
static void gpio_init_lowpower(void) {
    P1OUT = 0x00;
    P1DIR = 0xFF & ~PULSE_PIN_BIT; /* all outputs low; pulse pin as input */
    P2OUT = 0x00;
    P2DIR = 0xFF;

    P1SEL  &= ~PULSE_PIN_BIT;
    P1SEL2 &= ~PULSE_PIN_BIT;
    P1REN  &= ~PULSE_PIN_BIT; /* no internal pull */
    /* P1OUT bit already 0 -> ready to drive LOW when DIR=1 */
}

/**
 * @brief Initialize Timer_A to interrupt every ~30 s.
 */
static void timerA_init_30s(void) {
    TACTL    = TASSEL_1 | ID_3 | TACLR; /* ACLK, /8, clear */
    TACCR0   = CCR0_30S;                /* ~30 s */
    TACCTL0  = CCIE;                    /* enable CCR0 interrupt */
    TACTL   |= MC_1;                    /* up mode */
}

/**
 * @brief Simple delay in milliseconds using DCO=1 MHz.
 * @param ms number of milliseconds to delay
 */
static void delay_ms(unsigned int ms) {
    while (ms--) {
        __delay_cycles(1000);
    }
}

/**
 * @brief Generate a LOW pulse (open-drain style).
 * - Switch pin to output-LOW for @ref PULSE_MS, then back to input (Hi-Z).
 */
static void do_pulse(void) {
    P1OUT &= ~PULSE_PIN_BIT; /* ensure LOW when driven */
    P1DIR |= PULSE_PIN_BIT;  /* drive LOW */
    delay_ms(PULSE_MS);
    P1DIR &= ~PULSE_PIN_BIT; /* back to Hi-Z */
    /* P1OUT stays 0 for the next pulse */
}

/**
 * @brief Generate a debug burst on DBG_PIN_BIT.
 * - Pulses the pin 10 times with 100 ms HIGH, 100 ms LOW.
 * - Used to indicate startup.
 */
static void do_dbg_burst(void) {
    int i = 0;
    for (i = 0; i < 10; i++) {
        P1OUT |= DBG_PIN_BIT; /* drive HIGH */
        delay_ms(100);
        P1OUT &= ~DBG_PIN_BIT; /* drive LOW */
        delay_ms(100);
    }
    P1OUT &= ~DBG_PIN_BIT; /* ensure LOW */
}

/* ---------------- Main ---------------- */
int main(void) {
    WDTCTL = WDTPW | WDTHOLD; /* stop watchdog */

    clocks_init();
    gpio_init_lowpower();
    timerA_init_30s();
    do_dbg_burst();

    __enable_interrupt();

    for (;;) {
        __bis_SR_register(LPM3_bits | GIE); /* sleep until ISR */
    }
}

/* ---------------- Interrupt Service Routines ---------------- */

/**
 * @brief Timer_A0 ISR.
 * - Runs every ~30 s.
 * - Accumulates elapsed seconds until @ref PULSE_INTERVAL_MIN is reached.
 * - Calls do_pulse() when the interval expires.
 */
#pragma vector = TIMER0_A0_VECTOR
__interrupt void TIMER0_A0_ISR(void) {
    static unsigned long elapsed_sec  = 0;
    elapsed_sec                      += BASE_PERIOD_S;
    if (elapsed_sec >= (unsigned long)(PULSE_INTERVAL_MIN * 60UL)) {
        elapsed_sec = 0;
        do_pulse();
    }
}
