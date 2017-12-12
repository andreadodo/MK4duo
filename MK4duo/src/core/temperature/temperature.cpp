/**
 * MK4duo Firmware for 3D Printer, Laser and CNC
 *
 * Based on Marlin, Sprinter and grbl
 * Copyright (C) 2011 Camiel Gubbels / Erik van der Zalm
 * Copyright (C) 2013 Alberto Cotronei @MagoKimbra
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * temperature.cpp - temperature control
 */

#include "../../../MK4duo.h"

Temperature thermalManager;

constexpr uint16_t  temp_check_interval[HEATER_TYPE]  = { 0, BED_CHECK_INTERVAL, CHAMBER_CHECK_INTERVAL, COOLER_CHECK_INTERVAL };
constexpr bool      thermal_protection[HEATER_TYPE]   = { THERMAL_PROTECTION_HOTENDS, THERMAL_PROTECTION_BED, THERMAL_PROTECTION_CHAMBER, THERMAL_PROTECTION_COOLER };

// public:

#if HAS_MCU_TEMPERATURE
  float   Temperature::mcu_current_temperature  = 0.0,
          Temperature::mcu_highest_temperature  = 0.0,
          Temperature::mcu_lowest_temperature   = 4096.0,
          Temperature::mcu_alarm_temperature    = 80.0;
  int16_t Temperature::mcu_current_temperature_raw;
#endif

#if ENABLED(TEMP_SENSOR_1_AS_REDUNDANT)
  float Temperature::redundant_temperature = 0.0;
#endif

#if HAS_TEMP_HOTEND && ENABLED(PREVENT_COLD_EXTRUSION)
  int16_t Temperature::extrude_min_temp   = EXTRUDE_MINTEMP;
#endif

// private:

uint8_t Temperature::cycle_1_second = 0;

#if ENABLED(PID_ADD_EXTRUSION_RATE)
  float Temperature::cTerm[HOTENDS];
  long  Temperature::last_e_position,
        Temperature::lpq[LPQ_MAX_LEN];
  int   Temperature::lpq_ptr = 0,
        Temperature::lpq_len = 20;
#endif

uint8_t Temperature::pid_pointer = 255;

millis_t Temperature::next_check_ms[HEATER_COUNT];

#if ENABLED(FILAMENT_SENSOR)
  int8_t    Temperature::meas_shift_index;          // Index of a delayed sample in buffer
  uint16_t  Temperature::current_raw_filwidth = 0;  // Measured filament diameter - one extruder only
#endif

#if ENABLED(PROBING_HEATERS_OFF)
  bool Temperature::paused;
#endif

// Public Function

/**
 * Initialize the temperature manager
 */
void Temperature::init() {

  #if (PIDTEMP) && ENABLED(PID_ADD_EXTRUSION_RATE)
    last_e_position = 0;
  #endif

  HAL::analogStart();

  // Wait for temperature measurement to settle
  HAL::delayMilliseconds(250);

  #if ENABLED(PROBING_HEATERS_OFF)
    paused = false;
  #endif
}

void Temperature::wait_heater(Heater *act, bool no_wait_for_cooling/*=true*/) {

  #if TEMP_RESIDENCY_TIME > 0
    millis_t residency_start_ms = 0;
    // Loop until the temperature has stabilized
    #define TEMP_CONDITIONS (!residency_start_ms || PENDING(now, residency_start_ms + (TEMP_RESIDENCY_TIME) * 1000UL))
  #else
    #define TEMP_CONDITIONS (wants_to_cool ? act->isCooling() : act->isHeating())
  #endif

  float     target_temp         = -1.0,
            old_temp            = 9999.0;
  bool      wants_to_cool       = false;
  millis_t  now,
            next_cool_check_ms  = 0;

  printer.setWaitForHeatUp(true);

  const bool oldReport = printer.isAutoreportTemp();
  printer.setAutoreportTemp(true);

  #if DISABLED(BUSY_WHILE_HEATING)
    KEEPALIVE_STATE(NOT_BUSY);
  #endif

  #if ENABLED(PRINTER_EVENT_LEDS)
    const float start_temp = act->current_temperature;
    uint8_t old_blue = 0;
    uint8_t old_red  = 255;
  #endif

  do {

    wants_to_cool = act->isCooling();
    // Exit if S<lower>, continue if S<higher>, R<lower>, or R<higher>
    if (no_wait_for_cooling && wants_to_cool) break;

    #if ENABLED(BUSY_WHILE_HEATING)
      KEEPALIVE_STATE(WAIT_HEATER);
    #endif

    printer.idle();
    commands.refresh_cmd_timeout(); // to prevent stepper.stepper_inactive_time from running out

    const float temp = act->current_temperature;

    #if ENABLED(PRINTER_EVENT_LEDS)
      if (!wants_to_cool) {
        if (act->type == IS_HOTEND) {
          // Gradually change LED strip from violet to red as nozzle heats up
          const uint8_t blue = map(constrain(temp, start_temp, target_temp), start_temp, target_temp, 255, 0);
          if (blue != old_blue) {
            old_blue = blue;
            set_led_color(255, 0, blue, 0, true);
          }
        }
        else if (act->type == IS_BED) {
          // Gradually change LED strip from blue to violet as bed heats up
          const uint8_t red = map(constrain(temp, start_temp, target_temp), start_temp, target_temp, 0, 255);
          if (red != old_red) {
            old_red = red;
            set_led_color(red, 0, 255, 0, true);
          }
        }
      }
    #endif

    #if TEMP_RESIDENCY_TIME > 0

      const float temp_diff = FABS(target_temp - temp);

      if (!residency_start_ms) {
        // Start the TEMP_RESIDENCY_TIME timer when we reach target temp for the first time.
        if (temp_diff < TEMP_WINDOW) residency_start_ms = now;
      }
      else if (temp_diff > TEMP_HYSTERESIS) {
        // Restart the timer whenever the temperature falls outside the hysteresis.
        residency_start_ms = now;
      }

    #endif

    // Prevent a wait-forever situation if R is misused i.e. M190 R0
    if (wants_to_cool) {
      // Break after 60 seconds
      // if the temperature did not drop at least 1.5
      if (!next_cool_check_ms || ELAPSED(now, next_cool_check_ms)) {
        if (old_temp - temp < 1.5) break;
        next_cool_check_ms = now + 60000UL;
        old_temp = temp;
      }
    }

  } while (printer.isWaitForHeatUp() && TEMP_CONDITIONS);

  if (printer.isWaitForHeatUp()) {
    LCD_MESSAGEPGM(MSG_HEATING_COMPLETE);
    #if ENABLED(PRINTER_EVENT_LEDS)
      #if ENABLED(RGBW_LED) || ENABLED(NEOPIXEL_RGBW_LED)
        set_led_color(0, 0, 0, 255);  // Turn on the WHITE LED
      #else
        set_led_color(255, 255, 255); // Set LEDs All On
      #endif
    #endif
  }

  #if DISABLED(BUSY_WHILE_HEATING)
    KEEPALIVE_STATE(IN_HANDLER);
  #endif

  printer.setAutoreportTemp(oldReport);
}

void Temperature::set_current_temp_raw() {

  #if ANALOG_INPUTS > 0
    LOOP_HEATER() heaters[h].sensor.raw = HAL::AnalogInputValues[heaters[h].sensor.pin];
  #endif

  #if HAS_POWER_CONSUMPTION_SENSOR
    powerManager.current_raw_powconsumption = HAL::AnalogInputValues[POWER_CONSUMPTION_PIN];
  #endif

  #if HAS_FILAMENT_SENSOR
    current_raw_filwidth = HAL::AnalogInputValues[FILWIDTH_PIN];
  #endif

}

/**
 * Spin Manage heating activities for heaters, bed, chamber and cooler
 *  - Is called every 100ms.
 *  - Acquire updated temperature readings
 *  - Also resets the watchdog timer
 *  - Invoke thermal runaway protection
 *  - Manage extruder auto-fan
 *  - Apply filament width to the extrusion rate (may move)
 *  - Update the heated bed PID output value
 */
void Temperature::spin() {

  if (++cycle_1_second == 10) cycle_1_second = 0;

  updateTemperaturesFromRawValues();

  millis_t ms = millis();

  LOOP_HEATER() {

    Heater *act = &heaters[h];

    if (act->isON() && act->current_temperature > act->maxtemp) max_temp_error(h);
    if (act->isON() && act->current_temperature < act->mintemp) min_temp_error(h);

    #if HEATER_IDLE_HANDLER
      if (!act->is_idle() && act->idle_timeout_ms && ELAPSED(ms, act->idle_timeout_ms))
        act->idle_timeout_exceeded = true;
    #endif

    // Check for thermal runaway
    #if HAS_THERMALLY_PROTECTED_HEATER
      if (thermal_protection[act->type])
        thermal_runaway_protection(&thermal_runaway_state_machine[h], &thermal_runaway_timer[h], act->current_temperature, act->target_temperature, h, THERMAL_PROTECTION_PERIOD, THERMAL_PROTECTION_HYSTERESIS);
    #endif

    // Ignore heater we are currently testing
    if (pid_pointer == act->ID) continue;

    if (act->use_pid)
      act->soft_pwm = act->tempisrange() ? get_pid_output(h) : 0;
    else if (ELAPSED(ms, next_check_ms[h])) {
      next_check_ms[h] = ms + temp_check_interval[act->type];
      if (act->tempisrange())
        act->soft_pwm = act->isHeating() ? act->pidDriveMax : 0;
      else
        act->soft_pwm = 0;
    }

    #if WATCH_THE_HEATER
      // Make sure temperature is increasing
      if (act->watch_next_ms && ELAPSED(ms, act->watch_next_ms)) {
        if (act->current_temperature < act->watch_target_temp)
          _temp_error(h, PSTR(MSG_T_HEATING_FAILED), PSTR(MSG_HEATING_FAILED_LCD));
        else
          act->start_watching(); // Start again if the target is still far off
      }
    #endif

  } // LOOP_HEATER

  // Control the extruder rate based on the width sensor
  #if ENABLED(FILAMENT_SENSOR)
    if (filament_sensor) {
      meas_shift_index = filwidth_delay_index[0] - meas_delay_cm;
      if (meas_shift_index < 0) meas_shift_index += MAX_MEASUREMENT_DELAY + 1;  //loop around buffer if needed
      meas_shift_index = constrain(meas_shift_index, 0, MAX_MEASUREMENT_DELAY);

      // Get the delayed info and add 100 to reconstitute to a percent of
      // the nominal filament diameter then square it to get an area
      const float vmroot = measurement_delay[meas_shift_index] * 0.01 + 1.0;
      tools.volumetric_multiplier[FILAMENT_SENSOR_EXTRUDER_NUM] = vmroot <= 0.1 ? 0.01 : sq(vmroot);
      tools.refresh_e_factor(FILAMENT_SENSOR_EXTRUDER_NUM);
    }
  #endif // FILAMENT_SENSOR
}

/**
 * PID Autotuning (M303)
 *
 * Alternately heat and cool the nozzle, observing its behavior to
 * determine the best PID values to achieve a stable temperature.
 */
void Temperature::PID_autotune(Heater *act, const float temp, const uint8_t ncycles, const uint8_t method, const bool storeValues/*=false*/) {

  float currentTemp = 0.0;
  int cycles = 0;
  bool heating = true;

  const bool oldReport = printer.isAutoreportTemp();
  printer.setAutoreportTemp(true);

  disable_all_heaters(); // switch off all heaters.

  millis_t  t1 = millis(),
            t2 = t1;
  int32_t   t_high = 0,
            t_low = 0;
  float     Ku,
            Tu,
            workKp = 0,
            workKi = 0,
            workKd = 0,
            maxTemp = 20.0,
            minTemp = 20.0;

  const uint8_t pidMax = act->pidMax;
  act->soft_pwm = pidMax;

  int32_t bias  = pidMax >> 1;
  int32_t d     = pidMax >> 1;

  printer.setWaitForHeatUp(true);
  pid_pointer = act->ID;

  // PID Tuning loop
  while (printer.isWaitForHeatUp()) {

    printer.idle();

    #if ENABLED(BUSY_WHILE_HEATING)
      KEEPALIVE_STATE(WAIT_HEATER);
    #endif

    #if FAN_COUNT > 0
      LOOP_FAN() fans[f].Check();
    #endif

    const millis_t ms = millis();
    currentTemp = act->current_temperature;
    NOLESS(maxTemp, currentTemp);
    NOMORE(minTemp, currentTemp);

    if (heating && currentTemp > temp) {
      if (ELAPSED(ms, t2 + 2500UL)) {
        heating = false;

        act->soft_pwm = (bias - d);

        t1 = ms;
        t_high = t1 - t2;

        #if HAS_TEMP_COOLER
          if (act->type == IS_COOLER)
            minTemp = temp;
          else
        #endif
          maxTemp = temp;
      }
    }

    if (!heating && currentTemp < temp) {
      if (ELAPSED(ms, t1 + 5000UL)) {
        heating = true;
        t2 = ms;
        t_low = t2 - t1;
        if (cycles > 0) {

          bias += (d * (t_high - t_low)) / (t_low + t_high);
          bias = constrain(bias, 20, pidMax - 20);
          d = (bias > pidMax / 2) ? pidMax - 1 - bias : bias;

          SERIAL_MV(MSG_BIAS, bias);
          SERIAL_MV(MSG_D, d);
          SERIAL_MV(MSG_T_MIN, minTemp);
          SERIAL_EMV(MSG_T_MAX, maxTemp);
          if (cycles > 2) {
            Ku = (4.0 * d) / (M_PI * (maxTemp - minTemp));
            Tu = ((float)(t_low + t_high) * 0.001);
            SERIAL_MV(MSG_KU, Ku);
            SERIAL_EMV(MSG_TU, Tu);

            if (method == 0) {
              workKp = 0.6 * Ku;
              workKi = 2.0 * workKp / Tu;
              workKd = workKp * Tu * 0.125;
              SERIAL_EM(MSG_CLASSIC_PID);
            }
            else if (method == 1) {
              workKp = 0.33 * Ku;
              workKi = 2.0 * workKp / Tu;
              workKd = workKp * Tu / 3.0;
              SERIAL_EM(MSG_SOME_OVERSHOOT_PID);
            }
            else if (method == 2) {
              workKp = 0.2 * Ku;
              workKi = 2.0 * workKp / Tu;
              workKd = workKp * Tu / 3.0;
              SERIAL_EM(MSG_NO_OVERSHOOT_PID);
            }
            else if (method == 3) {
              workKp = 0.7 * Ku;
              workKi = 2.5 * workKp / Tu;
              workKd = workKp * Tu * 3.0 / 20.0;
              SERIAL_EM(MSG_PESSEN_PID);
            }
            SERIAL_EMV(MSG_KP, workKp);
            SERIAL_EMV(MSG_KI, workKi);
            SERIAL_EMV(MSG_KD, workKd);
          }
        }

        act->soft_pwm = (bias + d);

        cycles++;

        #if HAS_TEMP_COOLER
          if (act->type == IS_COOLER)
            maxTemp = temp;
          else
        #endif
          minTemp = temp;
      }
    }

    #define MAX_OVERSHOOT_PID_AUTOTUNE 40
    if (currentTemp > temp + MAX_OVERSHOOT_PID_AUTOTUNE
      #if HAS_TEMP_COOLER
        && act->type != IS_COOLER
      #endif
    ) {
      SERIAL_LM(ER, MSG_PID_TEMP_TOO_HIGH);
      pid_pointer = 255;
      break;
    }
    #if HAS_TEMP_COOLER
      else if (currentTemp < temp + MAX_OVERSHOOT_PID_AUTOTUNE && act->type == IS_COOLER) {
        SERIAL_LM(ER, MSG_PID_TEMP_TOO_LOW);
        pid_pointer = 255;
        break;
      }
    #endif

    // Timeout after 20 minutes since the last undershoot/overshoot cycle
    if (((ms - t1) + (ms - t2)) > (20L * 60L * 1000L)) {
      SERIAL_EM(MSG_PID_TIMEOUT);
      pid_pointer = 255;
      break;
    }

    if (cycles > ncycles) {

      SERIAL_EM(MSG_PID_AUTOTUNE_FINISHED);
      pid_pointer = 255;

      #if (PIDTEMP)
        if (act->type == IS_HOTEND) {
          SERIAL_MV(MSG_KP, workKp);
          SERIAL_MV(MSG_KI, workKi);
          SERIAL_EMV(MSG_KD, workKd);
        }
      #endif

      #if (PIDTEMPBED)
        if (act->type == IS_BED) {
          SERIAL_EMV("#define DEFAULT_bedKp ", workKp);
          SERIAL_EMV("#define DEFAULT_bedKi ", workKi);
          SERIAL_EMV("#define DEFAULT_bedKd ", workKd);
        }
      #endif

      #if (PIDTEMPCHAMBER)
        if (act->type == IS_CHAMBER) {
          SERIAL_EMV("#define DEFAULT_chamberKp ", workKp);
          SERIAL_EMV("#define DEFAULT_chamberKi ", workKi);
          SERIAL_EMV("#define DEFAULT_chamberKd ", workKd);
        }
      #endif

      #if (PIDTEMPCOOLER)
        if (act->type == IS_COOLER) {
          SERIAL_EMV("#define DEFAULT_coolerKp ", workKp);
          SERIAL_EMV("#define DEFAULT_coolerKi ", workKi);
          SERIAL_EMV("#define DEFAULT_coolerKd ", workKd);
        }
      #endif

      if (storeValues) {
        act->Kp = workKp;
        act->Ki = workKi;
        act->Kd = workKd;
        updatePID();
        eeprom.Store_Settings();
      }

      break;
    }

    #if ENABLED(NEXTION)
      lcd_key_touch_update();
    #else
      lcd_update();
    #endif

  }

  printer.setAutoreportTemp(oldReport);
  disable_all_heaters();
}

void Temperature::updatePID() {

  LOOP_HEATER() {
    if (heaters[h].use_pid && heaters[h].Ki != 0) {
      heaters[h].tempIStateLimitMin = (float)heaters[h].pidDriveMin * 10.0f / heaters[h].Ki;
      heaters[h].tempIStateLimitMax = (float)heaters[h].pidDriveMax * 10.0f / heaters[h].Ki;
    }
  }

  #if ENABLED(PID_ADD_EXTRUSION_RATE)
    last_e_position = 0;
  #endif
}

void Temperature::disable_all_heaters() {

  #if HAS_TEMP_HOTEND && ENABLED(AUTOTEMP)
    planner.autotemp_enabled = false;
  #endif

  #if HEATER_COUNT > 0
    LOOP_HEATER() {
      heaters[h].target_temperature = 0;
      heaters[h].soft_pwm = 0;
    }
  #endif

  #if ENABLED(LASER)
    // No laser firing with no coolers running! (paranoia)
    laser.extinguish();
  #endif

  // Unpause and reset everything
  #if HAS_BED_PROBE && ENABLED(PROBING_HEATERS_OFF)
    probe.probing_pause(false);
  #endif

  // If all heaters go down then for sure our print job has stopped
  print_job_counter.stop();

  pid_pointer = 255;
}

#if ENABLED(FILAMENT_SENSOR)
  // Convert raw Filament Width to a ratio
  int Temperature::widthFil_to_size_ratio() {
    float temp = filament_width_meas;
    if (temp < MEASURED_LOWER_LIMIT) temp = filament_width_nominal;  // assume sensor cut out
    else NOMORE(temp, MEASURED_UPPER_LIMIT);
    return filament_width_nominal / temp * 100;
  }
#endif

#if ENABLED(PROBING_HEATERS_OFF)
  void Temperature::pause(const bool p) {
    if (p != paused) {
      paused = p;
      if (p)
        LOOP_HEATER() heaters[h].start_idle_timer(0); // timeout immediately
      else
        LOOP_HEATER() heaters[h].reset_idle_timer();
    }
  }
#endif // PROBING_HEATERS_OFF

void Temperature::report_temperatures(const bool showRaw/*=false*/) {

  #if HAS_TEMP_HOTEND
    print_heater_state(&heaters[TRG_EXTRUDER_IDX], false, showRaw);
  #endif

  #if HAS_TEMP_BED
    print_heater_state(&heaters[BED_INDEX], false, showRaw);
    SERIAL_MV(MSG_BAT, (int)heaters[BED_INDEX].soft_pwm);
  #endif

  #if HAS_TEMP_CHAMBER
    print_heater_state(&heaters[CHAMBER_INDEX], false, showRaw);
    SERIAL_MV(MSG_CAT, (int)heaters[CHAMBER_INDEX].soft_pwm);
  #endif

  #if HAS_TEMP_COOLER
    print_heater_state(&heaters[COOLER_INDEX], false, showRaw);
  #endif

  #if HAS_TEMP_HOTEND
    SERIAL_MV(" @:", (int)heaters[TRG_EXTRUDER_IDX].soft_pwm);
  #endif

  #if HOTENDS > 1
    LOOP_HOTEND() {
      print_heater_state(&heaters[h], true, showRaw);
      SERIAL_MV(MSG_AT, h);
      SERIAL_CHR(':');
      SERIAL_VAL((int)heaters[h].soft_pwm);
    }
  #endif

  #if HAS_MCU_TEMPERATURE
    SERIAL_MV(" MCU min:", mcu_lowest_temperature, 2);
    SERIAL_MV(", current:", mcu_current_temperature, 2);
    SERIAL_MV(", max:", mcu_highest_temperature, 2);
    if (showRaw)
      SERIAL_MV(" C->", mcu_current_temperature_raw);
  #endif

  #if ENABLED(DHT_SENSOR)
    SERIAL_MV(" DHT Temp:", dhtsensor.Temperature, 2);
    SERIAL_MV(", Humidity:", dhtsensor.Humidity, 2);
  #endif
}

// Private function
/**
 * Get the raw values into the actual temperatures.
 * The raw values are created in interrupt context,
 * and this function is called from normal context
 * as it would block the stepper routine.
 */
void Temperature::updateTemperaturesFromRawValues() {

  LOOP_HEATER() heaters[h].current_temperature = heaters[h].sensor.GetTemperature(h);

  #if ENABLED(FILAMENT_SENSOR)
    filament_width_meas = analog2widthFil();
  #endif

  #if HAS_POWER_CONSUMPTION_SENSOR
    static millis_t last_update = millis();
    millis_t temp_last_update = millis();
    millis_t from_last_update = temp_last_update - last_update;
    static float watt_overflow = 0.0;
    powerManager.consumption_meas = powerManager.analog2power();
    /*SERIAL_MV("raw:", powerManager.raw_analog2voltage(), 5);
    SERIAL_MV(" - V:", powerManager.analog2voltage(), 5);
    SERIAL_MV(" - I:", powerManager.analog2current(), 5);
    SERIAL_EMV(" - P:", powerManager.analog2power(), 5);*/
    watt_overflow += (powerManager.consumption_meas * from_last_update) / 3600000.0;
    if (watt_overflow >= 1.0) {
      powerManager.consumption_hour++;
      watt_overflow--;
    }
    last_update = temp_last_update;
  #endif

  #if HAS_MCU_TEMPERATURE
    mcu_current_temperature = analog2tempMCU(mcu_current_temperature_raw);
    NOLESS(mcu_highest_temperature, mcu_current_temperature);
    NOMORE(mcu_lowest_temperature, mcu_current_temperature);
  #endif

  #if ENABLED(USE_WATCHDOG)
    // Reset the watchdog after we know we have a temperature measurement.
    watchdog_reset();
  #endif

}

#if HAS_MCU_TEMPERATURE
  float Temperature::analog2tempMCU(const int raw) {
    const float voltage = (float)raw * (3.3 / (float)16384);
    return (voltage - 0.8) * (1000.0 / 2.65) + 27.0; // + mcuTemperatureAdjust;			// accuracy at 27C is +/-45C
  }
#endif

#if ENABLED(FILAMENT_SENSOR)
  // Convert raw Filament Width to millimeters
  float Temperature::analog2widthFil() {
    return current_raw_filwidth * 5.0 * (1.0 / 16383.0);
    //return current_raw_filwidth;
  }
#endif

uint8_t Temperature::get_pid_output(const uint8_t h) {

  static float  last_temperature[HEATER_COUNT]  = { 0.0 },
                temperature_1s[HEATER_COUNT]    = { 0.0 };

  uint8_t pid_output = 0;

  Heater *act = &heaters[h];

  float error = (float)act->target_temperature - act->current_temperature;

  #if HEATER_IDLE_HANDLER
    if (act->is_idle()) {
      pid_output = 0;
      act->tempIState = 0;
    }
    else
  #endif
  if (error > PID_FUNCTIONAL_RANGE) {
    pid_output = act->pidMax;
  }
  else if (error < -(PID_FUNCTIONAL_RANGE) || act->target_temperature == 0 
    #if HEATER_IDLE_HANDLER
      || act->is_idle()
    #endif
  ) {
    pid_output = 0;
  }
  else {
    float pidTerm = act->Kp * error;
    act->tempIState = constrain(act->tempIState + error, act->tempIStateLimitMin, act->tempIStateLimitMax);
    pidTerm += act->Ki * act->tempIState * 0.1; // 0.1 = 10Hz
    float dgain = act->Kd * (last_temperature[h] - temperature_1s[h]);
    pidTerm += dgain;

    #if ENABLED(PID_ADD_EXTRUSION_RATE)
      cTerm[h] = 0;
      if (h == EXTRUDER_IDX) {
        long e_position = stepper.position(E_AXIS);
        if (e_position > last_e_position) {
          lpq[lpq_ptr] = e_position - last_e_position;
          last_e_position = e_position;
        }
        else {
          lpq[lpq_ptr] = 0;
        }
        if (++lpq_ptr >= lpq_len) lpq_ptr = 0;
        cTerm[h] = (lpq[lpq_ptr] * mechanics.steps_to_mm[E_AXIS]) * act->Kc;
        pidTerm += cTerm[h];
      }
    #endif // PID_ADD_EXTRUSION_RATE

    pid_output = constrain((int)pidTerm, 0, PID_MAX);

    if (cycle_1_second == 0) {
      last_temperature[h] = temperature_1s[h];
      temperature_1s[h] = act->current_temperature;
    }

  }

  #if ENABLED(PID_DEBUG)
    SERIAL_SMV(ECHO, MSG_PID_DEBUG, HOTEND_INDEX);
    SERIAL_MV(MSG_PID_DEBUG_INPUT, act->current_temperature);
    SERIAL_EMV(MSG_PID_DEBUG_OUTPUT, pid_output);
  #endif // PID_DEBUG

  return pid_output;
}

// Temperature Error Handlers
void Temperature::_temp_error(const uint8_t h, const char * const serial_msg, const char * const lcd_msg) {
  static bool killed = false;
  if (printer.IsRunning()) {
    SERIAL_ST(ER, serial_msg);
    SERIAL_MSG(MSG_STOPPED_HEATER);
    switch (heaters[h].type) {
      case IS_HOTEND:
        SERIAL_EV((int)h);
        break;
      #if HAS_TEMP_BED
        case IS_BED:
          SERIAL_EM(MSG_HEATER_BED);
          break;
      #endif
      #if HAS_TEMP_CHAMBER
        case IS_CHAMBER:
          SERIAL_EM(MSG_HEATER_CHAMBER);
          break;
      #endif
      #if HAS_TEMP_COOLER
        case IS_COOLER:
          SERIAL_EM(MSG_HEATER_COOLER);
          break;
      #endif
      default: break;
    }
  }

  #if DISABLED(BOGUS_TEMPERATURE_FAILSAFE_OVERRIDE)
    if (!killed) {
      printer.setRunning(false);
      killed = true;
      printer.kill(lcd_msg);
    }
    else
      disable_all_heaters();
  #endif
}
void Temperature::min_temp_error(const uint8_t h) {
  switch (heaters[h].type) {
    case IS_HOTEND:
      _temp_error(HOTEND_INDEX, PSTR(MSG_T_MINTEMP), PSTR(MSG_ERR_MINTEMP));
      break;
    #if HAS_TEMP_BED
      case IS_BED:
        _temp_error(h, PSTR(MSG_T_MINTEMP), PSTR(MSG_ERR_MINTEMP_BED));
        break;
    #endif
    #if HAS_TEMP_CHAMBER
      case IS_CHAMBER:
        _temp_error(h, PSTR(MSG_T_MINTEMP), PSTR(MSG_ERR_MINTEMP_CHAMBER));
        break;
    #endif
    default: break;
  }
}
void Temperature::max_temp_error(const uint8_t h) {
  switch (heaters[h].type) {
    case IS_HOTEND:
      _temp_error(HOTEND_INDEX, PSTR(MSG_T_MAXTEMP), PSTR(MSG_ERR_MAXTEMP));
      break;
    #if HAS_TEMP_BED
      case IS_BED:
        _temp_error(h, PSTR(MSG_T_MAXTEMP), PSTR(MSG_ERR_MAXTEMP_BED));
        break;
    #endif
    #if HAS_TEMP_CHAMBER
      case IS_CHAMBER:
        _temp_error(h, PSTR(MSG_T_MAXTEMP), PSTR(MSG_ERR_MAXTEMP_CHAMBER));
        break;
    #endif
    default: break;
  }
}

#if HAS_THERMALLY_PROTECTED_HEATER

  Temperature::TRState Temperature::thermal_runaway_state_machine[HEATER_COUNT] = { TRInactive };
  millis_t Temperature::thermal_runaway_timer[HEATER_COUNT] = { 0 };

  void Temperature::thermal_runaway_protection(Temperature::TRState* state, millis_t* timer, float temperature, float target_temperature, const uint8_t h, int period_seconds, int hysteresis_degc) {

    static float tr_target_temperature[HEATER_COUNT] = { 0.0 };

    /*
        SERIAL_MSG("Thermal Thermal Runaway Running. Heater ID: ");
        if (h < 0) SERIAL_MSG("bed"); else SERIAL_VAL(h);
        SERIAL_MV(" ;  State:", *state);
        SERIAL_MV(" ;  Timer:", *timer);
        SERIAL_MV(" ;  Temperature:", temperature);
        SERIAL_EMV(" ;  Target Temp:", target_temperature);
    */
   
    #if HEATER_IDLE_HANDLER
      // If the heater idle timeout expires, restart
      if (heaters[h].is_idle()) {
        *state = TRInactive;
        tr_target_temperature[h] = 0;
      }
      else
    #endif
    // If the target temperature changes, restart
    if (tr_target_temperature[h] != target_temperature) {
      tr_target_temperature[h] = target_temperature;
      *state = target_temperature > 0 ? TRFirstHeating : TRInactive;
    }

    switch (*state) {
      // Inactive state waits for a target temperature to be set
      case TRInactive: break;
      // When first heating, wait for the temperature to be reached then go to Stable state
      case TRFirstHeating:
        if (temperature < tr_target_temperature[h]) break;
        *state = TRStable;
      // While the temperature is stable watch for a bad temperature
      case TRStable:
        if (temperature >= tr_target_temperature[h] - hysteresis_degc) {
          *timer = millis() + period_seconds * 1000UL;
          break;
        }
        else if (PENDING(millis(), *timer)) break;
        *state = TRRunaway;
      case TRRunaway:
        _temp_error(h, PSTR(MSG_T_THERMAL_RUNAWAY), PSTR(MSG_THERMAL_RUNAWAY));
    }
  }

#endif // THERMAL_PROTECTION_HOTENDS || THERMAL_PROTECTION_BED

void Temperature::print_heater_state(Heater *act, const bool print_ID, const bool showRaw) {

  SERIAL_CHR(' ');

  #if HAS_TEMP_HOTEND
    if (act->type == IS_HOTEND) {
      SERIAL_CHR('T');
      #if HOTENDS > 1
        if (print_ID) SERIAL_VAL((int)act->ID);
      #endif
    }
  #endif

  #if HAS_TEMP_BED
    if (act->type == IS_BED) SERIAL_CHR('B');
  #endif

  #if HAS_TEMP_CHAMBER
    if (act->type == IS_CHAMBER) SERIAL_CHR('C');
  #endif

  #if HAS_TEMP_COOLER
    if (act->type == IS_COOLER) SERIAL_CHR('C');
  #endif

  SERIAL_CHR(':');
  SERIAL_VAL(act->current_temperature, 2);
  SERIAL_MV(" /" , act->target_temperature);
  if (showRaw) {
    SERIAL_MV(" (", act->sensor.raw);
    SERIAL_CHR(')');
  }

}
