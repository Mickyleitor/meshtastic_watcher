#include <msp430.h>
#include <stdbool.h>

/*
  Meshtastic Watcher — Supervisor-safe version (MSP430G2553)

  What it does
  - Simulates a button press to a Meshtastic Heltec V3 GPIO using an open-drain-like pulse.
  - Auto "press" about every 12 h (best-effort with VLO).
  - Local button can also trigger a press.
  - Software UVLO with hysteresis: MSP430 will not press unless VCC is above UVLO_RISE_MV
    for several confirmations; it returns to UVLO below UVLO_FALL_MV.
  - Startup inhibit window after power-good.

  Pins (change as needed)
    OUTPUT  -> P1.0  (to Meshtastic button GPIO through 220–1k series; open-drain style)
    BUTTON  -> P1.3  (local push button to GND; pull-up enabled)
    GND     -> common ground between MSP430 and Heltec

  Safety
  - Idle output is Hi-Z; we only drive LOW during the pulse.
  - No pulses below UVLO; no pulses during startup inhibit.
*/

/* ============================== Build-time config ============================== */

/* 1 = use VLO for ACLK (no crystal; large drift), 0 = use 32.768 kHz crystal */
#define USE_VLO                 1

/* Pins (port 1 bits) */
#define OUTPUT_PIN_BIT          BIT0      /* P1.0 -> Meshtastic GPIO (open-drain style) */
#define BUTTON_PIN_BIT          BIT3      /* P1.3 -> local button to GND */

/* Auto-press cadence in WDT ticks */
#if USE_VLO
  /* With VLO, WDT "1 s" interval is ~0.35–0.45 s. Field-calibrate if desired. */
  #define TOGGLE_TICKS          (115200UL)  /* ~12 h typical at ~0.38 s/tick */
#else
  #define TOGGLE_TICKS          (43200UL)   /* 12 h if ACLK = 32768 Hz */
#endif

/* Pulse width for simulated press */
#define PULSE_MS                120u

/* Debounce for local button, in WDT ticks */
#define DEBOUNCE_TICKS          1u         /* ~0.4 s with VLO; conservative */

/* Startup inhibit after power-good, in WDT ticks */
#define STARTUP_INHIBIT_TICKS   10u        /* ~a few seconds with VLO */

/* UVLO thresholds (millivolts) and policy */
#define UVLO_RISE_MV            3000u      /* leave UVLO only above this */
#define UVLO_FALL_MV            2900u      /* re-enter UVLO below this */
#define UVLO_CONFIRM_SAMPLES    3u         /* require consecutive confirmations */
#define UVLO_CHECK_EVERY_TICKS  8u         /* periodic VCC check cadence */

/* ============================== WDT compatibility shims ============================== */
#ifndef WDTSSEL__ACLK
  #define WDT_SRC_ACLK          WDTSSEL
#else
  #define WDT_SRC_ACLK          WDTSSEL__ACLK
#endif

#if defined(WDTIS__32K)
  #define WDT_DIV_32K           WDTIS__32K
#elif defined(WDTIS_3)
  #define WDT_DIV_32K           WDTIS_3
#else
  #define WDT_DIV_32K           (WDTIS0 | WDTIS1)
#endif

/* ============================== State ============================== */

static volatile unsigned long wdt_ticks = 0;
static volatile unsigned long ticks_since_last_uvlo_check = 0;
static volatile unsigned int  debounce_ticks = 0;
static volatile unsigned int  inhibit_ticks = 0;
static volatile bool          uvlo_active = true;      /* start in UVLO until proven otherwise */
static volatile bool          do_uvlo_check = false;   /* set by ISR; handled in main */

/* ============================== Helpers ============================== */

/* Open-drain-like output: idle Hi-Z, drive LOW during pulse */
static inline void od_idle(void) {
  P1OUT &= ~OUTPUT_PIN_BIT;   /* prepare low */
  P1DIR &= ~OUTPUT_PIN_BIT;   /* input = Hi-Z */
}

static inline void od_press_ms(unsigned int ms) {
  /* Only call when allowed; this function does not check UVLO/inhibit. */
  P1OUT &= ~OUTPUT_PIN_BIT;   /* low level to drive */
  P1DIR |=  OUTPUT_PIN_BIT;   /* drive low */
  /* Busy wait: assume ~1 MHz DCO; approximate is fine */
  const unsigned long cycles = (unsigned long)ms * 1000UL; /* ~1 cycle/µs at ~1 MHz */
  __delay_cycles(cycles);
  P1DIR &= ~OUTPUT_PIN_BIT;   /* back to Hi-Z */
}

/* Put all pins in a low-leakage state first */
static void gpio_global_lowpower_defaults(void) {
  P1DIR = 0xFF;  P1OUT = 0x00;
  P2DIR = 0xFF;  P2OUT = 0x00;
}

/* Configure used pins */
static void gpio_init(void) {
  /* OUTPUT */
  od_idle();

  /* BUTTON: input with pull-up; falling-edge interrupt */
  P1DIR &= ~BUTTON_PIN_BIT;
  P1OUT |=  BUTTON_PIN_BIT;   /* pull-up */
  P1REN |=  BUTTON_PIN_BIT;
  P1IES |=  BUTTON_PIN_BIT;   /* falling edge (press to GND) */
  P1IFG &= ~BUTTON_PIN_BIT;
  P1IE  |=  BUTTON_PIN_BIT;
}

/* Clocks */
static void clocks_init(void) {
#if USE_VLO
  BCSCTL3 |= LFXT1S_2;        /* ACLK = VLO */
#else
  BCSCTL3 &= ~(LFXT1S_3);     /* LFXT1 in LF mode */
  do { IFG1 &= ~OFIFG; __delay_cycles(50000); } while (IFG1 & OFIFG);
#endif
}

/* WDT interval timer (~1 s if crystal; ~0.35–0.45 s if VLO) */
static void wdt_interval_init(void) {
  WDTCTL = WDTPW | WDTTMSEL | WDT_SRC_ACLK | WDT_DIV_32K;
  IE1   |= WDTIE;
}

/* ADC10: read VCC in mV using internal VCC/2 channel and 2.5 V reference.
   On G2553, INCH_11 is VCC/2 when REFON is enabled.
   VCC(mV) ≈ ADC * 5000 / 1023 */
static unsigned int read_vcc_mV(void) {
  ADC10CTL0 = SREF_1 | REFON | REF2_5V | ADC10ON | ADC10SHT_3;
  ADC10CTL1 = INCH_11;             /* VCC/2 */
  __delay_cycles(30000);           /* allow reference to settle (~30 ms @ ~1 MHz) */

  ADC10CTL0 |= ENC | ADC10SC;
  while (!(ADC10CTL0 & ADC10IFG)) { /* wait */ }

  unsigned long mv = (unsigned long)ADC10MEM * 5000UL;

  ADC10CTL0 &= ~(ENC | ADC10ON | REFON); /* power down ADC + ref */
  return (unsigned int)(mv / 1023UL);
}

/* Block until VCC > UVLO_RISE_MV for UVLO_CONFIRM_SAMPLES consecutive reads */
static void wait_for_power_good(void) {
  unsigned int ok_count = 0;

  for (;;) {
    unsigned int mv = read_vcc_mV();
    if (mv >= UVLO_RISE_MV) {
      if (++ok_count >= UVLO_CONFIRM_SAMPLES) break;
    } else {
      ok_count = 0;
    }

    /* Sleep a little before next sample; use WDT ticks if available */
    unsigned int ticks = UVLO_CHECK_EVERY_TICKS;
    while (ticks--) {
      __bis_SR_register(LPM3_bits | GIE);
      __no_operation();
    }
  }
}

/* UVLO periodic check with hysteresis */
static void uvlo_periodic_check(void) {
  unsigned int mv = read_vcc_mV();
  if (uvlo_active) {
    static unsigned int ok_count = 0;
    if (mv >= UVLO_RISE_MV) {
      if (++ok_count >= UVLO_CONFIRM_SAMPLES) {
        uvlo_active = false;
        ok_count = 0;
        inhibit_ticks = STARTUP_INHIBIT_TICKS;  /* grace period after recovery */
      }
    } else {
      ok_count = 0;
    }
  } else {
    if (mv < UVLO_FALL_MV) {
      uvlo_active = true;
    }
  }
}

/* Sleep helper */
static inline void sleep_lpm3(void) {
  __bis_SR_register(LPM3_bits | GIE);
  __no_operation();
}

/* ============================== Main ============================== */

int main(void) {
  WDTCTL = WDTPW | WDTHOLD;  /* stop WDT during init */

  gpio_global_lowpower_defaults();
  clocks_init();
  gpio_init();
  wdt_interval_init();

  /* Power-good gate: remain in UVLO until VCC is safely above threshold */
  uvlo_active = true;
  wait_for_power_good();
  uvlo_active = false;
  inhibit_ticks = STARTUP_INHIBIT_TICKS;

  __enable_interrupt();

  for (;;) {
    /* All work is interrupt-driven; we handle UVLO checks here to keep ISRs short */
    sleep_lpm3();

    if (do_uvlo_check) {
      do_uvlo_check = false;
      uvlo_periodic_check();
    }

    if (uvlo_active) {
      od_idle(); /* ensure Hi-Z while in UVLO */
    }
  }
}

/* ============================== ISRs ============================== */

/* Watchdog Timer ISR:
   - Ticks cadence and auto-press counter
   - Schedules periodic UVLO checks
*/
#pragma vector=WDT_VECTOR
__interrupt void WDT_ISR(void) {
  if (debounce_ticks > 0)  debounce_ticks--;
  if (inhibit_ticks  > 0)  inhibit_ticks--;

  if (++ticks_since_last_uvlo_check >= UVLO_CHECK_EVERY_TICKS) {
    ticks_since_last_uvlo_check = 0;
    do_uvlo_check = true;                         /* let main perform ADC read */
    __bic_SR_register_on_exit(LPM3_bits);         /* wake main loop */
  }

  /* Auto-press only if active and not inhibited */
  if (!uvlo_active && inhibit_ticks == 0) {
    if (++wdt_ticks >= TOGGLE_TICKS) {
      wdt_ticks = 0;
      od_press_ms(PULSE_MS);
    }
  }
}

/* Port 1 ISR: local button press on P1.3 */
#pragma vector=PORT1_VECTOR
__interrupt void PORT1_ISR(void) {
  if (P1IFG & BUTTON_PIN_BIT) {
    P1IFG &= ~BUTTON_PIN_BIT;

    if (debounce_ticks == 0) {
      if (!uvlo_active && inhibit_ticks == 0) {
        od_press_ms(PULSE_MS);
        wdt_ticks = 0;                /* optional: restart cadence after manual press */
      }
      debounce_ticks = DEBOUNCE_TICKS;
    }
  }

  __bic_SR_register_on_exit(LPM3_bits);  /* return to active to process flags */
}
