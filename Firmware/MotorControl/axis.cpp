
#include <stdlib.h>
#include <functional>
#include "gpio.h"

#include "utils.h"
#include "odrive_main.h"

Axis::Axis(const AxisHardwareConfig_t& hw_config,
           Config_t& config,
           Encoder& encoder,
           SensorlessEstimator& sensorless_estimator,
           Controller& controller,
           Motor& motor,
           TrapezoidalTrajectory& trap,
           Endstop& min_endstop,
           Endstop& max_endstop)
    : hw_config_(hw_config),
      config_(config),
      encoder_(encoder),
      sensorless_estimator_(sensorless_estimator),
      controller_(controller),
      motor_(motor),
      trap_(trap),
      min_endstop_(min_endstop),
      max_endstop_(max_endstop)
{
    encoder_.axis_ = this;
    sensorless_estimator_.axis_ = this;
    controller_.axis_ = this;
    motor_.axis_ = this;
    trap_.axis_ = this;
    min_endstop_.axis_ = this;
    max_endstop_.axis_ = this;
}

static void step_cb_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->step_cb();
}


// @brief Sets up all components of the axis,
// such as gate driver and encoder hardware.
void Axis::setup() {
    encoder_.setup();
    motor_.setup();
}

static void run_state_machine_loop_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->run_state_machine_loop();
    reinterpret_cast<Axis*>(ctx)->thread_id_valid_ = false;
}

// @brief Starts run_state_machine_loop in a new thread
void Axis::start_thread() {
    osThreadDef(thread_def, run_state_machine_loop_wrapper, hw_config_.thread_priority, 0, 4*512);
    thread_id_ = osThreadCreate(osThread(thread_def), this);
    thread_id_valid_ = true;
}

// @brief Unblocks the control loop thread.
// This is called from the current sense interrupt handler.
void Axis::signal_current_meas() {
    if (thread_id_valid_)
        osSignalSet(thread_id_, M_SIGNAL_PH_CURRENT_MEAS);
}

// @brief Blocks until a current measurement is completed
// @returns True on success, false otherwise
bool Axis::wait_for_current_meas() {
    return osSignalWait(M_SIGNAL_PH_CURRENT_MEAS, PH_CURRENT_MEAS_TIMEOUT).status == osEventSignal;
}

// step/direction interface
void Axis::step_cb() {
    if (enable_step_dir_) {
        GPIO_PinState dir_pin = HAL_GPIO_ReadPin(hw_config_.dir_port, hw_config_.dir_pin);
        float dir = (dir_pin == GPIO_PIN_SET) ? 1.0f : -1.0f;
        controller_.pos_setpoint_ += dir * config_.counts_per_step;
    }
};

// @brief Enables or disables step/dir input
void Axis::set_step_dir_enabled(bool enable) {
    if (enable) {
        // Set up the direction GPIO as input
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Pin = hw_config_.dir_pin;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(hw_config_.dir_port, &GPIO_InitStruct);

        // Subscribe to rising edges of the step GPIO
        GPIO_subscribe(hw_config_.step_port, hw_config_.step_pin, GPIO_PULLDOWN, 
                       GPIO_MODE_IT_FALLING, step_cb_wrapper, this);

        enable_step_dir_ = true;
    } else {
        enable_step_dir_ = false;

        // Unsubscribe from step GPIO
        GPIO_unsubscribe(hw_config_.step_port, hw_config_.step_pin);
    }
}

// @brief Do axis level checks and call subcomponent do_checks
// Returns true if everything is ok.
bool Axis::do_checks() {
    if (!brake_resistor_armed)
        error_ |= ERROR_BRAKE_RESISTOR_DISARMED;
    if ((current_state_ != AXIS_STATE_IDLE) && (motor_.armed_state_ == Motor::ARMED_STATE_DISARMED))
        // motor got disarmed in something other than the idle loop
        error_ |= ERROR_MOTOR_DISARMED;
    if (!(vbus_voltage >= board_config.dc_bus_undervoltage_trip_level))
        error_ |= ERROR_DC_BUS_UNDER_VOLTAGE;
    if (!(vbus_voltage <= board_config.dc_bus_overvoltage_trip_level))
        error_ |= ERROR_DC_BUS_OVER_VOLTAGE;

    // Sub-components should use set_error which will propegate to this error_
    motor_.do_checks();
    encoder_.do_checks();
    // sensorless_estimator_.do_checks();
    // controller_.do_checks();

    return check_for_errors();
}

// @brief Update all esitmators
bool Axis::do_updates() {
    // Sub-components should use set_error which will propegate to this error_
    encoder_.update();
    sensorless_estimator_.update();
    min_endstop_.update();
    max_endstop_.update();
    return check_for_errors();
}

float Axis::get_temp() {
    float adc = adc_measurements_[hw_config_.thermistor_adc_ch];
    float normalized_voltage = adc / adc_full_scale;
    return horner_fma(normalized_voltage, thermistor_poly_coeffs, thermistor_num_coeffs);
}

bool Axis::run_sensorless_spin_up() {
    // Early Spin-up: spiral up current
    float x = 0.0f;
    run_control_loop([&](){
        float phase = wrap_pm_pi(config_.ramp_up_distance * x);
        float I_mag = config_.spin_up_current * x;
        x += current_meas_period / config_.ramp_up_time;
        if (!motor_.update(I_mag, phase))
            return error_ |= ERROR_MOTOR_FAILED, false;
        return x < 1.0f;
    });
    if (error_ != ERROR_NONE)
        return false;
    
    // Late Spin-up: accelerate
    float vel = config_.ramp_up_distance / config_.ramp_up_time;
    float phase = wrap_pm_pi(config_.ramp_up_distance);
    run_control_loop([&](){
        vel += config_.spin_up_acceleration * current_meas_period;
        phase = wrap_pm_pi(phase + vel * current_meas_period);
        float I_mag = config_.spin_up_current;
        if (!motor_.update(I_mag, phase))
            return error_ |= ERROR_MOTOR_FAILED, false;
        return vel < config_.spin_up_target_vel;
    });

    // call to controller.reset() that happend when arming means that vel_setpoint
    // is zeroed. So we make the setpoint the spinup target for smooth transition.
    controller_.vel_setpoint_ = config_.spin_up_target_vel;

    return check_for_errors();
}

// Note run_sensorless_control_loop and run_closed_loop_control_loop are very similar and differ only in where we get the estimate from.
bool Axis::run_sensorless_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);

    run_control_loop([this](){
        if (controller_.config_.control_mode >= Controller::CTRL_MODE_POSITION_CONTROL)
            return error_ |= ERROR_POS_CTRL_DURING_SENSORLESS, false;

        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(sensorless_estimator_.pll_pos_, sensorless_estimator_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false;
        if (!motor_.update(current_setpoint, sensorless_estimator_.phase_))
            return false; // set_error should update axis.error_
        return true;
    });
    set_step_dir_enabled(false);
    return check_for_errors();
}

// variables for homing - run_control_loop([this]() wouldn't allow local vars to be used
bool finding_min_endstop = true;
unsigned int loop_counter_check = 0;

bool Axis::run_closed_loop_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);
    finding_min_endstop = true;
    loop_counter_check = loop_counter_ + (CURRENT_MEAS_HZ * min_endstop_.config_.min_ms_homing) / 1000;
    run_control_loop([this]() {
        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(encoder_.pos_estimate_, encoder_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false;  //TODO: Make controller.set_error
        if (!motor_.update(current_setpoint, encoder_.phase_))
            return false;  // set_error should update axis.error_

        // Handle the homing case
        if (homing_state_ == HOMING_STATE_HOMING) {
            Endstop *current_endstop = &min_endstop_;
            if (!finding_min_endstop) {
                current_endstop = &max_endstop_;
            }
            bool found_end = (encoder_.vel_estimate_ == 0.0f && loop_counter_check <= loop_counter_);

            if (!current_endstop->getEndstopState() && found_end && !current_endstop->config_.physical_endstop)
            {
                //TODO return error
            }

            if (current_endstop->getEndstopState() || found_end)
            {
                if (finding_min_endstop)
                {
                    min_endstop_.offset_from_home = encoder_.shadow_count_; //temp holding position
                    finding_min_endstop = false;
                    loop_counter_check = loop_counter_ + (CURRENT_MEAS_HZ * current_endstop->config_.min_ms_homing) / 1000;
                    if (max_endstop_.config_.enabled) {
                        controller_.vel_integrator_current_ = 0.0f;
                        controller_.set_vel_setpoint(controller_.config_.homing_speed, 0.0f);
                    } else {
                        encoder_.set_linear_count(min_endstop_.config_.offset);
                        controller_.set_pos_setpoint(0.0f, 0.0f, 0.0f);
                        homing_state_ = HOMING_STATE_MOVE_TO_ZERO;
                    }  
                }
                else
                {
                    int total_cpr = encoder_.shadow_count_ - min_endstop_.offset_from_home;
                    if (min_endstop_.config_.home_percentage > 0) {
                        min_endstop_.offset_from_home = -(total_cpr * 1.0f) * (min_endstop_.config_.home_percentage / 100.0f);
                        max_endstop_.offset_from_home = total_cpr + min_endstop_.offset_from_home;
                        encoder_.set_linear_count(-min_endstop_.offset_from_home);
                    }
                    else {
                        min_endstop_.offset_from_home = min_endstop_.config_.offset;
                        max_endstop_.offset_from_home = total_cpr + min_endstop_.offset_from_home;
                        encoder_.set_linear_count(min_endstop_.config_.offset);
                    }

                    controller_.set_pos_setpoint(0.0f, 0.0f, 0.0f);
                    homing_state_ = HOMING_STATE_MOVE_TO_ZERO;
                }

            }
        } else if (homing_state_ == HOMING_STATE_MOVE_TO_ZERO) {
            if(!min_endstop_.getEndstopState()){
                    trap_.planTrapezoidal(0.0f, encoder_.pos_estimate_, encoder_.vel_estimate_,
                                controller_.config_.homing_speed,
                                controller_.config_.homing_speed / 4.0f,
                                controller_.config_.homing_speed / 4.0f);
                                controller_.traj_start_loop_count_ = loop_counter_;
                                controller_.config_.control_mode = controller_.CTRL_MODE_TRAJECTORY_CONTROL;
            }
        } else {
            // Check for endstop presses
            if (min_endstop_.config_.enabled && min_endstop_.getEndstopState()) {
                return error_ |= ERROR_MIN_ENDSTOP_PRESSED, false;
            } else if (max_endstop_.config_.enabled && max_endstop_.getEndstopState()) {
                return error_ |= ERROR_MAX_ENDSTOP_PRESSED, false;
            }
        }
        return true;
    });
    set_step_dir_enabled(false);
    return check_for_errors();
}

bool Axis::run_idle_loop() {
    // run_control_loop ignores missed modulation timing updates
    // if and only if we're in AXIS_STATE_IDLE
    safety_critical_disarm_motor_pwm(motor_);
    run_control_loop([this](){
        return true;
    });
    return check_for_errors();
}

// Infinite loop that does calibration and enters main control loop as appropriate
void Axis::run_state_machine_loop() {
    // Allocate the map for anti-cogging algorithm and initialize all values to 0.0f
    // TODO: Move this somewhere else
    // TODO: respect changes of CPR
    int encoder_cpr = encoder_.config_.cpr;
    controller_.anticogging_.cogging_map = (float*)malloc(encoder_cpr * sizeof(float));
    if (controller_.anticogging_.cogging_map != NULL) {
        for (int i = 0; i < encoder_cpr; i++) {
            controller_.anticogging_.cogging_map[i] = 0.0f;
        }
    }

    // arm!
    motor_.arm();
    
    for (;;) {
        // Load the task chain if a specific request is pending
        if (requested_state_ != AXIS_STATE_UNDEFINED) {
            size_t pos = 0;
            if (requested_state_ == AXIS_STATE_STARTUP_SEQUENCE) {
                if (config_.startup_motor_calibration)
                    task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (config_.startup_encoder_index_search && encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                if (config_.startup_encoder_offset_calibration)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                if (config_.startup_closed_loop_control){
                    if(config_.startup_homing)
                        task_chain_[pos++] = AXIS_STATE_HOMING;
                    task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                }
                else if (config_.startup_sensorless_control)
                    task_chain_[pos++] = AXIS_STATE_SENSORLESS_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_HOMING){
                task_chain_[pos++] = AXIS_STATE_HOMING;
                task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_FULL_CALIBRATION_SEQUENCE) {
                task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ != AXIS_STATE_UNDEFINED) {
                task_chain_[pos++] = requested_state_;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            }
            task_chain_[pos++] = AXIS_STATE_UNDEFINED; // TODO: bounds checking
            requested_state_ = AXIS_STATE_UNDEFINED;
            // Auto-clear any invalid state error
            error_ &= ~ERROR_INVALID_STATE;
        }

        // Note that current_state is a reference to task_chain_[0]

        // Validate the state before running it
        if (current_state_ > AXIS_STATE_MOTOR_CALIBRATION && !motor_.is_calibrated_)
            current_state_ = AXIS_STATE_UNDEFINED;
        if (current_state_ > AXIS_STATE_ENCODER_OFFSET_CALIBRATION && !encoder_.is_ready_)
            current_state_ = AXIS_STATE_UNDEFINED;

        // Run the specified state
        // Handlers should exit if requested_state != AXIS_STATE_UNDEFINED
        bool status;
        switch (current_state_) {
            case AXIS_STATE_MOTOR_CALIBRATION:
                status = motor_.run_calibration();
                break;

            case AXIS_STATE_ENCODER_INDEX_SEARCH:
                status = encoder_.run_index_search();
                break;

            case AXIS_STATE_HOMING:
                status = controller_.home_axis();
                break;

            case AXIS_STATE_ENCODER_OFFSET_CALIBRATION:
                status = encoder_.run_offset_calibration();
                break;

            case AXIS_STATE_SENSORLESS_CONTROL:
                status = run_sensorless_spin_up(); // TODO: restart if desired
                if (status)
                    status = run_sensorless_control_loop();
                break;

            case AXIS_STATE_CLOSED_LOOP_CONTROL:
                status = run_closed_loop_control_loop();
                break;

            case AXIS_STATE_IDLE:
                run_idle_loop();
                status = motor_.arm(); // done with idling - try to arm the motor
                break;

            default:
                error_ |= ERROR_INVALID_STATE;
                status = false; // this will set the state to idle
                break;
        }

        // If the state failed, go to idle, else advance task chain
        if (!status)
            current_state_ = AXIS_STATE_IDLE;
        else
            memcpy(task_chain_, task_chain_ + 1, sizeof(task_chain_) - sizeof(task_chain_[0]));
    }
}
