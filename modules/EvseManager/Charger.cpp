// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2022 Pionix GmbH and Contributors to EVerest
/*
 * Charger.cpp
 *
 *  Created on: 08.03.2021
 *      Author: cornelius
 *
 */

#include "Charger.hpp"

#include <math.h>
#include <string.h>

#include <fmt/core.h>

namespace module {

Charger::Charger(const std::unique_ptr<IECStateMachine>& bsp, const std::unique_ptr<ErrorHandling>& error_handling,
                 const types::evse_board_support::Connector_type& connector_type) :
    bsp(bsp), error_handling(error_handling), connector_type(connector_type) {
    connector_enabled = true;
    max_current = 6.0;
    if (connector_type == types::evse_board_support::Connector_type::IEC62196Type2Socket) {
        max_current_cable = bsp->read_pp_ampacity();
    }
    authorized = false;

    update_pwm_last_dc = 0.;

    current_state = EvseState::Idle;
    last_state = EvseState::Disabled;

    current_drawn_by_vehicle[0] = 0.;
    current_drawn_by_vehicle[1] = 0.;
    current_drawn_by_vehicle[2] = 0.;

    t_step_EF_return_state = EvseState::Idle;
    t_step_X1_return_state = EvseState::Idle;

    matching_started = false;

    transaction_active = false;
    session_active = false;

    hlc_use_5percent_current_session = false;

    // Register callbacks for errors/error clearings
    error_handling->signal_error.connect([this](const types::evse_manager::Error e, const bool prevent_charging) {
        if (prevent_charging) {
            std::scoped_lock lock(state_machine_mutex);
            error_prevent_charging_flag = true;
        }
    });

    error_handling->signal_all_errors_cleared.connect([this]() {
        EVLOG_info << "All errors cleared";
        signal_event(types::evse_manager::SessionEventEnum::AllErrorsCleared);
        {
            std::scoped_lock lock(state_machine_mutex);
            error_prevent_charging_flag = false;
        }
    });
}

Charger::~Charger() {
    pwm_F();
}

void Charger::main_thread() {
    // Enable CP output
    bsp->enable(true);

    // publish initial values
    signal_max_current(get_max_current_internal());
    signal_state(current_state);

    while (true) {
        if (main_thread_handle.shouldExit()) {
            break;
        }

        std::this_thread::sleep_for(MAINLOOP_UPDATE_RATE);

        {
            std::scoped_lock lock(state_machine_mutex);
            // update power limits
            power_available();
            // Run our own state machine update (i.e. run everything that needs
            // to be done on regular intervals independent from events)
            run_state_machine();
        }
    }
}

void Charger::run_state_machine() {

    // run over state machine loop until current_state does not change anymore
    do {
        // If a state change happened or an error recovered during a state we reinitialize the state
        bool initialize_state = (last_state_detect_state_change not_eq current_state) or
                                (last_error_prevent_charging_flag not_eq error_prevent_charging_flag);

        if (initialize_state) {
            session_log.evse(false,
                             fmt::format("Charger state: {}->{}", evse_state_to_string(last_state_detect_state_change),
                                         evse_state_to_string(current_state)));
        }

        last_state = last_state_detect_state_change;
        last_state_detect_state_change = current_state;
        last_error_prevent_charging_flag = error_prevent_charging_flag;

        auto now = std::chrono::system_clock::now();

        if (ac_with_soc_timeout and (ac_with_soc_timer -= 50) < 0) {
            ac_with_soc_timeout = false;
            ac_with_soc_timer = 3600000;
            signal_ac_with_soc_timeout();
            return;
        }

        if (initialize_state) {
            current_state_started = now;
            signal_state(current_state);
        }

        auto timeInCurrentState =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - current_state_started).count();

        switch (current_state) {
        case EvseState::Disabled:
            if (initialize_state) {
                signal_event(types::evse_manager::SessionEventEnum::Disabled);
                pwm_F();
            }
            break;

        case EvseState::Replug:
            if (initialize_state) {
                signal_event(types::evse_manager::SessionEventEnum::ReplugStarted);
                // start timer in case we need to
                if (ac_with_soc_timeout) {
                    ac_with_soc_timer = 120000;
                }
            }
            // simply wait here until BSP informs us that replugging was finished
            break;

        case EvseState::Idle:
            // make sure we signal availability to potential new cars
            if (initialize_state) {
                bcb_toggle_reset();
                iec_allow_close_contactor = false;
                hlc_charging_active = false;
                hlc_allow_close_contactor = false;
                max_current_cable = 0;
                hlc_charging_terminate_pause = HlcTerminatePause::Unknown;
                pwm_off();
                deauthorize_internal();
                transaction_active = false;
                clear_errors_on_unplug();
            }
            break;

        case EvseState::WaitingForAuthentication:

            // Explicitly do not allow to be powered on. This is important
            // to make sure control_pilot does not switch on relais even if
            // we start PWM here
            if (initialize_state) {
                bsp->allow_power_on(false, types::evse_board_support::Reason::PowerOff);

                if (last_state == EvseState::Replug) {
                    signal_event(types::evse_manager::SessionEventEnum::ReplugFinished);
                } else {
                    // First user interaction was plug in of car? Start session here.
                    if (not session_active) {
                        start_session(false);
                    }
                    // External signal on MQTT
                    signal_event(types::evse_manager::SessionEventEnum::AuthRequired);
                }
                hlc_use_5percent_current_session = false;

                // switch on HLC if configured. May be switched off later on after retries for this session only.
                if (charge_mode == ChargeMode::AC) {
                    ac_hlc_enabled_current_session = ac_hlc_enabled;
                    if (ac_hlc_enabled_current_session) {
                        hlc_use_5percent_current_session = ac_hlc_use_5percent;
                    }
                } else if (charge_mode == ChargeMode::DC) {
                    hlc_use_5percent_current_session = true;
                } else {
                    // unsupported charging mode, give up here.
                    error_handling->raise_internal_error("Unsupported charging mode.");
                }

                if (hlc_use_5percent_current_session) {
                    // FIXME: wait for SLAC to be ready. Teslas are really fast with sending the first slac packet after
                    // enabling PWM.
                    std::this_thread::sleep_for(SLEEP_BEFORE_ENABLING_PWM_HLC_MODE);
                    update_pwm_now(PWM_5_PERCENT);
                }
            }

            // Read PP value in case of AC socket
            if (connector_type == types::evse_board_support::Connector_type::IEC62196Type2Socket and
                max_current_cable == 0) {
                max_current_cable = bsp->read_pp_ampacity();
                // retry if the value is not yet available. Some BSPs may take some time to measure the PP.
                if (max_current_cable == 0) {
                    break;
                }
            }

            // Wait for Energy Manager to supply some power, otherwise wait here.
            // If we have zero power, some cars will not like the ChargingParameter message.
            if (charge_mode == ChargeMode::DC and not(current_evse_max_limits.EVSEMaximumCurrentLimit > 0 and
                                                      current_evse_max_limits.EVSEMaximumPowerLimit > 0)) {
                break;
            }

            // SLAC is running in the background trying to setup a PLC connection.

            // we get Auth (maybe before SLAC matching or during matching)
            // FIXME: getAuthorization needs to distinguish between EIM and PnC in Auth mananger

            // FIXME: Note Fig 7. is not fully supported here yet (AC Auth before plugin - this overides PnC and always
            // starts with nominal PWM). We need to support this as this is the only way a user can
            // skip PnC if he does NOT want to use it for this charging session.

            // FIXME: In case V2G is not successfull after all, it needs
            // to send a dlink_error request, which then needs to:
            // AC mode: fall back to nominal PWM charging etc (not
            // implemented yet!), it can do so because it is already authorized.
            // DC mode: go to error_hlc and give up

            // FIXME: if slac reports a dlink_ready=false while we are still waiting for auth we should:
            // in AC mode: go back to non HLC nominal PWM mode
            // in DC mode: go to error_slac for this session

            if (authorized and not authorized_pnc) {
                session_log.evse(false, "EIM Authorization received");

                // If we are restarting, the transaction may already be active
                if (not transaction_active) {
                    start_transaction();
                }

                const EvseState target_state(EvseState::PrepareCharging);

                // EIM done and matching process not started -> we need to go through t_step_EF and fall back to nominal
                // PWM. This is a complete waste of 4 precious seconds.
                if (charge_mode == ChargeMode::AC) {
                    if (ac_hlc_enabled_current_session) {
                        if (ac_enforce_hlc) {
                            // non standard compliant mode: we just keep 5 percent running all the time like in DC
                            session_log.evse(
                                false, "AC mode, HLC enabled(ac_enforce_hlc), keeping 5 percent on until a dlink error "
                                       "is signalled.");
                            hlc_use_5percent_current_session = true;
                            current_state = target_state;
                        } else {
                            if (not get_matching_started()) {
                                // SLAC matching was not started when EIM arrived

                                session_log.evse(
                                    false,
                                    fmt::format(
                                        "AC mode, HLC enabled, matching not started yet. Go through t_step_EF and "
                                        "disable 5 percent if it was enabled before: {}",
                                        hlc_use_5percent_current_session));

                                // Figure 3 of ISO15118-3: 5 percent start, PnC and EIM
                                // Figure 4 of ISO15118-3: X1 start, PnC and EIM
                                t_step_EF_return_state = target_state;
                                t_step_EF_return_pwm = 0.;
                                // fall back to nominal PWM after the t_step_EF break. Note that
                                // ac_hlc_enabled_current_session remains untouched as HLC can still start later in
                                // nominal PWM mode
                                hlc_use_5percent_current_session = false;
                                current_state = EvseState::T_step_EF;
                            } else {
                                // SLAC matching was started already when EIM arrived
                                if (hlc_use_5percent_current_session) {
                                    // Figure 5 of ISO15118-3: 5 percent start, PnC and EIM, matching already started
                                    // when EIM was done
                                    session_log.evse(
                                        false, "AC mode, HLC enabled(5percent), matching already started. Go through "
                                               "t_step_X1 and disable 5 percent.");
                                    t_step_X1_return_state = target_state;
                                    t_step_X1_return_pwm = 0.;
                                    hlc_use_5percent_current_session = false;
                                    current_state = EvseState::T_step_X1;
                                } else {
                                    // Figure 6 of ISO15118-3: X1 start, PnC and EIM, matching already started when EIM
                                    // was done. We can go directly to PrepareCharging, as we do not need to switch from
                                    // 5 percent to nominal first
                                    session_log.evse(
                                        false,
                                        "AC mode, HLC enabled(X1), matching already started. We are in X1 so we can "
                                        "go directly to nominal PWM.");
                                    current_state = target_state;
                                }
                            }
                        }

                    } else {
                        // HLC is disabled for this session.
                        // simply proceed to PrepareCharging, as we are fully authorized to start charging whenever car
                        // wants to.
                        session_log.evse(false, "AC mode, HLC disabled. We are in X1 so we can "
                                                "go directly to nominal PWM.");
                        current_state = target_state;
                    }
                } else if (charge_mode == ChargeMode::DC) {
                    // Figure 8 of ISO15118-3: DC with EIM before or after plugin or PnC
                    // simple here as we always stay within 5 percent mode anyway.
                    session_log.evse(false,
                                     "DC mode. We are in 5percent mode so we can continue without further action.");
                    current_state = target_state;
                } else {
                    // unsupported charging mode, give up here.
                    error_handling->raise_internal_error("Unsupported charging mode.");
                }
            } else if (authorized and authorized_pnc) {

                start_transaction();

                const EvseState target_state(EvseState::PrepareCharging);

                // We got authorization by Plug and Charge
                session_log.evse(false, "PnC Authorization received");
                if (charge_mode == ChargeMode::AC) {
                    // Figures 3,4,5,6 of ISO15118-3: Independent on how we started we can continue with 5 percent
                    // signalling once we got PnC authorization without going through t_step_EF or t_step_X1.

                    session_log.evse(
                        false, "AC mode, HLC enabled, PnC auth received. We will continue with 5percent independent on "
                               "how we started.");
                    hlc_use_5percent_current_session = true;
                    current_state = target_state;

                } else if (charge_mode == ChargeMode::DC) {
                    // Figure 8 of ISO15118-3: DC with EIM before or after plugin or PnC
                    // simple here as we always stay within 5 percent mode anyway.
                    session_log.evse(false,
                                     "DC mode. We are in 5percent mode so we can continue without further action.");
                    current_state = target_state;
                } else {
                    // unsupported charging mode, give up here.
                    error_handling->raise_internal_error("Unsupported charging mode.");
                }
            }

            break;

        case EvseState::T_step_EF:
            if (initialize_state) {
                session_log.evse(false, "Enter T_step_EF");
                pwm_F();
            }
            if (timeInCurrentState >= t_step_EF) {
                session_log.evse(false, "Exit T_step_EF");
                if (t_step_EF_return_pwm == 0.) {
                    pwm_off();
                } else {
                    update_pwm_now(t_step_EF_return_pwm);
                }
                current_state = t_step_EF_return_state;
            }
            break;

        case EvseState::T_step_X1:
            if (initialize_state) {
                session_log.evse(false, "Enter T_step_X1");
                pwm_off();
            }
            if (timeInCurrentState >= t_step_X1) {
                session_log.evse(false, "Exit T_step_X1");
                if (t_step_X1_return_pwm == 0.) {
                    pwm_off();
                } else {
                    update_pwm_now(t_step_X1_return_pwm);
                }
                current_state = t_step_X1_return_state;
            }
            break;

        case EvseState::PrepareCharging:

            if (initialize_state) {
                signal_event(types::evse_manager::SessionEventEnum::PrepareCharging);
                bcb_toggle_reset();
            }

            if (charge_mode == ChargeMode::DC) {
                if (hlc_allow_close_contactor and iec_allow_close_contactor) {
                    bsp->allow_power_on(true, types::evse_board_support::Reason::DCCableCheck);
                }
            }

            // Wait here until all errors are cleared
            if (errors_prevent_charging_internal()) {
                break;
            }

            // make sure we are enabling PWM
            if (not hlc_use_5percent_current_session) {
                update_pwm_now_if_changed(ampere_to_duty_cycle(get_max_current_internal()));
            } else {
                update_pwm_now_if_changed(PWM_5_PERCENT);
            }

            if (charge_mode == ChargeMode::AC) {
                // In AC mode BASIC, iec_allow is sufficient.  The same is true for HLC mode when nominal PWM is used as
                // the car can do BASIC and HLC charging any time. In AC HLC with 5 percent mode, we need to wait for
                // both iec_allow and hlc_allow.
                if ((iec_allow_close_contactor and not hlc_use_5percent_current_session) or
                    (iec_allow_close_contactor and hlc_allow_close_contactor and hlc_use_5percent_current_session)) {

                    signal_event(types::evse_manager::SessionEventEnum::ChargingStarted);

                    if (power_available()) {
                        current_state = EvseState::Charging;
                    } else {
                        current_state = EvseState::Charging;
                        pause_charging_wait_for_power_internal();
                    }
                }

                if (not hlc_charging_active and not legacy_wakeup_done and timeInCurrentState > legacy_wakeup_timeout) {
                    session_log.evse(
                        false,
                        "EV did not transition to state C, trying one legacy wakeup according to IEC61851-1 A.5.3");
                    legacy_wakeup_done = true;
                    t_step_EF_return_state = EvseState::PrepareCharging;
                    t_step_EF_return_pwm = ampere_to_duty_cycle(get_max_current_internal());
                    current_state = EvseState::T_step_EF;
                }
            }

            // if (charge_mode == ChargeMode::DC) {
            //  DC: wait until car requests power on CP (B->C/D).
            //  Then we close contactor and wait for instructions from HLC.
            //  HLC will perform CableCheck, PreCharge, and PowerDelivery.
            //  These are all controlled from the handlers directly, so there is nothing we need to do here.
            //  Once HLC informs us about CurrentDemand has started, we will go to Charging in the handler.
            //}

            break;

        case EvseState::Charging:
            if (initialize_state) {
                hlc_charging_terminate_pause = HlcTerminatePause::Unknown;
            }

            // Wait here until all errors are cleared
            if (errors_prevent_charging_internal()) {
                break;
            }

            if (charge_mode == ChargeMode::DC) {
                // FIXME: handle DC pause/resume here
                // FIXME: handle DC no power available from Energy management
            } else {
                check_soft_over_current();

                if (not power_available()) {
                    pause_charging_wait_for_power_internal();
                    break;
                }

                if (initialize_state) {
                    if (last_state not_eq EvseState::PrepareCharging) {
                        signal_event(types::evse_manager::SessionEventEnum::ChargingResumed);
                    }

                    // Allow another wake-up sequence
                    legacy_wakeup_done = false;

                    bsp->allow_power_on(true, types::evse_board_support::Reason::FullPowerCharging);
                    // make sure we are enabling PWM
                    if (hlc_use_5percent_current_session) {
                        update_pwm_now_if_changed(PWM_5_PERCENT);
                    } else {
                        update_pwm_now_if_changed(ampere_to_duty_cycle(get_max_current_internal()));
                    }
                } else {
                    // update PWM if it has changed and 5 seconds have passed since last update
                    if (not hlc_use_5percent_current_session) {
                        update_pwm_max_every_5seconds(ampere_to_duty_cycle(get_max_current_internal()));
                    }
                }
            }
            break;

        case EvseState::ChargingPausedEV:

            if (charge_mode == ChargeMode::AC) {
                check_soft_over_current();
            }

            // A pause issued by the EV needs to be handled differently for the different charging modes

            // 1) BASIC AC charging: Nominal PWM needs be running, so the EV can actually resume charging when it wants
            // to

            // 2) HLC charging: [V2G3-M07-19] requires the EV to switch to state B, so we will end up here in this state
            //    [V2G3-M07-20] forces us to switch off PWM.
            //    This is also true for nominal PWM AC HLC charging, so an EV that does HLC AC and pauses can only
            //    resume in HLC mode and not in BASIC charging.

            if (hlc_charging_active) {
                // This is for HLC charging (both AC and DC)
                if (initialize_state) {
                    bcb_toggle_reset();
                    bsp->allow_power_on(false, types::evse_board_support::Reason::PowerOff);
                    if (charge_mode == ChargeMode::DC) {
                        signal_dc_supply_off();
                    }
                    signal_event(types::evse_manager::SessionEventEnum::ChargingPausedEV);
                }

                if (bcb_toggle_detected()) {
                    current_state = EvseState::PrepareCharging;
                }

                // We come here by a state C->B transition but the ISO message may not have arrived yet,
                // so we wait here until we know wether it is Terminate or Pause. Until we leave PWM on (should not be
                // shut down before SessionStop.req)

                if (hlc_charging_terminate_pause == HlcTerminatePause::Terminate) {
                    // EV wants to terminate session
                    current_state = EvseState::StoppingCharging;
                    if (pwm_running) {
                        pwm_off();
                    }
                } else if (hlc_charging_terminate_pause == HlcTerminatePause::Pause) {
                    // EV wants an actual pause
                    if (pwm_running) {
                        pwm_off();
                    }
                }

            } else {
                // This is for BASIC charging only

                // Normally power should be available, since we request a minimum power also during EV pause.
                // In case the energy manager gives us no energy, we effectivly switch to a pause by EVSE here.
                if (not power_available()) {
                    pause_charging_wait_for_power_internal();
                    break;
                }

                if (initialize_state) {
                    signal_event(types::evse_manager::SessionEventEnum::ChargingPausedEV);
                } else {
                    // update PWM if it has changed and 5 seconds have passed since last update
                    if (not errors_prevent_charging_internal()) {
                        update_pwm_max_every_5seconds(ampere_to_duty_cycle(get_max_current_internal()));
                    }
                }
            }
            break;

        case EvseState::ChargingPausedEVSE:
            if (initialize_state) {
                signal_event(types::evse_manager::SessionEventEnum::ChargingPausedEVSE);
                if (hlc_charging_active) {
                    // currentState = EvseState::StoppingCharging;
                    last_stop_transaction_reason = types::evse_manager::StopTransactionReason::Local;
                    // tell HLC stack to stop the session
                    signal_hlc_stop_charging();
                    pwm_off();
                } else {
                    pwm_off();
                }
            }
            break;

        case EvseState::WaitingForEnergy:
            if (initialize_state) {
                signal_event(types::evse_manager::SessionEventEnum::WaitingForEnergy);
                if (not hlc_use_5percent_current_session) {
                    pwm_off();
                }
            }
            break;

        case EvseState::StoppingCharging:
            if (initialize_state) {
                bcb_toggle_reset();
                if (transaction_active or session_active) {
                    signal_event(types::evse_manager::SessionEventEnum::StoppingCharging);
                }

                if (hlc_charging_active) {
                    if (charge_mode == ChargeMode::DC) {
                        // DC supply off - actually this is after relais switched off
                        // this is a backup switch off, normally it should be switched off earlier by ISO protocol.
                        signal_dc_supply_off();
                    }
                    // Car is maybe not unplugged yet, so for HLC(AC/DC) wait in this state. We will go to Finished once
                    // car is unplugged.
                } else {
                    // For AC BASIC charging, we reached StoppingCharging because an unplug happend.
                    pwm_off();
                    current_state = EvseState::Finished;
                }
            }

            // Allow session restart after SessionStop.terminate (full restart including new SLAC).
            // Only allow that if the transaction is still running. If it was cancelled externally with
            // cancel_transaction(), we do not allow restart. If OCPP cancels a transaction it assumes it cannot be
            // restarted. In all other cases, e.g. the EV stopping the transaction it may resume with a BCB toggle.
            if (hlc_charging_active and bcb_toggle_detected()) {
                if (transaction_active) {
                    current_state = EvseState::PrepareCharging;
                    // wake up SLAC as well
                    signal_slac_start();
                } else {
                    session_log.car(false, "Car requested restarting with BCB toggle. Ignored, since we were cancelled "
                                           "externally before.");
                }
            }
            break;

        case EvseState::Finished:

            if (initialize_state) {
                // Transaction may already be stopped when it was cancelled earlier.
                // In that case, do not sent a second transactionFinished event.
                if (transaction_active) {
                    stop_transaction();
                }
                // We may come here from an error state, so a session was maybe not active.
                if (session_active) {
                    stop_session();
                }

                if (charge_mode == ChargeMode::DC) {
                    signal_dc_supply_off();
                }
            }

            current_state = EvseState::Idle;
            break;
        }
    } while (last_state_detect_state_change not_eq current_state);
}

void Charger::process_event(CPEvent cp_event) {
    switch (cp_event) {
    case CPEvent::CarPluggedIn:
    case CPEvent::CarRequestedPower:
    case CPEvent::CarRequestedStopPower:
    case CPEvent::CarUnplugged:
    case CPEvent::BCDtoEF:
    case CPEvent::EFtoBCD:
        session_log.car(false, fmt::format("Event {}", cpevent_to_string(cp_event)));
        break;
    default:
        session_log.evse(false, fmt::format("Event {}", cpevent_to_string(cp_event)));
        break;
    }

    std::scoped_lock lock(state_machine_mutex);

    if (cp_event == CPEvent::PowerOn) {
        contactors_closed = true;
    } else if (cp_event == CPEvent::PowerOff) {
        contactors_closed = false;
    }

    run_state_machine();

    // Process all event actions that are independent of the current state
    process_cp_events_independent(cp_event);

    run_state_machine();

    // Process all events that depend on the current state
    process_cp_events_state(cp_event);

    run_state_machine();
}

void Charger::process_cp_events_state(CPEvent cp_event) {
    switch (current_state) {

    case EvseState::Idle:
        if (cp_event == CPEvent::CarPluggedIn) {
            current_state = EvseState::WaitingForAuthentication;
        }
        break;

    case EvseState::WaitingForAuthentication:
        if (cp_event == CPEvent::CarRequestedPower) {
            session_log.car(false, "B->C transition before PWM is enabled at this stage violates IEC61851-1");
            iec_allow_close_contactor = true;
        } else if (cp_event == CPEvent::CarRequestedStopPower) {
            session_log.car(false, "C->B transition at this stage violates IEC61851-1");
            iec_allow_close_contactor = false;
        }
        break;

    case EvseState::PrepareCharging:
        if (cp_event == CPEvent::CarRequestedPower) {
            iec_allow_close_contactor = true;
        } else if (cp_event == CPEvent::CarRequestedStopPower) {
            iec_allow_close_contactor = false;
            // current_state = EvseState::StoppingCharging;
        }
        break;

    case EvseState::Charging:
        if (cp_event == CPEvent::CarRequestedStopPower) {
            iec_allow_close_contactor = false;
            current_state = EvseState::ChargingPausedEV;
        }
        break;

    case EvseState::ChargingPausedEV:
        if (cp_event == CPEvent::CarRequestedPower) {
            iec_allow_close_contactor = true;
            // For BASIC charging we can simply switch back to Charging
            if (charge_mode == ChargeMode::AC and not hlc_charging_active) {
                current_state = EvseState::Charging;
            } else if (not pwm_running) {
                bcb_toggle_detect_start_pulse();
            }
        }

        if (cp_event == CPEvent::CarRequestedStopPower and not pwm_running and hlc_charging_active) {
            bcb_toggle_detect_stop_pulse();
        }
        break;

    case EvseState::StoppingCharging:
        // Allow session restart from EV after SessionStop.terminate with BCB toggle
        if (hlc_charging_active and not pwm_running) {
            if (cp_event == CPEvent::CarRequestedPower) {
                bcb_toggle_detect_start_pulse();
            } else if (cp_event == CPEvent::CarRequestedStopPower) {
                bcb_toggle_detect_stop_pulse();
            }
        }
        break;

    default:
        break;
    }
}

void Charger::process_cp_events_independent(CPEvent cp_event) {
    switch (cp_event) {
    case CPEvent::EvseReplugStarted:
        current_state = EvseState::Replug;
        break;
    case CPEvent::EvseReplugFinished:
        current_state = EvseState::WaitingForAuthentication;
        break;
    case CPEvent::CarUnplugged:
        if (not hlc_charging_active) {
            current_state = EvseState::StoppingCharging;
        } else {
            current_state = EvseState::Finished;
        }
        break;
    default:
        break;
    }
}

void Charger::update_pwm_max_every_5seconds(float dc) {
    if (dc not_eq update_pwm_last_dc) {
        auto now = date::utc_clock::now();
        auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pwm_update).count();
        if (timeSinceLastUpdate >= 5000) {
            update_pwm_now(dc);
        }
    }
}

void Charger::update_pwm_now(float dc) {
    auto start = date::utc_clock::now();
    update_pwm_last_dc = dc;
    pwm_running = true;
    bsp->set_pwm(dc);

    session_log.evse(
        false,
        fmt::format("Set PWM On ({}%) took {} ms", dc * 100.,
                    (std::chrono::duration_cast<std::chrono::milliseconds>(date::utc_clock::now() - start)).count()));
    last_pwm_update = date::utc_clock::now();
}

void Charger::update_pwm_now_if_changed(float dc) {
    if (update_pwm_last_dc not_eq dc) {
        update_pwm_now(dc);
    }
}

void Charger::pwm_off() {
    session_log.evse(false, "Set PWM Off");
    pwm_running = false;
    update_pwm_last_dc = 1.;
    bsp->set_pwm_off();
}

void Charger::pwm_F() {
    session_log.evse(false, "Set PWM F");
    pwm_running = false;
    update_pwm_last_dc = 0.;
    bsp->set_pwm_F();
}

void Charger::run() {
    // spawn new thread and return
    main_thread_handle = std::thread(&Charger::main_thread, this);
}

float Charger::ampere_to_duty_cycle(float ampere) {
    float dc = 0;

    // calculate max current
    if (ampere < 5.9) {
        // Invalid argument, switch to error
        dc = 1.0;
    } else if (ampere <= 6.1) {
        dc = 0.1;
    } else if (ampere < 52.5) {
        if (ampere > 51.)
            ampere = 51; // Weird gap in norm: 51A .. 52.5A has no defined PWM.
        dc = ampere / 0.6 / 100.;
    } else if (ampere <= 80) {
        dc = ((ampere / 2.5) + 64) / 100.;
    } else if (ampere <= 80.1) {
        dc = 0.97;
    } else {
        // Invalid argument, switch to error
        dc = 1.0;
    }

    return dc;
}

bool Charger::set_max_current(float c, std::chrono::time_point<date::utc_clock> validUntil) {
    if (c >= 0.0 and c <= CHARGER_ABSOLUTE_MAX_CURRENT) {

        // is it still valid?
        if (validUntil > date::utc_clock::now()) {
            {
                std::lock_guard lock(state_machine_mutex);
                max_current = c;
                max_current_valid_until = validUntil;
            }
            bsp->set_overcurrent_limit(c);
            signal_max_current(c);
            return true;
        }
    }
    return false;
}

// pause if currently charging, else do nothing.
bool Charger::pause_charging() {
    std::scoped_lock lock(state_machine_mutex);
    if (current_state == EvseState::Charging) {
        legacy_wakeup_done = false;
        current_state = EvseState::ChargingPausedEVSE;
        return true;
    }
    return false;
}

bool Charger::resume_charging() {
    std::scoped_lock lock(state_machine_mutex);

    if (hlc_charging_active and transaction_active and current_state == EvseState::ChargingPausedEVSE) {
        current_state = EvseState::PrepareCharging;
        // wake up SLAC as well
        signal_slac_start();
        return true;
    } else if (transaction_active and current_state == EvseState::ChargingPausedEVSE) {
        current_state = EvseState::WaitingForEnergy;
        return true;
    }

    return false;
}

// pause charging since no power is available at the moment
bool Charger::pause_charging_wait_for_power() {
    std::scoped_lock lock(state_machine_mutex);
    return pause_charging_wait_for_power_internal();
}

// pause charging since no power is available at the moment
bool Charger::pause_charging_wait_for_power_internal() {
    if (current_state == EvseState::Charging) {
        current_state = EvseState::WaitingForEnergy;
        return true;
    }
    return false;
}

// resume charging since power became available. Does not resume if user paused charging.
bool Charger::resume_charging_power_available() {
    std::scoped_lock lock(state_machine_mutex);

    if (transaction_active and current_state == EvseState::WaitingForEnergy and power_available()) {
        current_state = EvseState::Charging;
        return true;
    }
    return false;
}

// pause charging since we run through replug sequence
bool Charger::evse_replug() {
    // call BSP to start the replug sequence. It BSP actually does it,
    // it will emit a EvseReplugStarted event which will then modify our state.
    // If BSP never executes the replug, we also never change state and nothing happens.
    // After replugging finishes, BSP will emit EvseReplugFinished event and we will go back to WaitingForAuth
    EVLOG_info << fmt::format("Calling evse_replug({})...", T_REPLUG_MS);
    bsp->evse_replug(T_REPLUG_MS);
    return true;
}

// Cancel transaction/charging from external EvseManager interface (e.g. via OCPP)
bool Charger::cancel_transaction(const types::evse_manager::StopTransactionRequest& request) {
    std::scoped_lock lock(state_machine_mutex);

    if (transaction_active) {
        if (hlc_charging_active) {
            current_state = EvseState::StoppingCharging;
            signal_hlc_stop_charging();
        } else {
            current_state = EvseState::ChargingPausedEVSE;
        }

        transaction_active = false;
        last_stop_transaction_reason = request.reason;
        if (request.id_tag) {
            stop_transaction_id_token = request.id_tag.value();
        }
        signal_event(types::evse_manager::SessionEventEnum::ChargingFinished);
        signal_event(types::evse_manager::SessionEventEnum::TransactionFinished);
        return true;
    }
    return false;
}

void Charger::start_session(bool authfirst) {
    session_active = true;
    authorized = false;
    if (authfirst)
        last_start_session_reason = types::evse_manager::StartSessionReason::Authorized;
    else
        last_start_session_reason = types::evse_manager::StartSessionReason::EVConnected;
    signal_event(types::evse_manager::SessionEventEnum::SessionStarted);
}

void Charger::stop_session() {
    session_active = false;
    authorized = false;
    signal_event(types::evse_manager::SessionEventEnum::SessionFinished);
}

void Charger::start_transaction() {
    stop_transaction_id_token.reset();
    transaction_active = true;
    signal_event(types::evse_manager::SessionEventEnum::TransactionStarted);
}

void Charger::stop_transaction() {
    transaction_active = false;
    last_stop_transaction_reason = types::evse_manager::StopTransactionReason::EVDisconnected;
    signal_event(types::evse_manager::SessionEventEnum::ChargingFinished);
    signal_event(types::evse_manager::SessionEventEnum::TransactionFinished);
}

std::optional<types::authorization::ProvidedIdToken> Charger::get_stop_transaction_id_token() {
    std::lock_guard lock(state_machine_mutex);
    return stop_transaction_id_token;
}

types::evse_manager::StopTransactionReason Charger::get_transaction_finished_reason() {
    std::scoped_lock lock(state_machine_mutex);
    return last_stop_transaction_reason;
}

types::evse_manager::StartSessionReason Charger::get_session_started_reason() {
    std::scoped_lock lock(state_machine_mutex);
    return last_start_session_reason;
}

bool Charger::switch_three_phases_while_charging(bool n) {
    bsp->switch_three_phases_while_charging(n);
    return false; // FIXME: implement real return value when protobuf has sync calls
}

void Charger::setup(bool three_phases, bool has_ventilation, const std::string& country_code,
                    const ChargeMode _charge_mode, bool _ac_hlc_enabled, bool _ac_hlc_use_5percent,
                    bool _ac_enforce_hlc, bool _ac_with_soc_timeout, float _soft_over_current_tolerance_percent,
                    float _soft_over_current_measurement_noise_A) {
    // set up board support package
    bsp->setup(three_phases, has_ventilation, country_code);

    std::scoped_lock lock(state_machine_mutex);
    // cache our config variables
    charge_mode = _charge_mode;
    ac_hlc_enabled_current_session = ac_hlc_enabled = _ac_hlc_enabled;
    ac_hlc_use_5percent = _ac_hlc_use_5percent;
    ac_enforce_hlc = _ac_enforce_hlc;
    ac_with_soc_timeout = _ac_with_soc_timeout;
    ac_with_soc_timer = 3600000;
    soft_over_current_tolerance_percent = _soft_over_current_tolerance_percent;
    soft_over_current_measurement_noise_A = _soft_over_current_measurement_noise_A;
    if (charge_mode == ChargeMode::AC and ac_hlc_enabled)
        EVLOG_info << "AC HLC mode enabled.";
}

Charger::EvseState Charger::get_current_state() {
    std::scoped_lock lock(state_machine_mutex);
    return current_state;
}

bool Charger::get_authorized_pnc() {
    std::scoped_lock lock(state_machine_mutex);
    return (authorized and authorized_pnc);
}

bool Charger::get_authorized_eim() {
    std::scoped_lock lock(state_machine_mutex);
    return (authorized and not authorized_pnc);
}

bool Charger::get_authorized_pnc_ready_for_hlc() {
    bool auth = false, ready = false;
    std::scoped_lock lock(state_machine_mutex);
    auth = (authorized and authorized_pnc);
    ready = (current_state == EvseState::ChargingPausedEV) or (current_state == EvseState::ChargingPausedEVSE) or
            (current_state == EvseState::Charging) or (current_state == EvseState::WaitingForEnergy);
    return (auth and ready);
}

bool Charger::get_authorized_eim_ready_for_hlc() {
    bool auth = false, ready = false;
    std::scoped_lock lock(state_machine_mutex);
    auth = (authorized and not authorized_pnc);
    ready = (current_state == EvseState::ChargingPausedEV) or (current_state == EvseState::ChargingPausedEVSE) or
            (current_state == EvseState::Charging) or (current_state == EvseState::WaitingForEnergy);
    return (auth and ready);
}

void Charger::authorize(bool a, const types::authorization::ProvidedIdToken& token) {
    std::scoped_lock lock(state_machine_mutex);
    if (a) {
        // First user interaction was auth? Then start session already here and not at plug in
        if (not session_active) {
            start_session(true);
        }
        authorized = true;
        authorized_pnc = token.authorization_type == types::authorization::AuthorizationType::PlugAndCharge;
        id_token = token;
    } else {
        if (session_active) {
            stop_session();
        }
        authorized = false;
    }
}

types::authorization::ProvidedIdToken Charger::get_id_token() {
    std::scoped_lock lock(state_machine_mutex);
    return id_token;
}

bool Charger::deauthorize() {
    std::scoped_lock lock(state_machine_mutex);
    return deauthorize_internal();
}

bool Charger::deauthorize_internal() {
    if (session_active) {
        auto s = current_state;

        if (s == EvseState::Disabled or s == EvseState::Idle or s == EvseState::WaitingForAuthentication) {

            // We can safely remove auth as it is not in use right now
            if (not authorized) {
                signal_event(types::evse_manager::SessionEventEnum::PluginTimeout);
                return false;
            }
            authorized = false;
            stop_session();
            return true;
        }
    }
    return false;
}

bool Charger::disable(int connector_id) {
    std::scoped_lock lock(state_machine_mutex);
    if (connector_id not_eq 0) {
        connector_enabled = false;
    }
    current_state = EvseState::Disabled;
    signal_event(types::evse_manager::SessionEventEnum::Disabled);
    return true;
}

bool Charger::enable(int connector_id) {
    std::scoped_lock lock(state_machine_mutex);

    if (connector_id not_eq 0) {
        connector_enabled = true;
    }

    signal_event(types::evse_manager::SessionEventEnum::Enabled);
    if (current_state == EvseState::Disabled) {
        if (connector_enabled) {
            current_state = EvseState::Idle;
        }
        return true;
    }
    return false;
}

void Charger::set_faulted() {
    std::scoped_lock lock(state_machine_mutex);
    error_prevent_charging_flag = true;
}

std::string Charger::evse_state_to_string(EvseState s) {
    switch (s) {
    case EvseState::Disabled:
        return ("Disabled");
        break;
    case EvseState::Idle:
        return ("Idle");
        break;
    case EvseState::WaitingForAuthentication:
        return ("Wait for Auth");
        break;
    case EvseState::Charging:
        return ("Charging");
        break;
    case EvseState::ChargingPausedEV:
        return ("Car Paused");
        break;
    case EvseState::ChargingPausedEVSE:
        return ("EVSE Paused");
        break;
    case EvseState::WaitingForEnergy:
        return ("Wait for energy");
        break;
    case EvseState::Finished:
        return ("Finished");
        break;
    case EvseState::T_step_EF:
        return ("T_step_EF");
        break;
    case EvseState::T_step_X1:
        return ("T_step_X1");
        break;
    case EvseState::Replug:
        return ("Replug");
        break;
    case EvseState::PrepareCharging:
        return ("PrepareCharging");
        break;
    case EvseState::StoppingCharging:
        return ("StoppingCharging");
        break;
    }
    return "Invalid";
}

float Charger::get_max_current() {
    std::scoped_lock lock(state_machine_mutex);
    return get_max_current_internal();
}

float Charger::get_max_current_internal() {
    auto maxc = max_current;

    if (connector_type == types::evse_board_support::Connector_type::IEC62196Type2Socket and
        max_current_cable < maxc and current_state not_eq EvseState::Idle) {
        maxc = max_current_cable;
    }

    return maxc;
}

void Charger::set_current_drawn_by_vehicle(float l1, float l2, float l3) {
    std::scoped_lock lock(state_machine_mutex);
    current_drawn_by_vehicle[0] = l1;
    current_drawn_by_vehicle[1] = l2;
    current_drawn_by_vehicle[2] = l3;
}

void Charger::check_soft_over_current() {
    // Allow some tolerance
    float limit = (get_max_current_internal() + soft_over_current_measurement_noise_A) *
                  (1. + soft_over_current_tolerance_percent / 100.);

    if (current_drawn_by_vehicle[0] > limit or current_drawn_by_vehicle[1] > limit or
        current_drawn_by_vehicle[2] > limit) {
        if (not over_current) {
            over_current = true;
            // timestamp when over current happend first
            last_over_current_event = date::utc_clock::now();
            session_log.evse(false,
                             fmt::format("Soft overcurrent event (L1:{}, L2:{}, L3:{}, limit {}), starting timer.",
                                         current_drawn_by_vehicle[0], current_drawn_by_vehicle[1],
                                         current_drawn_by_vehicle[2], limit));
        }
    } else {
        over_current = false;
    }
    auto now = date::utc_clock::now();
    auto timeSinceOverCurrentStarted =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_over_current_event).count();
    if (over_current and timeSinceOverCurrentStarted >= soft_over_current_timeout) {
        auto errstr =
            fmt::format("Soft overcurrent event (L1:{}, L2:{}, L3:{}, limit {}) triggered", current_drawn_by_vehicle[0],
                        current_drawn_by_vehicle[1], current_drawn_by_vehicle[2], limit);
        session_log.evse(false, errstr);
        // raise the OC error
        error_handling->raise_overcurrent_error(errstr);
    }
}

// returns whether power is actually available from EnergyManager
// i.e. max_current is in valid range
bool Charger::power_available() {
    if (max_current_valid_until < date::utc_clock::now()) {
        EVLOG_warning << "Power budget expired, falling back to 0.";
        max_current = 0.;
        signal_max_current(max_current);
    }
    return (get_max_current_internal() > 5.9);
}

void Charger::request_error_sequence() {
    std::scoped_lock lock(state_machine_mutex);
    if (current_state == EvseState::WaitingForAuthentication or current_state == EvseState::PrepareCharging) {
        t_step_EF_return_state = current_state;
        current_state = EvseState::T_step_EF;
        signal_slac_reset();
        if (hlc_use_5percent_current_session) {
            t_step_EF_return_pwm = PWM_5_PERCENT;
        } else {
            t_step_EF_return_pwm = 0.;
        }
    }
}

void Charger::set_matching_started(bool m) {
    std::scoped_lock lock(state_machine_mutex);
    matching_started = m;
}

bool Charger::get_matching_started() {
    std::scoped_lock lock(state_machine_mutex);
    return matching_started;
}

void Charger::notify_currentdemand_started() {
    std::scoped_lock lock(state_machine_mutex);
    if (current_state == EvseState::PrepareCharging) {
        signal_event(types::evse_manager::SessionEventEnum::ChargingStarted);
        current_state = EvseState::Charging;
    }
}

void Charger::inform_new_evse_max_hlc_limits(
    const types::iso15118_charger::DC_EVSEMaximumLimits& _currentEvseMaxLimits) {
    std::scoped_lock lock(state_machine_mutex);
    current_evse_max_limits = _currentEvseMaxLimits;
}

types::iso15118_charger::DC_EVSEMaximumLimits Charger::get_evse_max_hlc_limits() {
    std::scoped_lock lock(state_machine_mutex);
    return current_evse_max_limits;
}

// HLC stack signalled a pause request for the lower layers.
void Charger::dlink_pause() {
    std::scoped_lock lock(state_machine_mutex);
    hlc_allow_close_contactor = false;
    pwm_off();
    hlc_charging_terminate_pause = HlcTerminatePause::Pause;
}

// HLC requested end of charging session, so we can stop the 5% PWM
void Charger::dlink_terminate() {
    std::scoped_lock lock(state_machine_mutex);
    hlc_allow_close_contactor = false;
    pwm_off();
    hlc_charging_terminate_pause = HlcTerminatePause::Terminate;
}

void Charger::dlink_error() {
    std::scoped_lock lock(state_machine_mutex);

    hlc_allow_close_contactor = false;

    // Is PWM on at the moment?
    if (not pwm_running) {
        // [V2G3-M07-04]: With receiving a D-LINK_ERROR.request from HLE in X1 state, the EVSE’s communication node
        // shall perform a state X1 to state E/F to state X1 or X2 transition.
    } else {
        // [V2G3-M07-05]: With receiving a D-LINK_ERROR.request in X2 state from HLE, the EVSE’s communication node
        // shall perform a state X2 to X1 to state E/F to state X1 or X2 transition.

        // Are we in 5% mode or not?
        if (hlc_use_5percent_current_session) {
            // [V2G3-M07-06] Within the control pilot state X1, the communication node shall leave the logical
            // network and change the matching state to "Unmatched". [V2G3-M07-07] With reaching the state
            // “Unmatched”, the EVSE shall switch to state E/F.

            // FIXME: We don't wait for SLAC to go to UNMATCHED in X1 for now but just do a normal 3 seconds
            // t_step_X1 instead. This should be more then sufficient for the SLAC module to reset.

            // Do t_step_X1 with a t_step_EF afterwards
            // [V2G3-M07-08] The state E/F shall be applied at least T_step_EF: This is already handled in the
            // t_step_EF state.
            t_step_X1_return_state = EvseState::T_step_EF;
            t_step_X1_return_pwm = 0.;
            current_state = EvseState::T_step_X1;

            // After returning from T_step_EF, go to Waiting for Auth (We are restarting the session)
            t_step_EF_return_state = EvseState::WaitingForAuthentication;
            // [V2G3-M07-09] After applying state E/F, the EVSE shall switch to contol pilot state X1 or X2 as soon
            // as the EVSE is ready control for pilot incoming duty matching cycle requests: This is already handled
            // in the Auth step.

            // [V2G3-M07-05] says we need to go through X1 at the end of the sequence
            t_step_EF_return_pwm = 0.;
        }
        // else {
        // [V2G3-M07-10] Gives us two options for nominal PWM mode and HLC in case of error: We choose [V2G3-M07-12]
        // (Don't interrupt basic AC charging just because an error in HLC happend)
        // So we don't do anything here, SLAC will be notified anyway to reset
        //}
    }
}

void Charger::set_hlc_charging_active() {
    std::scoped_lock lock(state_machine_mutex);
    hlc_charging_active = true;
}

void Charger::set_hlc_allow_close_contactor(bool on) {
    std::scoped_lock lock(state_machine_mutex);
    hlc_allow_close_contactor = on;
}

void Charger::set_hlc_error() {
    std::scoped_lock lock(state_machine_mutex);
    error_prevent_charging_flag = true;
}

// this resets the BCB sequence (which may contain 1-3 toggle pulses)
void Charger::bcb_toggle_reset() {
    hlc_ev_pause_bcb_count = 0;
    hlc_bcb_sequence_started = false;
}

// call this B->C transitions
void Charger::bcb_toggle_detect_start_pulse() {
    // For HLC charging, PWM already off: This is probably a BCB Toggle to wake us up from sleep mode.
    // Remember start of BCB toggle.
    hlc_ev_pause_start_of_bcb = std::chrono::steady_clock::now();
    if (hlc_ev_pause_bcb_count == 0) {
        // remember sequence start
        hlc_ev_pause_start_of_bcb_sequence = std::chrono::steady_clock::now();
        hlc_bcb_sequence_started = true;
    }
}

// call this on C->B transitions
void Charger::bcb_toggle_detect_stop_pulse() {
    if (not hlc_bcb_sequence_started) {
        return;
    }

    // This is probably and end of BCB toggle, verify it was not too long or too short
    auto pulse_length = std::chrono::steady_clock::now() - hlc_ev_pause_start_of_bcb;

    if (pulse_length > TP_EV_VALD_STATE_DURATION_MIN and pulse_length < TP_EV_VALD_STATE_DURATION_MAX) {

        // enable PWM again. ISO stack should have been ready for the whole time.
        // FIXME where do we go from here? Auth?
        hlc_ev_pause_bcb_count++;

        session_log.car(false, fmt::format("BCB toggle ({} ms), #{} in sequence",
                                           std::chrono::duration_cast<std::chrono::milliseconds>(pulse_length).count(),
                                           hlc_ev_pause_bcb_count));

    } else {
        hlc_ev_pause_bcb_count = 0;
        EVLOG_warning << "BCB toggle with invalid duration detected: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(pulse_length).count();
    }
}

// Query if a BCB sequence (of 1-3 pulses) was detected and is finished. If that is true, PWM can be enabled again
// etc
bool Charger::bcb_toggle_detected() {
    auto sequence_length = std::chrono::steady_clock::now() - hlc_ev_pause_start_of_bcb_sequence;
    if (hlc_bcb_sequence_started and (sequence_length > TT_EVSE_VALD_TOGGLE or hlc_ev_pause_bcb_count >= 3)) {
        // no need to wait for further BCB toggles
        hlc_ev_pause_bcb_count = 0;
        return true;
    }
    return false;
}

void Charger::set_rcd_error() {
    std::scoped_lock lock(state_machine_mutex);
    error_prevent_charging_flag = true;
}

bool Charger::errors_prevent_charging() {
    std::scoped_lock lock(state_machine_mutex);
    return errors_prevent_charging_internal();
}

bool Charger::errors_prevent_charging_internal() {
    if (error_prevent_charging_flag) {
        graceful_stop_charging();
        return true;
    }
    return false;
}

void Charger::graceful_stop_charging() {

    if (pwm_running) {
        pwm_off();
    }

    // Shutdown DC power supplies
    if (charge_mode == ChargeMode::DC) {
        signal_dc_supply_off();
    }

    // open contactors
    if (contactors_closed) {
        bsp->allow_power_on(false, types::evse_board_support::Reason::PowerOff);
    }
}

void Charger::clear_errors_on_unplug() {
    error_handling->clear_overcurrent_error();
    error_handling->clear_internal_error();
    error_handling->clear_powermeter_transaction_start_failed_error();
}

} // namespace module
