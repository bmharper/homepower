#include <time.h>
#include <unistd.h>
#include "../bcm2835/bcm2835.h"
#include "controller.h"
#include "monitor.h"

using namespace std;

namespace homepower {

const char* HeavyLoadModeToString(HeavyLoadMode mode) {
	switch (mode) {
	case HeavyLoadMode::AlwaysOn: return "AlwaysOn";
	case HeavyLoadMode::OnWithSolar: return "OnWithSolar";
	}
	return "INVALID";
	// unreachable
}

const char* HeavyLoadStateToString(HeavyLoadState mode) {
	switch (mode) {
	case HeavyLoadState::Off: return "Off";
	case HeavyLoadState::Grid: return "Grid";
	case HeavyLoadState::Inverter: return "Inverter";
	}
	return "INVALID";
	// unreachable
}

Controller::Controller(homepower::Monitor* monitor, bool enableGpio, bool enableInverterStateChange) {
	// I don't know of any way to read the state of the GPIO output pins using WiringPI,
	// so in order to know our state at startup, we need to create it.
	// This seems like a conservative thing to do anyway.
	// I'm sure there is a way to read the state using other mechanisms, but I don't
	// have any need for that, because this server is intended to come on and stay
	// on for months, without a restart.
	Monitor                   = monitor;
	MustExit                  = false;
	EnableGpio                = enableGpio;
	EnableInverterStateChange = enableInverterStateChange;
	if (EnableGpio) {
		if (bcm2835_init() == 0) {
			fprintf(stderr, "bcm2835_init failed\n");
			exit(1);
		}
		bcm2835_gpio_fsel(GpioPinGrid, BCM2835_GPIO_FSEL_OUTP);
		bcm2835_gpio_fsel(GpioPinInverter, BCM2835_GPIO_FSEL_OUTP);
		bcm2835_gpio_clr(GpioPinGrid);
		bcm2835_gpio_clr(GpioPinInverter);
	}
	CurrentHeavyLoadState = HeavyLoadState::Off;
	ChangePowerSourceMsg  = (int) PowerSource::Unknown;

	MinBattery[0]  = 45;
	MinBattery[1]  = 40;
	MinBattery[2]  = 35;
	MinBattery[3]  = 35;
	MinBattery[4]  = 35;
	MinBattery[5]  = 35;
	MinBattery[6]  = 35;
	MinBattery[7]  = 35;
	MinBattery[8]  = 35;
	MinBattery[9]  = 38; // On a sunny winter's day, our charge only increases by 3 percent from 8:20 (first direct sunlight) to 9:00.
	MinBattery[10] = 45;
	MinBattery[11] = 50;
	MinBattery[12] = 60;
	MinBattery[13] = 70;
	MinBattery[14] = 80;
	MinBattery[15] = 90;
	MinBattery[16] = 92;
	MinBattery[17] = 90;
	MinBattery[18] = 85;
	MinBattery[19] = 70;
	MinBattery[20] = 65;
	MinBattery[21] = 60;
	MinBattery[22] = 55;
	MinBattery[23] = 50;

	MinCharge[0] = {TimePoint(8, 0), 45, 35};
	MinCharge[1] = {TimePoint(16, 0), 94, 94};

	time_t    t  = time(nullptr);
	struct tm lt = {0};
	localtime_r(&t, &lt);
	TimezoneOffsetMinutes = (int) (lt.tm_gmtoff / 60);
	fprintf(stderr, "Offset to GMT is %d minutes\n", TimezoneOffsetMinutes);

	auto now = Now();
	fprintf(stderr, "Time now (local): %d:%02d\n", now.Hour, now.Minute);

	LastEqualizeAt = time(nullptr);
}

Controller::~Controller() {
	if (EnableGpio) {
		bcm2835_close();
	}
}

bool Controller::Start() {
	if (!BakeChargeLimits()) {
		return false;
	}

	MustExit = false;
	Thread   = thread([&]() {
        fprintf(stderr, "Controller started\n");
        fprintf(stderr, "Auto charge: %s\n", EnableAutoCharge ? "yes" : "no");
        if (EnableAutoCharge) {
            PrintChargeLimits();
        }
        Run();
        fprintf(stderr, "Controller exited\n");
    });

	return true;
}

void Controller::Stop() {
	MustExit = true;
	Thread.join();
}

bool Controller::BakeChargeLimits() {
	if (NMinCharge < 2) {
		fprintf(stderr, "Too few MinCharge points (%d < 2)", NMinCharge);
		return false;
	}
	if (NMinCharge > MaxNMinChargePoints) {
		fprintf(stderr, "Too many MinCharge points (%d > %d)", NMinCharge, MaxNMinChargePoints);
		return false;
	}
	// Copy into flat arrays, so that we can use TimePoint::Interpolate()
	for (int i = 0; i < NMinCharge; i++) {
		if (i != 0 && MinCharge[i].Time <= MinCharge[i - 1].Time) {
			fprintf(stderr, "MinCharge points must be in increasing order");
			return false;
		}
		MinChargeTimePoints[i] = MinCharge[i].Time;
		MinChargeSoft[i]       = MinCharge[i].Soft;
		MinChargeHard[i]       = MinCharge[i].Hard;
	}
	return true;
}

void Controller::PrintChargeLimits() {
	fprintf(stderr, "Minimum battery charge percentage for each hour:\n");
	fprintf(stderr, "  Hour: Soft Hard\n");
	for (int i = 0; i < 24; i++) {
		float soft = TimePoint::Interpolate(TimePoint(i, 0), NMinCharge, MinChargeTimePoints, MinChargeSoft);
		float hard = TimePoint::Interpolate(TimePoint(i, 0), NMinCharge, MinChargeTimePoints, MinChargeHard);
		fprintf(stderr, "  %02dh:  %02.0f%%  %02.0f%%\n", i, soft, hard);
		//fprintf(stderr, "  %02dh: %02d%%\n", i, MinBattery[i]);
	}
}

void Controller::SetHeavyLoadMode(HeavyLoadMode m) {
	lock_guard<mutex> lock(HeavyLoadLock);
	fprintf(stderr, "Set heavy load mode to %s\n", HeavyLoadModeToString(m));
	CurrentHeavyLoadMode = m;
}

void Controller::SetHeavyLoadState(HeavyLoadState m, bool forceWrite) {
	lock_guard<mutex> lock(HeavyLoadLock);

	if (CurrentHeavyLoadState == m && !forceWrite)
		return;

	fprintf(stderr, "Set heavy load state to %s\n", HeavyLoadStateToString(m));

	// Ideally, we'd use a switchover device that can do zero crossing,
	// which means the switch waits until the AC signal crosses over 0 voltage.
	// Since we can't control that, what we do is impose an extra delay
	// in between switching the old one off, and switching the new one on.
	// Because our contactors are physical devices with delays in them,
	// we don't need to add much extra delay to be safe.
	timespec pause;
	pause.tv_sec  = 0;
	pause.tv_nsec = SwitchSleepMilliseconds * 1000 * 1000;

	if (m == HeavyLoadState::Inverter) {
		if (EnableGpio) {
			bcm2835_gpio_clr(GpioPinGrid);
		}
		nanosleep(&pause, nullptr);
		if (EnableGpio) {
			bcm2835_gpio_set(GpioPinInverter);
		}
		Monitor->IsHeavyOnInverter = true;
	} else if (m == HeavyLoadState::Grid) {
		if (EnableGpio) {
			bcm2835_gpio_clr(GpioPinInverter);
		}
		nanosleep(&pause, nullptr);
		if (EnableGpio) {
			bcm2835_gpio_set(GpioPinGrid);
		}
		Monitor->IsHeavyOnInverter = false;
	} else if (m == HeavyLoadState::Off) {
		if (EnableGpio) {
			bcm2835_gpio_clr(GpioPinInverter);
			bcm2835_gpio_clr(GpioPinGrid);
		}
		Monitor->IsHeavyOnInverter = false;
	}

	CurrentHeavyLoadState = m;
}

void Controller::ChangePowerSource(PowerSource source) {
	ChangePowerSourceMsg = (int) source;
}

void Controller::Run() {
	auto lastStatus    = 0;
	auto lastChargeMsg = 0;
	auto lastPVStatus  = 0;
	while (!MustExit) {
		time_t         now               = time(nullptr);
		auto           nowP              = Now();
		HeavyLoadState desiredHeavyState = HeavyLoadState::Grid;

		bool  monitorIsAlive = Monitor->IsInitialized;
		float avgSolarV      = Monitor->AvgSolarV;
		float batteryP       = Monitor->BatteryP;
		float avgBatteryP    = Monitor->AvgBatteryP;
		bool  hasGridPower   = Monitor->HasGridPower;
		float avgSolarW      = Monitor->AvgSolarW;
		float avgLoadW       = Monitor->AvgLoadW;

		HeavyLoadLock.lock();
		auto heavyMode = CurrentHeavyLoadMode;
		HeavyLoadLock.unlock();

		if (monitorIsAlive) {
			bool solarExceedsLoads = avgSolarW > avgLoadW;

			// This is a grace factor added so that we can do things like run a washing machine in the morning,
			// even if the load exceeds the solar capacity for a while. The thing we're trying to exclude here
			// is the situation where it's getting late in the day, and we're just needlessly draining the battery,
			// only to have to charge it later in the evening. In this situations, we'd rather use grid power to
			// reduce the charging/discharging losses.
			// In my house, heavy loads in the afternoon are almost always the airconditioners.
			bool earlyInDayAndBatteryOK = nowP.Hour >= 7 && nowP.Hour <= 15 && batteryP >= 45.0f;

			if (solarExceedsLoads) {
				// Use solar power for heavy loads
				desiredHeavyState = HeavyLoadState::Inverter;
			} else if (hasGridPower) {
				// Avoid transfer losses
				desiredHeavyState = HeavyLoadState::Grid;
			} else if (earlyInDayAndBatteryOK || heavyMode == HeavyLoadMode::AlwaysOn) {
				// For HeavyLoadMode::OnWithSolar: Allow heavy appliances to run in the morning
				// For HeavyLoadMode::AlwaysOn: No solar, no grid, but we must remain on
				desiredHeavyState = HeavyLoadState::Inverter;
			}

			if (Monitor->IsBatteryOverloaded || Monitor->IsOutputOverloaded || batteryP < 40.0f)
				desiredHeavyState = HeavyLoadState::Grid;
		}

		if (desiredHeavyState == HeavyLoadState::Grid && !hasGridPower) {
			// When the grid is off, and we don't have enough solar power, we switch all non-essential devices off.
			// This prevents them from being subject to a spike when the grid is switched back on again.
			// We assume that this grid spike only lasts a few milliseconds, and by the time we've detected
			// that the grid is back on, the spike has subsided. In other words, we make no attempt to add
			// an extra delay before switching the contactors back on. We assume our polling interval adds
			// enough delay.
			desiredHeavyState = HeavyLoadState::Off;
		}

		if (monitorIsAlive && now - lastStatus > 10 * 60) {
			lastStatus = now;
			fprintf(stderr, "hasGridPower: %s, avgSolarV: %.1f, OutputOverloaded: %s, BatteryOverloaded: %s (time %d:%02d)\n",
			        hasGridPower ? "yes" : "no", avgSolarV,
			        Monitor->IsOutputOverloaded ? "yes" : "no",
			        Monitor->IsBatteryOverloaded ? "yes" : "no",
			        nowP.Hour, nowP.Minute);
			fflush(stderr);
		}

		// Figure out whether we should be charging from grid or not, and running loads from grid or battery
		PowerSource     desiredSource         = CurrentPowerSource;
		ChargerPriority desiredChargePriority = CurrentChargePriority;
		bool            wantSoftSwitch        = false; // True if we're below our soft threshold and want to switch
		bool            wantHardSwitch        = false; // True if we're below our hard threshold and want to switch

		if (monitorIsAlive && EnableAutoCharge) {
			float softBatteryGoal = TimePoint::Interpolate(nowP, NMinCharge, MinChargeTimePoints, MinChargeSoft);
			float hardBatteryGoal = TimePoint::Interpolate(nowP, NMinCharge, MinChargeTimePoints, MinChargeHard);

			//int goalBatteryP = MinBattery[nowP.Hour];
			//if (ChargeStartedInHour == nowP.Hour) {
			//	// If we hit our trigger low battery threshold, then charge up until we're at least 5% above the trigger threshold,
			//	// and until we're charged up enough to not hit the charge threshold on the next hour.
			//	int currentPlus5 = min(100, MinBattery[nowP.Hour] + 5);
			//	int nextHour     = (nowP.Hour + 1) % 24;
			//	goalBatteryP     = max(currentPlus5, MinBattery[nextHour]);
			//}

			// If we hit either of our thresholds within the last 30 minutes, then raise the target
			// to ensure that we overshoot it by some margin. Otherwise we ping pong along the bottom.
			if (now - LastSoftSwitch < 30 * 60) {
				softBatteryGoal = std::min(softBatteryGoal + 10, 100.0f);
			}
			if (now - LastHardSwitch < 30 * 60) {
				hardBatteryGoal = std::min(hardBatteryGoal + 10, 100.0f);
			}

			if (avgBatteryP >= 100.0f)
				LastEqualizeAt = now;

			time_t secondsSinceLastEqualize = now - LastEqualizeAt;

			if (nowP.Hour >= 17) {
				// Ensure that we give the battery a chance to balance the cells every day, regardless of the SOC hourly goal.
				// Note that it is vital that we alter goalBatteryP up here, before any of the other decisions are made,
				// otherwise we can end up in a situation where we switch to SUB mode, and then immediately switch back to SBU,
				// because both sides of our "let's charge" and "we have enough" will be true at the same time.
				// It was due to this original bug that we added the MinSecondsBetweenSBUSwitches protection logic, to
				// prevent flipping between states too frequently. The inverter DID NOT enjoy this, and kept restarting itself.
				// We somewhat arbitrarily choose to do equalization after 5pm, because that happens to coincide with the time
				// when we expect the battery SOC to be close to 100%
				if (secondsSinceLastEqualize >= HoursBetweenEqualize * 3600) {
					// Ensure that we stay in charging mode until we're equalized.
					// The SOC can never be 200, so this will force SUB and Grid Charge modes.
					softBatteryGoal = max(softBatteryGoal, 200.0f);
					hardBatteryGoal = max(hardBatteryGoal, 200.0f);
				}
			}

			// A key thing about the voltronic MKS inverters is that once the battery is charged, and they're in SUB mode,
			// then they no longer use the solar power for anything besides running the inverter itself (around 50W).
			// For this reason, we want to be in SBU mode as much of the time as possible, so that we never waste sunlight.

			/*
			bool earlyInDayOK = false;
			bool lateInDayOK  = false;
			bool endOfDayOK   = false;

			if (nowP.Hour < 12) {
				// Here we're hopeful that the sun will shine even more.
				earlyInDayOK = batteryP >= goalBatteryP && avgSolarW >= avgLoadW * 1.2;
			} else {
				// Here we have a good amount of charge, and don't want to dump solar power just because our battery is full.
				// Some inverters (eg Voltronics) can't use solar to power loads when in SUB mode, so that's why switching
				// back to SBU is vital, in order to utilize that solar power.
				// We must make this switch some time before 100%, because by the time we reach 100%, the solarW will have
				// dropped to near zero, since the BMS is telling us that it is almost full. With my Pylontech UP5000 batteries,
				// 98% SOC is where the charge amps really start to drop off, so 96% is some grace on top of that.
				lateInDayOK = batteryP >= 96.0f && nowP.Hour >= 12 && avgSolarW >= avgLoadW;
			}

			if (nowP.Hour >= 17 || nowP.Hour <= 7) {
				// By this time solar power has pretty much dropped to zero, so we always go into battery mode at this time,
				// provided we're fully charged.
				endOfDayOK = avgBatteryP >= 100.0f;
			}
			*/

			if (monitorIsAlive && now - lastChargeMsg > 10 * 60) {
				lastChargeMsg = now;
				//fprintf(stderr, "Charge - Current: %s, ChargeStartedInHour: %d, goalBatteryP: %d, batteryP: %d, solarW: %.0f, loadW: %.0f, earlyInDayOK: %s, lateInDayOK: %s, endOfDayOK: %s, sinceEqualize: %.0f\n",
				//        PowerSourceDescribe(CurrentPowerSource), ChargeStartedInHour, goalBatteryP, batteryP, avgSolarW, avgLoadW, earlyInDayOK ? "yes" : "no", lateInDayOK ? "yes" : "no", endOfDayOK ? "yes" : "no", (float) secondsSinceLastEqualize);
				fprintf(stderr, "Mode: %s, SwitchPowerSourceAt: %d, SwitchChargerPriorityAt: %d, softBatteryGoal: %.1f, hardBatteryGoal: %.1f, batteryP: %.1f\n",
				        PowerSourceDescribe(CurrentPowerSource), int(now - SwitchPowerSourceAt), int(now - SwitchChargerPriorityAt), softBatteryGoal, hardBatteryGoal, batteryP);
				fprintf(stderr, "solarW: %.0f, loadW: %.0f, sinceEqualize: %.0f\n", avgSolarW, avgLoadW, (float) secondsSinceLastEqualize);
				fflush(stderr);
			}

			if (batteryP < hardBatteryGoal) {
				// We've hit our hard limit. We must charge at all costs.
				desiredSource         = PowerSource::SUB;
				desiredChargePriority = ChargerPriority::UtilitySolar;
				wantHardSwitch        = true;
				if (CurrentPowerSource != desiredSource || CurrentChargePriority != desiredChargePriority) {
					fprintf(stderr, "Battery is lower than hard limit (%.1f < %.1f), switching to SUB and Grid + Solar Charge\n", batteryP, hardBatteryGoal);
				}
			} else if (batteryP < softBatteryGoal) {
				// We've hit our soft limit. Switch loads to grid, to preserve battery cycling
				desiredSource         = PowerSource::SUB;
				desiredChargePriority = ChargerPriority::SolarOnly;
				wantSoftSwitch        = true;
				if (CurrentPowerSource != desiredSource || CurrentChargePriority != desiredChargePriority) {
					fprintf(stderr, "Battery is lower than soft limit (%.1f < %.1f), switching to SUB and Solar Only\n", batteryP, softBatteryGoal);
				}
			} else {
				// Our battery charge is good. We can switch to battery + solar only.
				desiredSource         = PowerSource::SBU;
				desiredChargePriority = ChargerPriority::SolarOnly;
				if (CurrentPowerSource != desiredSource || CurrentChargePriority != desiredChargePriority) {
					fprintf(stderr, "Battery is good (%.1f >= %.1f), switching to SBU and Solar Only\n", batteryP, softBatteryGoal);
				}
			}
		}

		// Check if we have a 'please change to X mode' request from our HTTP server
		// This is sloppy use of an atomic variable, but our needs are simple.
		auto specialRequest = (PowerSource) ChangePowerSourceMsg.load();

		if (specialRequest != PowerSource::Unknown) {
			// Ignore this - doesn't make sense anymore with auto charging modes.
			//desiredSource        = specialRequest;
			//ChargeStartedInHour  = -1;
			ChangePowerSourceMsg = (int) PowerSource::Unknown; // signal that we've made the desired change (aka reset the atomic toggle)
		}

		bool switchSuccess = false;

		if (desiredChargePriority != CurrentChargePriority &&
		    now - SwitchChargerPriorityAt > 5 * 60 &&
		    now - LastAttemptedSwitch > 10 &&
		    monitorIsAlive) {
			fprintf(stderr, "Switching charger priority from %s to %s\n", ChargerPriorityToString(CurrentChargePriority), ChargerPriorityToString(desiredChargePriority));
			LastAttemptedSwitch = now;
			bool ok             = true;
			if (!EnableInverterStateChange) {
				fprintf(stderr, "EnableInverterStateChange is false, so not actually running command\n");
			} else {
				ok = Monitor->RunInverterCmd(ChargerPriorityToString(desiredChargePriority));
			}
			if (ok) {
				switchSuccess           = true;
				CurrentChargePriority   = desiredChargePriority;
				SwitchChargerPriorityAt = now;
			}
		}

		if (desiredSource != CurrentPowerSource &&
		    now - SwitchPowerSourceAt > 5 * 60 &&
		    now - LastAttemptedSwitch > 10 &&
		    monitorIsAlive) {
			fprintf(stderr, "Switching charger source from %s to %s\n", PowerSourceDescribe(CurrentPowerSource), PowerSourceDescribe(desiredSource));
			LastAttemptedSwitch = now;
			bool ok             = true;
			if (!EnableInverterStateChange) {
				fprintf(stderr, "EnableInverterStateChange is false, so not actually running command\n");
			} else {
				ok = Monitor->RunInverterCmd(PowerSourceToString(desiredSource));
			}
			if (ok) {
				if (CurrentPowerSource == PowerSource::SBU && desiredSource == PowerSource::SUB) {
					// Pause briefly before switching back to grid. The idea here is that if we're going to
					// switch our heavy loads from battery to grid, then we want to make sure that we're synchronized
					// with the grid. As far as I know, these inverters remain in sync, even if they're in SBU mode,
					// so this shouldn't really be necessary.
					fprintf(stderr, "Pausing for 100 ms, after switching back to grid\n");
					usleep(100 * 1000);
				}
				switchSuccess       = true;
				CurrentPowerSource  = desiredSource;
				SwitchPowerSourceAt = now;
			}
		}

		if (switchSuccess && wantSoftSwitch)
			LastSoftSwitch = now;

		if (switchSuccess && wantHardSwitch)
			LastHardSwitch = now;

		/*
		if (desiredSource != CurrentPowerSource && monitorIsAlive) {
			bool skip  = false;
			bool cmdOK = false;
			if (desiredSource == PowerSource::SBU && now - LastSwitchToSBU < MinSecondsBetweenSBUSwitches) {
				if (now - LastSwitchToSBU < 5) {
					// only emit the log message for the first 5 seconds
					fprintf(stderr, "Not switching to SBU, because too little time has elapsed since last switch (%d < %d)\n", (int) (now - LastSwitchToSBU), (int) MinSecondsBetweenSBUSwitches);
				}
				skip = true;
			} else if (!EnableInverterStateChange) {
				fprintf(stderr, "EnableInverterStateChange = false, so not actually changing inverter state\n");
				cmdOK = true;
			} else {
				fprintf(stderr, "Switching inverter from %s to %s\n", PowerSourceDescribe(CurrentPowerSource), PowerSourceDescribe(desiredSource));
				cmdOK = Monitor->RunInverterCmd(PowerSourceToString(desiredSource));
			}
			if (!skip) {
				if (cmdOK) {
					if (CurrentPowerSource == PowerSource::SBU && desiredSource == PowerSource::SUB) {
						// When switching from Battery to Utility, give a short pause to adjust to the grid phase, in case
						// we're also about to switch the heavy loads from Inverter back to Grid. I have NO IDEA
						// whether this is necessary, or if this pause is long enough, or whether the VM III even loses
						// phase lock with the grid when in SBU mode. From my measurements of ACinHz vs ACoutHz, I'm
						// guessing that the VM III does indeed keep it's phase locked to the grid, even when in SBU mode.
						// At 50hz, each cycle is 20ms.
						fprintf(stderr, "Pausing for 500 ms, after switching back to grid\n");
						usleep(100 * 1000);
					}
					if (desiredSource == PowerSource::SBU)
						LastSwitchToSBU = now;
					CurrentPowerSource          = desiredSource;
					Monitor->CurrentPowerSource = CurrentPowerSource;
				} else {
					fprintf(stderr, "Switching inverter mode failed\n");
				}
			}
		}
		*/

		if (desiredHeavyState != CurrentHeavyLoadState) {
			if (desiredHeavyState == HeavyLoadState::Grid || desiredHeavyState == HeavyLoadState::Off || HeavyCooloff.IsGood(now)) {
				if (desiredHeavyState != HeavyLoadState::Inverter)
					HeavyCooloff.SignalAlarm(now);
				SetHeavyLoadState(desiredHeavyState);
			}
		}

		if (desiredHeavyState == HeavyLoadState::Inverter)
			HeavyCooloff.SignalFine(now);

		int millisecond = 1000;
		usleep(100 * millisecond);
	}
}

TimePoint Controller::Now() {
	return TimePoint::Now(TimezoneOffsetMinutes);
}

} // namespace homepower