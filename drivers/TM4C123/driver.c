/*
  driver.c - An embedded CNC Controller with rs274/ngc (g-code) support

  Driver code for Texas Instruments Tiva C (TM4C123GH6PM) ARM processor

  Part of GrblHAL

  Copyright (c) 2016-2020 Terje Io

  Some parts
   Copyright (c) 2011-2015 Sungeun K. Jeon
   Copyright (c) 2009-2011 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"
#include "eeprom.h"
#include "serial.h"

#if KEYPAD_ENABLE
#include "keypad/keypad.h"
static void keyclick_int_handler (void);
#endif

#if TRINAMIC_ENABLE
static void trinamic_warn_isr (void);
#if !KEYPAD_ENABLE
static void trinamic_diag1_isr (void);
#endif
#endif

#if KEYPAD_ENABLE || TRINAMIC_I2C
#include "i2c.h"
#endif

#if ATC_ENABLE
#include "atc.h"
#endif

#if LASER_PPI || ATC_ENABLE
#include "usermcodes.h"
#endif

#ifdef DRIVER_SETTINGS
driver_settings_t driver_settings;
#endif

// prescale step counter to 20Mhz (80 / (STEPPER_DRIVER_PRESCALER + 1))
#define STEPPER_DRIVER_PRESCALER 3

//#define SPINDLE_SYNC_ENABLE // do NOT enable - not complete!

#if PWM_RAMPED

#define SPINDLE_RAMP_STEP_INCR 20 // timer compare register change per ramp step
#define SPINDLE_RAMP_STEP_TIME 2  // ms

typedef struct {
    volatile uint32_t ms_cfg;
    volatile uint32_t delay.ms;
    int32_t pwm_current;
    int32_t pwm_target;
    int32_t pwm_step;
} pwm_ramp_t;

static pwm_ramp_t pwm_ramp;
#endif

#if LASER_PPI

laser_ppi_t laser;

static void ppi_timeout_isr (void);

#endif

#ifdef SPINDLE_SYNC_ENABLE

typedef struct {                     // Set when last encoder pulse count did not match at last index
    float block_start;
    float prev_pos;
    float dpp; // distance per pulse in mm
    void (*stepper_pulse_start_normal)(stepper_t *stepper);
    uint32_t timer_value_start;
    uint_fast8_t segment_id;
    uint32_t segments;
} spindle_sync_t;

static void stepperPulseStartSyncronized (stepper_t *stepper);

#endif

static bool pwmEnabled = false, IOInitDone = false, probeState = false;
static axes_signals_t next_step_outbits;
static spindle_pwm_t spindle_pwm;
static delay_t delay = { .ms = 1, .callback = NULL }; // NOTE: initial ms set to 1 for "resetting" systick timer on startup

// Inverts the probe pin state depending on user settings and probing cycle mode.
static uint8_t probe_invert;

#if STEP_OUTMODE == GPIO_MAP

    static const uint8_t c_step_outmap[8] = {
        0,
        X_STEP_PIN,
        Y_STEP_PIN,
        X_STEP_PIN|Y_STEP_PIN,
        Z_STEP_PIN,
        X_STEP_PIN|Z_STEP_PIN,
        Y_STEP_PIN|Z_STEP_PIN,
        X_STEP_PIN|Y_STEP_PIN|Z_STEP_PIN
    };

    static uint8_t step_outmap[8];

#endif

#if DIRECTION_OUTMODE == GPIO_MAP

    static const uint8_t c_dir_outmap[8] = {
        0,
        X_DIRECTION_PIN,
        Y_DIRECTION_PIN,
        X_DIRECTION_PIN|Y_DIRECTION_PIN,
        Z_DIRECTION_PIN,
        X_DIRECTION_PIN|Z_DIRECTION_PIN,
        Y_DIRECTION_PIN|Z_DIRECTION_PIN,
        X_DIRECTION_PIN|Y_DIRECTION_PIN|Z_DIRECTION_PIN
    };

    static uint8_t dir_outmap[8];

#endif

static void spindle_set_speed (uint_fast16_t pwm_value);

// Interrupt handler prototypes

static void stepper_driver_isr (void);
static void stepper_pulse_isr (void);
static void stepper_pulse_isr_delayed (void);
static void limit_isr (void);
static void limit_isr_debounced (void);
static void control_isr (void);
static void software_debounce_isr (void);
static void systick_isr (void);

static void driver_delay_ms (uint32_t ms, void (*callback)(void))
{
    if(delay.callback)
        delay.callback();

    if(ms) {
        delay.ms = ms;
        SysTickEnable();
        if(!(delay.callback = callback))
            while(delay.ms);
    } else {
        if(delay.ms) {
            delay.callback = NULL;
            delay.ms = 1;
        }
        if(callback)
            callback();
    }
}

// Set stepper pulse output pins
// NOTE: step_outbits are: bit0 -> X, bit1 -> Y, bit2 -> Z...
// Mapping to registers can be done by
// 1. bitbanding. Pros: can assign pins to different ports, no RMW needed. Cons: overhead, pin changes not synchronous
// 2. bit shift. Pros: fast, Cons: bits must be consecutive
// 3. lookup table. Pros: signal inversions done at setup, Cons: slower than bit shift
inline static void set_step_outputs (axes_signals_t step_outbits)
{
#if STEP_OUTMODE == GPIO_MAP
    GPIOPinWrite(STEP_PORT, HWSTEP_MASK, step_outmap[step_outbits.value]);
#else
    GPIOPinWrite(STEP_PORT, HWSTEP_MASK, (step_outbits.value ^ settings.steppers.step_invert.mask) << STEP_OUTMODE);
#endif
}

// Set stepper direction output pins
// NOTE: see note for set_step_outputs()
inline static void set_dir_outputs (axes_signals_t dir_outbits)
{
#if DIRECTION_OUTMODE == GPIO_MAP
    GPIOPinWrite(DIRECTION_PORT, HWDIRECTION_MASK, dir_outmap[dir_outbits.value]);
#else
    GPIOPinWrite(DIRECTION_PORT, HWDIRECTION_MASK, (dir_outbits.value ^ settings.dir_invert.mask) << DIRECTION_OUTMODE);
#endif
}

// Disable steppers
static void stepperEnable (axes_signals_t enable)
{
    enable.mask ^= settings.steppers.enable_invert.mask;
#if TRINAMIC_ENABLE && TRINAMIC_I2C
    axes_signals_t tmc_enable = trinamic_stepper_enable(enable);
  #if !CNC_BOOSTERPACK // Trinamic BoosterPack does not support mixed drivers
    if(!tmc_enable.z)
        GPIOPinWrite(STEPPERS_DISABLE_Z_PORT, STEPPERS_DISABLE_Z_PIN, enable.z ? STEPPERS_DISABLE_Z_PIN : 0);
    if(!tmc_enable.x)
        GPIOPinWrite(STEPPERS_DISABLE_Z_PORT, STEPPERS_DISABLE_Z_PIN, enable.z ? STEPPERS_DISABLE_Z_PIN : 0);
  #endif
#elif CNC_BOOSTERPACK
    GPIOPinWrite(STEPPERS_DISABLE_XY_PORT, STEPPERS_DISABLE_XY_PIN, enable.x ? STEPPERS_DISABLE_XY_PIN : 0);
    GPIOPinWrite(STEPPERS_DISABLE_Z_PORT, STEPPERS_DISABLE_Z_PIN, enable.z ? STEPPERS_DISABLE_Z_PIN : 0);
#else
    GPIOPinWrite(STEPPERS_DISABLE_PORT, STEPPERS_DISABLE_PIN, enable.x ? STEPPERS_DISABLE_PIN : 0);
#endif
}

// Starts stepper driver ISR timer and forces a stepper driver interrupt callback
static void stepperWakeUp (void)
{
#if LASER_PPI
    laser.next_pulse = 0;
#endif

    // Enable stepper drivers.
    stepperEnable((axes_signals_t){AXES_BITMASK});

    TimerLoadSet(STEPPER_TIMER_BASE, TIMER_A, 5000);    // dummy...
    TimerEnable(STEPPER_TIMER_BASE, TIMER_A);
}

// Disables stepper driver interrupts and reset outputs
static void stepperGoIdle (bool clear_signals)
{
    TimerDisable(STEPPER_TIMER_BASE, TIMER_A);

    if(clear_signals) {
        set_step_outputs((axes_signals_t){0});
        set_dir_outputs((axes_signals_t){0});
    }
}

// Sets up stepper driver interrupt timeout
static void stepperCyclesPerTick (uint32_t cycles_per_tick)
{
// Limit min steps/s to about 2 (hal.f_step_timer @ 20MHz)
#ifdef ADAPTIVE_MULTI_AXIS_STEP_SMOOTHING
    TimerLoadSet(STEPPER_TIMER_BASE, TIMER_A, cycles_per_tick < (1UL << 18) ? cycles_per_tick : (1UL << 18) - 1UL);
#else
    TimerLoadSet(STEPPER_TIMER_BASE, TIMER_A, cycles_per_tick < (1UL << 23) ? cycles_per_tick : (1UL << 23) - 1UL);
#endif
}

// "Normal" version: Sets stepper direction and pulse pins and starts a step pulse a few nanoseconds later.
// If spindle synchronized motion switch to PID version.
static void stepperPulseStart (stepper_t *stepper)
{
#ifdef SPINDLE_SYNC_ENABLE
    if(stepper->new_block) {
        if(stepper->exec_segment->spindle_sync) {
            spindle_tracker.stepper_pulse_start_normal = hal.stepper_pulse_start;
            hal.stepper_pulse_start = stepperPulseStartSyncronized;
            stepperPulseStartSyncronized(stepper);
            return;
        }
        stepper->new_block = false;
        set_dir_outputs(stepper->dir_outbits);
    }
#else
    if(stepper->new_block) {
        stepper->new_block = false;
        set_dir_outputs(stepper->dir_outbits);
    }
#endif

    if(stepper->step_outbits.value) {
        set_step_outputs(stepper->step_outbits);
        TimerEnable(PULSE_TIMER_BASE, TIMER_A);
    }
}

// Delayed pulse version: sets stepper direction and pulse pins and starts a step pulse with an initial delay.
// If spindle synchronized motion switch to PID version.
// TODO: only delay after setting dir outputs?
static void stepperPulseStartDelayed (stepper_t *stepper)
{
#ifdef SPINDLE_SYNC_ENABLE
    if(stepper->new_block) {
        if(stepper->exec_segment->spindle_sync) {
            spindle_tracker.stepper_pulse_start_normal = hal.stepper_pulse_start;
            hal.stepper_pulse_start = stepperPulseStartSyncronized;
            stepperPulseStartSyncronized(stepper);
            return;
        }
        stepper->new_block = false;
        set_dir_outputs(stepper->dir_outbits);
    }
#else
    if(stepper->new_block) {
        stepper->new_block = false;
        set_dir_outputs(stepper->dir_outbits);
    }
#endif

    if(stepper->step_outbits.value) {
        next_step_outbits = stepper->step_outbits; // Store out_bits
        TimerEnable(PULSE_TIMER_BASE, TIMER_A);
    }
}

#if SPINDLE_SYNC_ENABLE

#error Spindle sync code not ready!

// Spindle sync version: sets stepper direction and pulse pins and starts a step pulse.
// Switches back to "normal" version if spindle synchronized motion is finished.
// TODO: add delayed pulse handling...
static void stepperPulseStartSyncronized (stepper_t *stepper)
{
    static spindle_sync_t spindle_sync;

    if(stepper->new_block) {
        if(!stepper->exec_segment->spindle_sync) {
            hal.stepper_pulse_start = spindle_tracker.stepper_pulse_start_normal;
            hal.stepper_pulse_start(stepper);
            return;
        } else {
            spindle_sync.dpp = stepper->exec_block->programmed_rate * 120.0f;
            spindle_sync.prev_pos = 0.0f;
            spindle_sync.timer_value_start = 123;
            spindle_sync.block_start = 2.33f;
            spindle_sync.segments = 0;
            spindle_sync.segment_id = stepper->exec_segment->id + 1; // force recalc
        }
        stepper->new_block = false;
        set_dir_outputs(stepper->dir_outbits);
    }

    if(stepper->step_outbits.value) {
        set_step_outputs(stepper->step_outbits);
        TimerEnable(PULSE_TIMER_BASE, TIMER_A);
    }

    if(spindle_sync.segment_id != stepper->exec_segment->id) {

        spindle_sync.segment_id = stepper->exec_segment->id;

        float dist = stepper->exec_segment->target_position - spindle_sync.prev_pos;

        float epulses = dist * spindle_sync.dpp;

        sys.pid_log.target[spindle_sync.segments] = stepper->exec_segment->target_position;

        spindle_sync.segments++;

 //       float current_pos = (spindleGetData(true).angular_position - spindle_sync.block_start) * stepper->exec_block->programmed_rate;

        spindle_sync.prev_pos = stepper->exec_segment->target_position;
    }
}
#endif

#if LASER_PPI

static void spindle_on ();

// Sets stepper direction and pulse pins and starts a step pulse with an initial delay
// When delayed pulse the step register is written in the step delay interrupt handler
static void stepperPulseStartPPI (stepper_t *stepper)
{
    static uint_fast16_t current_pwm = 0;

    if(stepper->new_block) {
        stepper->new_block = false;
        set_dir_outputs(stepper->dir_outbits);
        uint_fast16_t steps_per_pulse = stepper->exec_block->steps_per_mm * 25.4f / laser.ppi;
        if(laser.next_pulse && laser.steps_per_pulse)
            laser.next_pulse = laser.next_pulse * steps_per_pulse / laser.steps_per_pulse;
        laser.steps_per_pulse = steps_per_pulse;
    }

    if(stepper->step_outbits.value) {
        if(stepper->spindle_pwm != current_pwm) {
            current_pwm = spindle_set_speed(stepper->spindle_pwm);
            laser.next_pulse = 0;
        }

        if(laser.next_pulse == 0) {
            laser.next_pulse = laser.steps_per_pulse;
            if(current_pwm != spindle_pwm.off_value) {
                spindle_on();
                TimerEnable(LASER_PPI_TIMER_BASE, TIMER_A);
                // TODO: T2CCP0 - use timer timeout to switch off CCP output w/o using interrupt? single shot PWM?
            }
        } else
            laser.next_pulse--;

        set_step_outputs(stepper->step_outbits);
        TimerEnable(PULSE_TIMER_BASE, TIMER_A);
    }
}
#endif

// Enable/disable limit pins interrupt
static void limitsEnable (bool on, bool homing)
{
    if (on && settings.limits.flags.hard_enabled)
        GPIOIntEnable(LIMIT_PORT, HWLIMIT_MASK); // Enable Pin Change Interrupt
    else
        GPIOIntDisable(LIMIT_PORT, HWLIMIT_MASK); // Disable Pin Change Interrupt

#if TRINAMIC_ENABLE
    trinamic_homing(homing);
#endif
}

// Returns limit state as an axes_signals_t variable.
// Each bitfield bit indicates an axis limit, where triggered is 1 and not triggered is 0.
inline static axes_signals_t limitsGetState()
{
    uint32_t flags = GPIOPinRead(LIMIT_PORT, HWLIMIT_MASK);
    axes_signals_t signals;

    signals.x = (flags & X_LIMIT_PIN) != 0;
    signals.y = (flags & Y_LIMIT_PIN) != 0;
    signals.z = (flags & Z_LIMIT_PIN) != 0;

    if (settings.limits.invert.value)
        signals.value ^= settings.limits.invert.value;

    return signals;
}

// Returns system state as a control_signals_t variable.
// Each bitfield bit indicates a control signal, where triggered is 1 and not triggered is 0.
inline static control_signals_t systemGetState (void)
{
    uint32_t flags = GPIOPinRead(CONTROL_PORT, HWCONTROL_MASK);
    control_signals_t signals = {0};

    signals.reset = (flags & RESET_PIN) != 0;
    signals.safety_door_ajar = (flags & SAFETY_DOOR_PIN) != 0;
    signals.feed_hold = (flags & FEED_HOLD_PIN) != 0;
    signals.cycle_start = (flags & CYCLE_START_PIN) != 0;

    if(settings.control_invert.value)
        signals.value ^= settings.control_invert.value;

    return signals;
}

// Sets up the probe pin invert mask to
// appropriately set the pin logic according to setting for normal-high/normal-low operation
// and the probing cycle modes for toward-workpiece/away-from-workpiece.
static void probeConfigure(bool is_probe_away)
{
  probe_invert = settings.flags.invert_probe_pin ? 0 : PROBE_PIN;

  if (is_probe_away)
      probe_invert ^= PROBE_PIN;

  GPIOIntTypeSet(PROBE_PORT, PROBE_PIN, probe_invert ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
  GPIOIntEnable(PROBE_PORT, PROBE_PIN);

  probeState = (uint8_t)(GPIOPinRead(PROBE_PORT, PROBE_PIN)) ^ probe_invert != 0;
}

// Returns the probe pin state. Triggered = true.
bool probeGetState (void)
{   //return probeState; // TODO: check out using interrupt instead (we want to trap trigger and not risk losing it due to bouncing)
    return (((uint8_t)GPIOPinRead(PROBE_PORT, PROBE_PIN)) ^ probe_invert) != 0;
}

// Static spindle (off, on cw & on ccw)

inline static void spindle_off ()
{
    GPIOPinWrite(SPINDLE_ENABLE_PORT, SPINDLE_ENABLE_PIN, settings.spindle.invert.on ? SPINDLE_ENABLE_PIN : 0);
}

inline static void spindle_on ()
{
    GPIOPinWrite(SPINDLE_ENABLE_PORT, SPINDLE_ENABLE_PIN, settings.spindle.invert.on ? 0 : SPINDLE_ENABLE_PIN);
}

inline static void spindle_dir (bool ccw)
{
    GPIOPinWrite(SPINDLE_DIRECTION_PORT, SPINDLE_DIRECTION_PIN, (ccw ^ settings.spindle.invert.ccw) ? SPINDLE_DIRECTION_PIN : 0);
}


// Start or stop spindle
static void spindleSetState (spindle_state_t state, float rpm)
{
    if (!state.on)
        spindle_off();
    else {
        spindle_dir(state.ccw);
        spindle_on();
    }
}

// Variable spindle control functions

// Sets spindle speed
#if PWM_RAMPED

static uint_fast16_t spindle_set_speed (uint_fast16_t pwm_value)
{
    if (pwm_value == spindle_pwm.off_value) {
        pwm_ramp.pwm_target = 0;
        pwm_ramp.pwm_step = -SPINDLE_RAMP_STEP_INCR;
        pwm_ramp.delay.ms = 0;
        pwm_ramp.ms_cfg = SPINDLE_RAMP_STEP_TIME;
        SysTickEnable();
     } else {

        if(!pwmEnabled) {
            spindle_on();
            pwmEnabled = true;
            pwm_ramp.pwm_current = spindle_pwm.min_value;
            pwm_ramp.delay.ms = 0;
            TimerMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period - pwm_ramp.pwm_current + 15);
            TimerLoadSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period);
            TimerEnable(SPINDLE_PWM_TIMER_BASE, TIMER_A); // Ensure PWM output is enabled.
//            TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, false);
        }
        pwm_ramp.pwm_target = pwm_value;
        pwm_ramp.pwm_step = pwm_ramp.pwm_target < pwm_ramp.pwm_current ? -SPINDLE_RAMP_STEP_INCR : SPINDLE_RAMP_STEP_INCR;
        pwm_ramp.ms_cfg = SPINDLE_RAMP_STEP_TIME;
        TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, false);
        SysTickEnable();
    }

    return pwm_value;
}

#else

static void spindle_set_speed (uint_fast16_t pwm_value)
{
    if (pwm_value == spindle_pwm.off_value) {
        pwmEnabled = false;
        if(settings.spindle.disable_with_zero_speed)
            spindle_off();
        if(spindle_pwm.always_on) {
            TimerPrescaleMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.off_value >> 16);
            TimerMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.off_value & 0xFFFF);
            TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, !settings.spindle.invert.pwm);
            TimerEnable(SPINDLE_PWM_TIMER_BASE, TIMER_A); // Ensure PWM output is enabled.
        } else {
            uint_fast16_t pwm = spindle_pwm.period + 20000;
            TimerPrescaleSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, pwm >> 16);
            TimerLoadSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, pwm & 0xFFFF);
            if(!pwmEnabled)
                TimerEnable(SPINDLE_PWM_TIMER_BASE, TIMER_A);                                   // Ensure PWM output is enabled to
            TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, !settings.spindle.invert.pwm);   // ensure correct output level.
            TimerDisable(SPINDLE_PWM_TIMER_BASE, TIMER_A);                                      // Disable PWM.
        }
     } else {
        TimerPrescaleMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, pwm_value >> 16);
        TimerMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, pwm_value & 0xFFFF);
        if(!pwmEnabled) {
            spindle_on();
            pwmEnabled = true;
            TimerPrescaleSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period >> 16);
            TimerLoadSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period & 0xFFFF);
            TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, !settings.spindle.invert.pwm);
            TimerEnable(SPINDLE_PWM_TIMER_BASE, TIMER_A); // Ensure PWM output is enabled.
        }
    }
}

#endif

#ifdef SPINDLE_PWM_DIRECT

static uint_fast16_t spindleGetPWM (float rpm)
{
    return spindle_compute_pwm_value(&spindle_pwm, rpm, false);
}

#else

static void spindleUpdateRPM (float rpm)
{
    spindle_set_speed(spindle_compute_pwm_value(&spindle_pwm, rpm, false));
}

#endif

// Start or stop spindle
static void spindleSetStateVariable (spindle_state_t state, float rpm)
{
    if (!state.on || rpm == 0.0f) {
        spindle_set_speed(spindle_pwm.off_value);
        spindle_off();
    } else {
        spindle_dir(state.ccw);
        spindle_set_speed(spindle_compute_pwm_value(&spindle_pwm, rpm, false));
    }
}

#ifdef SPINDLE_SYNC_ENABLE
static spindle_data_t spindleGetData (spindle_data_request_t request)
{
    static spindle_data_t spindle_data;

    spindle_data.rpm = GPIOPinRead(SPINDLE_ENABLE_PORT, SPINDLE_ENABLE_PIN) ? 300.0f : 0.0f;
    spindle_data.angular_position = 0.0f;
    spindle_data.index_count++;

    return spindle_data;
}

static void spindleDataReset (void)
{
}

#endif

// Returns spindle state in a spindle_state_t variable
static spindle_state_t spindleGetState (void)
{
    spindle_state_t state = {0};

    state.on = GPIOPinRead(SPINDLE_ENABLE_PORT, SPINDLE_ENABLE_PIN) != 0;
    if(hal.driver_cap.spindle_dir)
        state.ccw = GPIOPinRead(SPINDLE_DIRECTION_PORT, SPINDLE_DIRECTION_PIN) != 0;
    state.value ^= settings.spindle.invert.mask;
    if(pwmEnabled)
        state.on |= pwmEnabled;
    state.value ^= settings.spindle.invert.mask;
#if PWM_RAMPED
    state.at_speed = pwm_ramp.pwm_current == pwm_ramp.pwm_target;
#endif
#ifdef SPINDLE_SYNC_ENABLE
    state.at_speed = spindleGetData(SpindleData_RPM).rpm == (state.on ? 300.0f : 0.0f);
#endif

    return state;
}

// end spindle code

// Start/stop coolant (and mist if enabled)
static void coolantSetState (coolant_state_t mode)
{
    mode.value ^= settings.coolant_invert.mask;
    GPIOPinWrite(COOLANT_FLOOD_PORT, COOLANT_FLOOD_PIN, mode.flood ? COOLANT_FLOOD_PIN : 0);
    GPIOPinWrite(COOLANT_MIST_PORT, COOLANT_MIST_PIN, mode.mist ? COOLANT_MIST_PIN : 0);
}

// Returns coolant state in a coolant_state_t variable
static coolant_state_t coolantGetState (void)
{
    coolant_state_t state = {0};

    state.flood = GPIOPinRead(COOLANT_FLOOD_PORT, COOLANT_FLOOD_PIN) != 0;
    state.mist  = GPIOPinRead(COOLANT_MIST_PORT, COOLANT_MIST_PIN) != 0;
    state.value ^= settings.coolant_invert.mask;

    return state;
}

static void showMessage (const char *msg)
{
    hal.stream.write("[MSG:");
    hal.stream.write(msg);
    hal.stream.write("]\r\n");
}

// Helper functions for setting/clearing/inverting individual bits atomically (uninterruptable)
static void bitsSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    IntMasterDisable();
    *ptr |= bits;
    IntMasterEnable();
}

static uint_fast16_t bitsClearAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    IntMasterDisable();
    uint_fast16_t prev = *ptr;
    *ptr &= ~bits;
    IntMasterEnable();
    return prev;
}

static uint_fast16_t valueSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t value)
{
    IntMasterDisable();
    uint_fast16_t prev = *ptr;
    *ptr = value;
    IntMasterEnable();
    return prev;
}

// Configures perhipherals when settings are initialized or changed
static void settings_changed (settings_t *settings)
{
    hal.driver_cap.variable_spindle = spindle_precompute_pwm_values(&spindle_pwm, SysCtlClockGet());

#if (STEP_OUTMODE == GPIO_MAP) || (DIRECTION_OUTMODE == GPIO_MAP)
    uint8_t i;
#endif

#if STEP_OUTMODE == GPIO_MAP
    for(i = 0; i < sizeof(step_outmap); i++)
        step_outmap[i] = c_step_outmap[i] ^ c_step_outmap[settings->step_invert.mask];
#endif

#if DIRECTION_OUTMODE == GPIO_MAP
    for(i = 0; i < sizeof(dir_outmap); i++)
        dir_outmap[i] = c_dir_outmap[i] ^ c_dir_outmap[settings->steppers.dir_invert.mask];
#endif

    if(IOInitDone) {

      #if TRINAMIC_ENABLE
        trinamic_configure();
      #endif

        stepperEnable(settings->steppers.deenergize);

        if(hal.driver_cap.variable_spindle) {
            TimerPrescaleSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period >> 16);
            TimerLoadSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period & 0xFFFF);
            hal.spindle_set_state = spindleSetStateVariable;
        } else
            hal.spindle_set_state = spindleSetState;

        if(settings->steppers.pulse_delay_microseconds) {
            TimerIntRegister(PULSE_TIMER_BASE, TIMER_A, stepper_pulse_isr_delayed);
            TimerMatchSet(PULSE_TIMER_BASE, TIMER_A, settings->steppers.pulse_microseconds);
            TimerLoadSet(PULSE_TIMER_BASE, TIMER_A, settings->steppers.pulse_microseconds + settings->steppers.pulse_delay_microseconds - 1);
            TimerIntEnable(PULSE_TIMER_BASE, TIMER_TIMA_TIMEOUT|TIMER_TIMA_MATCH);
            hal.stepper_pulse_start = stepperPulseStartDelayed;
        } else {
            TimerIntRegister(PULSE_TIMER_BASE, TIMER_A, stepper_pulse_isr);
            TimerLoadSet(PULSE_TIMER_BASE, TIMER_A, settings->steppers.pulse_microseconds - 1);
            TimerIntEnable(PULSE_TIMER_BASE, TIMER_TIMA_TIMEOUT);
            hal.stepper_pulse_start = stepperPulseStart;
        }

      #if LASER_PPI
        if(!settings->flags.laser_mode)
            laser_ppi_mode(false);
      #endif

       /*************************
        *  Control pins config  *
        ************************/

        control_signals_t control_fei;
        control_fei.mask = settings->control_disable_pullup.mask ^ settings->control_invert.mask;

        GPIOIntDisable(CONTROL_PORT, HWCONTROL_MASK);    // Disable pin change interrupt

        GPIOPadConfigSet(CONTROL_PORT, CYCLE_START_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.cycle_start ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
        GPIOPadConfigSet(CONTROL_PORT, FEED_HOLD_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.feed_hold ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
        GPIOPadConfigSet(CONTROL_PORT, RESET_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.reset ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
        GPIOPadConfigSet(CONTROL_PORT, SAFETY_DOOR_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.safety_door_ajar ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);

        GPIOIntTypeSet(CONTROL_PORT, CYCLE_START_PIN, control_fei.cycle_start ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
        GPIOIntTypeSet(CONTROL_PORT, FEED_HOLD_PIN, control_fei.feed_hold ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
        GPIOIntTypeSet(CONTROL_PORT, RESET_PIN, control_fei.reset ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
        GPIOIntTypeSet(CONTROL_PORT, SAFETY_DOOR_PIN, control_fei.safety_door_ajar ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);

        GPIOIntClear(CONTROL_PORT, HWCONTROL_MASK);     // Clear any pending interrupt
        GPIOIntEnable(CONTROL_PORT, HWCONTROL_MASK);    // and enable pin change interrupt

       /***********************
        *  Limit pins config  *
        ***********************/

        axes_signals_t limit_fei;
        limit_fei.mask = settings->limits.disable_pullup.mask ^ settings->limits.invert.mask;

        // Configure pullup/pulldown
        GPIOPadConfigSet(LIMIT_PORT, X_LIMIT_PIN, GPIO_STRENGTH_2MA, settings->limits.disable_pullup.x ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
        GPIOPadConfigSet(LIMIT_PORT, Y_LIMIT_PIN, GPIO_STRENGTH_2MA, settings->limits.disable_pullup.y ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
        GPIOPadConfigSet(LIMIT_PORT, Z_LIMIT_PIN, GPIO_STRENGTH_2MA, settings->limits.disable_pullup.z ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);

        // Configure interrupts
        GPIOIntTypeSet(LIMIT_PORT, X_LIMIT_PIN, limit_fei.x ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
        GPIOIntTypeSet(LIMIT_PORT, Y_LIMIT_PIN, limit_fei.y ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
        GPIOIntTypeSet(LIMIT_PORT, Z_LIMIT_PIN, limit_fei.z ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);

       /********************
        *  Probe pin init  *
        ********************/

        GPIOPadConfigSet(PROBE_PORT, PROBE_PIN, GPIO_STRENGTH_2MA, hal.driver_cap.probe_pull_up ? GPIO_PIN_TYPE_STD_WPU : GPIO_PIN_TYPE_STD_WPD);
    }
}

// Initializes MCU peripherals for Grbl use
static bool driver_setup (settings_t *settings)
{

#ifdef DRIVER_SETTINGS
    if(hal.eeprom.driver_area.address != 0) {
        if(!hal.eeprom.memcpy_from_with_checksum((uint8_t *)&driver_settings, hal.eeprom.driver_area.address, sizeof(driver_settings)))
            hal.driver_settings_restore();
      #if TRINAMIC_ENABLE && CNC_BOOSTERPACK // Trinamic BoosterPack does not support mixed drivers
        driver_settings.trinamic.driver_enable.mask = AXES_BITMASK;
      #endif
    }
#endif

    // System init

#ifndef BACKCHANNEL
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
#endif
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    SysCtlPeripheralEnable(STEPPER_TIMER_PERIPH);
    SysCtlPeripheralEnable(PULSE_TIMER_PERIPH);

    SysCtlDelay(26); // wait a bit for peripherals to wake up

    /******************
     *  Stepper init  *
     ******************/

    // Unlock GPIOF0, used for stepper disable Z control
    HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY;
    HWREG(GPIO_PORTF_BASE + GPIO_O_CR) |= GPIO_PIN_0;
    HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = 0;

    GPIOPinTypeGPIOOutput(STEP_PORT, HWSTEP_MASK);
    GPIOPadConfigSet(STEP_PORT, HWSTEP_MASK, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

    GPIOPinTypeGPIOOutput(DIRECTION_PORT, HWDIRECTION_MASK);
    GPIOPadConfigSet(DIRECTION_PORT, HWDIRECTION_MASK, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

#if !TRINAMIC_ENABLE
#if CNC_BOOSTERPACK
    GPIOPinTypeGPIOOutput(STEPPERS_DISABLE_XY_PORT, STEPPERS_DISABLE_XY_PIN);
    GPIOPinTypeGPIOOutput(STEPPERS_DISABLE_Z_PORT, STEPPERS_DISABLE_Z_PIN);
#else
    GPIOPinTypeGPIOOutput(STEPPERS_DISABLE_PORT, STEPPERS_DISABLE_PIN);
#endif
#endif

    // Configure stepper driver timer
    TimerConfigure(STEPPER_TIMER_BASE, TIMER_CFG_SPLIT_PAIR|TIMER_CFG_A_PERIODIC);
    IntPrioritySet(STEPPER_TIMER_INT, 0x20); // lower priority than for Timer2 (which resets the step-dir signal)
    TimerControlStall(STEPPER_TIMER_BASE, TIMER_A, true); //timer1 will stall in debug mode
    TimerIntRegister(STEPPER_TIMER_BASE, TIMER_A, stepper_driver_isr);
    TimerIntClear(STEPPER_TIMER_BASE, 0xFFFF);
    IntPendClear(STEPPER_TIMER_INT);
    TimerPrescaleSet(STEPPER_TIMER_BASE, TIMER_A, STEPPER_DRIVER_PRESCALER); // 20 MHz clock
    TimerIntEnable(STEPPER_TIMER_BASE, TIMER_TIMA_TIMEOUT);

    // Configure step pulse timer
//  TimerClockSourceSet(PULSE_TIMER_BASE, TIMER_CLOCK_SYSTEM);
    TimerConfigure(PULSE_TIMER_BASE, TIMER_CFG_SPLIT_PAIR|TIMER_CFG_A_ONE_SHOT);
    IntPrioritySet(PULSE_TIMER_INT, 0x00); // highest priority - higher than for Timer1 (which sets the step-dir output)
    TimerControlStall(PULSE_TIMER_BASE, TIMER_A, true); //timer2 will stall in debug mode
    TimerIntClear(PULSE_TIMER_BASE, 0xFFFF);
    IntPendClear(PULSE_TIMER_INT);
    TimerPrescaleSet(PULSE_TIMER_BASE, TIMER_A, 79); // for 1uS per count

#if CNC_BOOSTERPACK_A4998
    GPIOPinTypeGPIOOutput(STEPPERS_VDD_PORT, STEPPERS_VDD_PIN);
    GPIOPadConfigSet(STEPPERS_VDD_PORT, STEPPERS_VDD_PIN, GPIO_STRENGTH_12MA, GPIO_PIN_TYPE_STD);
    GPIOPinWrite(STEPPERS_VDD_PORT, STEPPERS_VDD_PIN, STEPPERS_VDD_PIN);
#endif

#if LASER_PPI

   /********************************
    *  PPI mode pulse width timer  *
    ********************************/

    laser.ppi = 600.0f;
    laser.pulse_length = 1500;

    SysCtlPeripheralEnable(LASER_PPI_TIMER_PERIPH);
    SysCtlDelay(26); // wait a bit for peripherals to wake up
    TimerConfigure(LASER_PPI_TIMER_BASE, TIMER_CFG_SPLIT_PAIR|TIMER_CFG_A_ONE_SHOT);
    IntPrioritySet(LASER_PPI_TIMER_INT, 0x40); // lower priority than for Timer2 (which resets the step-dir signal)
    TimerControlStall(LASER_PPI_TIMER_BASE, TIMER_A, true); //TIMER5 will stall in debug mode
    TimerIntClear(LASER_PPI_TIMER_BASE, 0xFFFF);
    IntPendClear(LASER_PPI_TIMER_INT);
    TimerPrescaleSet(LASER_PPI_TIMER_BASE, TIMER_A, 79); // for 1uS per count
    TimerIntRegister(LASER_PPI_TIMER_BASE, TIMER_A, ppi_timeout_isr);
    TimerLoadSet(LASER_PPI_TIMER_BASE, TIMER_A, laser.pulse_length);
    TimerIntEnable(LASER_PPI_TIMER_BASE, TIMER_TIMA_TIMEOUT|TIMER_TIMA_MATCH);

#endif

   /****************************
    *  Software debounce init  *
    ****************************/

    if(hal.driver_cap.software_debounce) {
        SysCtlPeripheralEnable(DEBOUNCE_TIMER_PERIPH);
        SysCtlDelay(26); // wait a bit for peripherals to wake up
        IntPrioritySet(DEBOUNCE_TIMER_INT, 0x40); // lower priority than for Timer2 (which resets the step-dir signal)
        TimerConfigure(DEBOUNCE_TIMER_BASE, TIMER_CFG_SPLIT_PAIR|TIMER_CFG_A_ONE_SHOT);
        TimerControlStall(DEBOUNCE_TIMER_BASE, TIMER_A, true); //timer2 will stall in debug mode
        TimerIntRegister(DEBOUNCE_TIMER_BASE, TIMER_A, software_debounce_isr);
        TimerIntClear(DEBOUNCE_TIMER_BASE, 0xFFFF);
        IntPendClear(DEBOUNCE_TIMER_INT);
        TimerPrescaleSet(DEBOUNCE_TIMER_BASE, TIMER_A, 79); // configure for 1us per count
        TimerLoadSet(DEBOUNCE_TIMER_BASE, TIMER_A, 32000);  // and for a total of 32ms
        TimerIntEnable(DEBOUNCE_TIMER_BASE, TIMER_TIMA_TIMEOUT);
    }

   /***********************
    *  Control pins init  *
    ***********************/

    GPIOPinTypeGPIOInput(CONTROL_PORT, HWCONTROL_MASK);
    GPIOIntRegister(CONTROL_PORT, control_isr);             // Register interrupt handler

    GPIOPadConfigSet(CONTROL_PORT, CYCLE_START_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.cycle_start ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
    GPIOPadConfigSet(CONTROL_PORT, FEED_HOLD_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.feed_hold ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
    GPIOPadConfigSet(CONTROL_PORT, RESET_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.reset ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
    GPIOPadConfigSet(CONTROL_PORT, SAFETY_DOOR_PIN, GPIO_STRENGTH_2MA, settings->control_disable_pullup.safety_door_ajar ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);

    GPIOIntTypeSet(CONTROL_PORT, CYCLE_START_PIN, settings->control_invert.cycle_start ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
    GPIOIntTypeSet(CONTROL_PORT, FEED_HOLD_PIN, settings->control_invert.feed_hold ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
    GPIOIntTypeSet(CONTROL_PORT, RESET_PIN, settings->control_invert.reset ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
    GPIOIntTypeSet(CONTROL_PORT, SAFETY_DOOR_PIN, settings->control_invert.safety_door_ajar ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);

    GPIOIntClear(CONTROL_PORT, HWCONTROL_MASK);     // Clear any pending interrupt
    GPIOIntEnable(CONTROL_PORT, HWCONTROL_MASK);    // and enable pin change interrupt

   /*********************
    *  Limit pins init  *
    *********************/

    GPIOPinTypeGPIOInput(LIMIT_PORT, HWLIMIT_MASK);
    GPIOIntRegister(LIMIT_PORT, hal.driver_cap.software_debounce ? limit_isr_debounced : limit_isr); // Register a call-back funcion for interrupt

    // Configure pullup/pulldown
    GPIOPadConfigSet(LIMIT_PORT, X_LIMIT_PIN, GPIO_STRENGTH_2MA, settings->limits.disable_pullup.x ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
    GPIOPadConfigSet(LIMIT_PORT, Y_LIMIT_PIN, GPIO_STRENGTH_2MA, settings->limits.disable_pullup.y ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);
    GPIOPadConfigSet(LIMIT_PORT, Z_LIMIT_PIN, GPIO_STRENGTH_2MA, settings->limits.disable_pullup.z ? GPIO_PIN_TYPE_STD_WPD : GPIO_PIN_TYPE_STD_WPU);

    // Configure interrupts
    GPIOIntTypeSet(LIMIT_PORT, X_LIMIT_PIN, settings->limits.invert.x ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
    GPIOIntTypeSet(LIMIT_PORT, Y_LIMIT_PIN, settings->limits.invert.y ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);
    GPIOIntTypeSet(LIMIT_PORT, Z_LIMIT_PIN, settings->limits.invert.z ? GPIO_FALLING_EDGE : GPIO_RISING_EDGE);

   /********************
    *  Probe pin init  *
    ********************/

    GPIOPinTypeGPIOInput(PROBE_PORT, PROBE_PIN);
    GPIOPadConfigSet(PROBE_PORT, PROBE_PIN, GPIO_STRENGTH_2MA, hal.driver_cap.probe_pull_up ? GPIO_PIN_TYPE_STD_WPU : GPIO_PIN_TYPE_STD_WPD);

   /***********************
    *  Coolant pins init  *
    ***********************/

    // Unlock GPIOD7, used for mist control
    HWREG(GPIO_PORTD_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY;
    HWREG(GPIO_PORTD_BASE + GPIO_O_CR) |= GPIO_PIN_7;
    HWREG(GPIO_PORTD_BASE + GPIO_O_LOCK) = 0;

    GPIOPinTypeGPIOOutput(COOLANT_FLOOD_PORT, COOLANT_FLOOD_PIN);
    GPIOPinTypeGPIOOutput(COOLANT_MIST_PORT, COOLANT_MIST_PIN);
    GPIOPadConfigSet(COOLANT_FLOOD_PORT, COOLANT_FLOOD_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    GPIOPadConfigSet(COOLANT_MIST_PORT, COOLANT_MIST_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

   /******************
    *  Spindle init  *
    ******************/

    GPIOPinTypeGPIOOutput(SPINDLE_ENABLE_PORT, SPINDLE_ENABLE_PIN);
    GPIOPadConfigSet(SPINDLE_ENABLE_PORT, SPINDLE_ENABLE_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

    GPIOPinTypeGPIOOutput(SPINDLE_DIRECTION_PORT, SPINDLE_DIRECTION_PIN);
    GPIOPadConfigSet(SPINDLE_DIRECTION_PORT, SPINDLE_DIRECTION_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

    SysCtlPeripheralEnable(SPINDLE_PWM_TIMER_PERIPH);
    SysCtlDelay(26); // wait a bit for peripherals to wake up
    TimerClockSourceSet(SPINDLE_PWM_TIMER_BASE, TIMER_CLOCK_SYSTEM);
    TimerConfigure(SPINDLE_PWM_TIMER_BASE, TIMER_CFG_SPLIT_PAIR|TIMER_CFG_A_PWM);
//      TimerControlStall(SPINDLE_PWM_TIMER_BASE, TIMER_A, false); //timer1 will stall in debug mode
//      TimerPrescaleSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, STEPPER_DRIVER_PRESCALER); // 20 MHz clock
//      TimerPrescaleMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, STEPPER_DRIVER_PRESCALER);
    TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, false);
    GPIOPinConfigure(SPINDLEPWM_MAP);
    GPIOPinTypeTimer(SPINDLEPPORT, SPINDLEPPIN);
    GPIOPadConfigSet(SPINDLEPPORT, SPINDLEPPIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
  #if PWM_RAMPED
    pwm_ramp.ms_cfg = pwm_ramp.pwm_current = pwm_ramp.pwm_target = 0;
  #endif

#if KEYPAD_ENABLE

   /*********************
    *  I2C KeyPad init  *
    *********************/

    GPIOPinTypeGPIOInput(KEYINTR_PORT, KEYINTR_PIN);
    GPIOPadConfigSet(KEYINTR_PORT, KEYINTR_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU); // -> WPU

    GPIOIntRegister(KEYINTR_PORT, keyclick_int_handler);
    GPIOIntTypeSet(KEYINTR_PORT, KEYINTR_PIN, GPIO_BOTH_EDGES);
    GPIOIntEnable(KEYINTR_PORT, KEYINTR_PIN);

#endif

#if TRINAMIC_ENABLE

    trinamic_init();

    // Configure input pin for DIAG1 signal (with pullup) and enable interrupt
    GPIOPinTypeGPIOInput(TRINAMIC_DIAG_IRQ_PORT, TRINAMIC_DIAG_IRQ_PIN);
  #if !KEYPAD_ENABLE
    GPIOIntRegister(TRINAMIC_DIAG_IRQ_PORT, trinamic_diag1_isr); // Register a call-back function for interrupt
  #endif
    GPIOPadConfigSet(TRINAMIC_DIAG_IRQ_PORT, TRINAMIC_DIAG_IRQ_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
    GPIOIntTypeSet(TRINAMIC_DIAG_IRQ_PORT, TRINAMIC_DIAG_IRQ_PIN, GPIO_FALLING_EDGE);
    GPIOIntEnable(TRINAMIC_DIAG_IRQ_PORT, TRINAMIC_DIAG_IRQ_PIN);

  #if TRINAMIC_I2C
  // Configure input pin for WARN signal (with pullup) and enable interrupt
    GPIOPinTypeGPIOInput(TRINAMIC_WARN_IRQ_PORT, TRINAMIC_WARN_IRQ_PIN);
  #if CNC_BOOSTERPACK_SHORTS
    GPIOIntRegister(TRINAMIC_WARN_IRQ_PORT, trinamic_warn_isr); // Register a call-back function for interrupt
  #endif
    GPIOPadConfigSet(TRINAMIC_WARN_IRQ_PORT, TRINAMIC_WARN_IRQ_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
    GPIOIntTypeSet(TRINAMIC_WARN_IRQ_PORT, TRINAMIC_WARN_IRQ_PIN, GPIO_FALLING_EDGE);
    GPIOIntEnable(TRINAMIC_WARN_IRQ_PORT, TRINAMIC_WARN_IRQ_PIN);
  #endif

#endif

  // Set defaults

    IOInitDone = settings->version == 15;

    settings_changed(settings);

    hal.stepper_go_idle(true);
    hal.spindle_set_state((spindle_state_t){0}, 0.0f);
    hal.coolant_set_state((coolant_state_t){0});

    return IOInitDone;
}

#ifdef DRIVER_SETTINGS

static status_code_t driver_setting (setting_type_t param, float value, char *svalue)
{
    status_code_t status = Status_Unhandled;

#if KEYPAD_ENABLE
    status = keypad_setting(param, value, svalue);
#endif

#if TRINAMIC_ENABLE
    if(status == Status_Unhandled)
        status = trinamic_setting(param, value, svalue);
#endif

    if(status == Status_OK)
        hal.eeprom.memcpy_to_with_checksum(hal.eeprom.driver_area.address, (uint8_t *)&driver_settings, sizeof(driver_settings));

    return status;
}

static void driver_settings_report (setting_type_t setting)
{
#if KEYPAD_ENABLE
    keypad_settings_report(setting);
#endif
#if TRINAMIC_ENABLE
    trinamic_settings_report(setting);
#endif
}

void driver_settings_restore (void)
{
#if KEYPAD_ENABLE
    keypad_settings_restore();
#endif
#if TRINAMIC_ENABLE
    trinamic_settings_restore();
#endif
    hal.eeprom.memcpy_to_with_checksum(hal.eeprom.driver_area.address, (uint8_t *)&driver_settings, sizeof(driver_settings));
}

#endif

// Initialize HAL pointers, setup serial comms and enable EEPROM
// NOTE: Grbl is not yet configured (from EEPROM data), driver_setup() will be called when done
bool driver_init (void)
{
    // Set up systick timer with a 1ms period
    SysTickPeriodSet((SysCtlClockGet() / 1000) - 1);
    SysTickIntRegister(systick_isr);
    IntPrioritySet(FAULT_SYSTICK, 0x40);
    SysTickIntEnable();
    SysTickEnable();

    // Enable EEPROM and serial port here for Grbl to be able to configure itself and report any errors
    SysCtlPeripheralEnable(SYSCTL_PERIPH_EEPROM0);
    SysCtlDelay(26); // wait a bit for peripheral to wake up
    EEPROMInit();

    SysCtlPeripheralEnable(SYSCTL_PERIPH_HIBERNATE);
    HibernateEnableExpClk(SysCtlClockGet());
    HibernateClockConfig(HIBERNATE_OSC_LOWDRIVE);
    HibernateRTCEnable();

    hal.f_step_timer = SysCtlPIOSCCalibrate(SYSCTL_PIOSC_CAL_AUTO);

    serialInit();

#if KEYPAD_ENABLE || (TRINAMIC_ENABLE && TRINAMIC_I2C)
    I2CInit();
#endif

    hal.info = "TM4C123HP6PM";
    hal.driver_version = "200120";
    hal.driver_setup = driver_setup;
    hal.f_step_timer = SysCtlClockGet() / (STEPPER_DRIVER_PRESCALER + 1); // 20 MHz
    hal.rx_buffer_size = RX_BUFFER_SIZE;
    hal.delay_ms = driver_delay_ms;
    hal.settings_changed = settings_changed;

    hal.stepper_wake_up = stepperWakeUp;
    hal.stepper_go_idle = stepperGoIdle;
    hal.stepper_enable = stepperEnable;
    hal.stepper_cycles_per_tick = stepperCyclesPerTick;
    hal.stepper_pulse_start = stepperPulseStart;

    hal.limits_enable = limitsEnable;
    hal.limits_get_state = limitsGetState;

    hal.coolant_set_state = coolantSetState;
    hal.coolant_get_state = coolantGetState;

    hal.probe_get_state = probeGetState;
    hal.probe_configure_invert_mask = probeConfigure;

    hal.spindle_set_state = spindleSetState;
    hal.spindle_get_state = spindleGetState;
#ifdef SPINDLE_PWM_DIRECT
    hal.spindle_get_pwm = spindleGetPWM;
    hal.spindle_update_pwm = spindle_set_speed;
#else
    hal.spindle_update_rpm = spindleUpdateRPM;
#endif
#ifdef SPINDLE_SYNC_ENABLE
    hal.spindle_get_data = spindleGetData;
    hal.spindle_reset_data = spindleDataReset;
#endif

    hal.system_control_get_state = systemGetState;

    hal.stream.read = serialGetC;
    hal.stream.write = serialWriteS;
    hal.stream.write_all = serialWriteS;
    hal.stream.get_rx_buffer_available = serialRxFree;
    hal.stream.reset_read_buffer = serialRxFlush;
    hal.stream.cancel_read_buffer = serialRxCancel;
    hal.stream.suspend_read = serialSuspendInput;

    hal.eeprom.type = EEPROM_Physical;
    hal.eeprom.get_byte = eepromGetByte;
    hal.eeprom.put_byte = eepromPutByte;
    hal.eeprom.memcpy_to_with_checksum = eepromWriteBlockWithChecksum;
    hal.eeprom.memcpy_from_with_checksum = eepromReadBlockWithChecksum;

#ifdef DRIVER_SETTINGS

  #if !TRINAMIC_ENABLE
    assert(EEPROM_ADDR_TOOL_TABLE - (sizeof(driver_settings_t) + 2) > EEPROM_ADDR_GLOBAL + sizeof(settings_t) + 1);
    hal.eeprom.driver_area.address = EEPROM_ADDR_TOOL_TABLE - (sizeof(driver_settings_t) + 2);
  #else
    hal.eeprom.driver_area.address = 1024;
    hal.eeprom.size = GRBL_EEPROM_SIZE + sizeof(driver_settings_t) + 1;
  #endif
    hal.eeprom.driver_area.size = sizeof(driver_settings_t);
    hal.driver_setting = driver_setting;
    hal.driver_settings_report = driver_settings_report;
    hal.driver_settings_restore = driver_settings_restore;
#endif

#if TRINAMIC_ENABLE
    hal.user_mcode_check = trinamic_MCodeCheck;
    hal.user_mcode_validate = trinamic_MCodeValidate;
    hal.user_mcode_execute = trinamic_MCodeExecute;
    hal.driver_rt_report = trinamic_RTReport;
    hal.driver_axis_settings_report = trinamic_axis_settings_report;
#endif

    hal.set_bits_atomic = bitsSetAtomic;
    hal.clear_bits_atomic = bitsClearAtomic;
    hal.set_value_atomic = valueSetAtomic;

#ifdef _USERMCODES_H_
    hal.driver_mcode_check = userMCodeCheck;
    hal.driver_mcode_validate = userMCodeValidate;
    hal.driver_mcode_execute = userMCodeExecute;
#endif

    hal.show_message = showMessage;

#if KEYPAD_ENABLE
    hal.execute_realtime = keypad_process_keypress;
    hal.driver_setting = driver_setting;
    hal.driver_settings_restore = driver_settings_restore;
    hal.driver_settings_report = driver_settings_report;
#endif

#ifdef _ATC_H_
    hal.tool_select = atc_tool_selected;
    hal.tool_change = atc_tool_change;
#endif

  // driver capabilities, used for announcing and negotiating (with Grbl) driver functionality

    hal.driver_cap.spindle_dir = On;
    hal.driver_cap.variable_spindle = On;
    hal.driver_cap.spindle_pwm_invert = On;
#if PWM_RAMPED
    hal.driver_cap.spindle_at_speed = On;
#endif
#ifdef SPINDLE_SYNC_ENABLE
    hal.driver_cap.spindle_sync = On;
    hal.driver_cap.spindle_at_speed = On;
#endif
    hal.driver_cap.mist_control = On;
    hal.driver_cap.software_debounce = On;
    hal.driver_cap.step_pulse_delay = On;
    hal.driver_cap.amass_level = 3;
    hal.driver_cap.control_pull_up = On;
    hal.driver_cap.limits_pull_up = On;
    hal.driver_cap.probe_pull_up = On;
#if LASER_PPI
    hal.driver_cap.laser_ppi_mode = On;
#endif

    // No need to move version check before init.
    // Compiler will fail any signature mismatch for existing entries.
    return hal.version == 6;
}

/* interrupt handlers */

// Main stepper driver
static void stepper_driver_isr (void)
{
    TimerIntClear(STEPPER_TIMER_BASE, TIMER_TIMA_TIMEOUT); // clear interrupt flag
    hal.stepper_interrupt_callback();
}

/* The Stepper Port Reset Interrupt: This interrupt handles the falling edge of the step
   pulse. This should always trigger before the next general stepper driver interrupt and independently
   finish, if stepper driver interrupts is disabled after completing a move.
   NOTE: Interrupt collisions between the serial and stepper interrupts can cause delays by
   a few microseconds, if they execute right before one another. Not a big deal, but can
   cause issues at high step rates if another high frequency asynchronous interrupt is
   added to Grbl.
*/
// This interrupt is enabled when Grbl sets the motor port bits to execute
// a step. This ISR resets the motor port after a short period (settings.pulse_microseconds)
// completing one step cycle.
// NOTE: TivaC has a shared interrupt for match and timeout
static void stepper_pulse_isr (void)
{
    TimerIntClear(PULSE_TIMER_BASE, TIMER_TIMA_TIMEOUT); // Clear interrupt flag
    set_step_outputs((axes_signals_t){0});
}

static void stepper_pulse_isr_delayed (void)
{
    uint32_t iflags = TimerIntStatus(PULSE_TIMER_BASE, true);
    TimerIntClear(PULSE_TIMER_BASE, iflags);        // clear interrupt flags
    set_step_outputs(iflags & TIMER_TIMA_MATCH ? next_step_outbits : (axes_signals_t){0});
}

static void software_debounce_isr (void)
{
    TimerIntClear(DEBOUNCE_TIMER_BASE, TIMER_TIMA_TIMEOUT); // clear interrupt flag

    axes_signals_t state = limitsGetState();

    if(state.value) //TODO: add check for limit swicthes having same state as when limit_isr were invoked?
        hal.limit_interrupt_callback(state);
}

#if LASER_PPI

void laser_ppi_mode (bool on)
{
    if(on)
        hal.stepper_pulse_start = stepperPulseStartPPI;
    else
        hal.stepper_pulse_start = settings.steppers.pulse_delay_microseconds ? stepperPulseStartDelayed : stepperPulseStart;
    gc_set_laser_ppimode(on);
}

// Switches off the spindle (laser) after laser.pulse_length time has elapsed
static void ppi_timeout_isr (void)
{
    TimerIntClear(LASER_PPI_TIMER_BASE, TIMER_TIMA_TIMEOUT); // clear interrupt flag
    spindle_off();
}
#endif

#if TRINAMIC_ENABLE && TRINAMIC_I2C
static void trinamic_warn_isr (void)
{
    uint32_t iflags = GPIOIntStatus(TRINAMIC_WARN_IRQ_PORT, true);

    GPIOIntClear(TRINAMIC_WARN_IRQ_PORT, iflags);

    if(iflags & TRINAMIC_WARN_IRQ_PIN)
        trinamic_warn_handler();
}
#endif

static void limit_isr (void)
{
    uint32_t iflags = GPIOIntStatus(LIMIT_PORT, true);

    GPIOIntClear(LIMIT_PORT, iflags);
    if(iflags & HWLIMIT_MASK)
        hal.limit_interrupt_callback(limitsGetState());
    else if(iflags & PROBE_PIN)
        probeState = probe_invert != 0;
}

static void limit_isr_debounced (void)
{
    uint32_t iflags = GPIOIntStatus(LIMIT_PORT, true);

    GPIOIntClear(LIMIT_PORT, iflags);
    if(iflags & HWLIMIT_MASK) {
        // TODO: disable interrups here and reenable in software_debounce_isr?
        TimerLoadSet(DEBOUNCE_TIMER_BASE, TIMER_A, 32000);  // 32ms
        TimerEnable(DEBOUNCE_TIMER_BASE, TIMER_A);
    } else if(iflags & PROBE_PIN)
        probeState = probe_invert != 0;
}

static void control_isr (void)
{
// No debounce??
    uint32_t iflags = GPIOIntStatus(CONTROL_PORT, true) & HWCONTROL_MASK;

    if(iflags) {
        GPIOIntClear(CONTROL_PORT, iflags);
        hal.control_interrupt_callback(systemGetState());
    }
}


#if KEYPAD_ENABLE

static void keyclick_int_handler (void)
{
    uint32_t iflags = GPIOIntStatus(KEYINTR_PORT, true);

    GPIOIntClear(KEYINTR_PORT, iflags);

    if(iflags & KEYINTR_PIN)
        keypad_keyclick_handler(GPIOPinRead(KEYINTR_PORT, KEYINTR_PIN) != 0);
  #if TRINAMIC_ENABLE
    else if(iflags & TRINAMIC_DIAG_IRQ_PIN)
      trinamic_fault_handler();
  #endif
}
#elif TRINAMIC_ENABLE

static void trinamic_diag1_isr (void)
{
    uint32_t iflags = GPIOIntStatus(TRINAMIC_DIAG_IRQ_PORT, true);

    GPIOIntClear(TRINAMIC_DIAG_IRQ_PORT, iflags);

    if(iflags & TRINAMIC_DIAG_IRQ_PIN)
        trinamic_fault_handler();
}

#endif

// Interrupt handler for 1 ms interval timer
#if PWM_RAMPED
static void systick_isr (void)
{
    if(pwm_ramp.ms_cfg) {
        if(++pwm_ramp.delay.ms == pwm_ramp.ms_cfg) {

            pwm_ramp.delay.ms = 0;
            pwm_ramp.pwm_current += pwm_ramp.pwm_step;

            if(pwm_ramp.pwm_step < 0) { // decrease speed

                if(pwm_ramp.pwm_current < pwm_ramp.pwm_target)
                    pwm_ramp.pwm_current = pwm_ramp.pwm_target;

                if(pwm_ramp.pwm_current == 0) { // stop?
                    if(settings.spindle.disable_with_zero_speed)
                        spindle_off();
                    TimerLoadSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period + 20000);
                    TimerDisable(SPINDLE_PWM_TIMER_BASE, TIMER_A); // Disable PWM. Output voltage is zero.
                    if(pwmEnabled)
                        TimerControlLevel(SPINDLE_PWM_TIMER_BASE, TIMER_A, true);
                    pwmEnabled = false;
                } else
                    TimerMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period - pwm_ramp.pwm_current); // use LUT?
            } else {
                 if(pwm_ramp.pwm_current > pwm_ramp.pwm_target)
                     pwm_ramp.pwm_current = pwm_ramp.pwm_target;
                TimerMatchSet(SPINDLE_PWM_TIMER_BASE, TIMER_A, spindle_pwm.period - pwm_ramp.pwm_current); // use LUT?
            }
            if(pwm_ramp.pwm_current == pwm_ramp.pwm_target)
                pwm_ramp.ms_cfg = 0;
        }
    }

    if(delay.ms && !(--delay.ms) && delay.callback) {
        delay.callback();
        delay.callback = 0;
    }

    if(!(delay.ms || pwm_ramp.ms_cfg))
        SysTickDisable();
}
#else
static void systick_isr (void)
{
    if(!(--delay.ms)) {
        SysTickDisable();
        if(delay.callback) {
            delay.callback();
            delay.callback = NULL;
        }
    }
}
#endif
