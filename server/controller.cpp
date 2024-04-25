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
	StormModeUntil            = 0;
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

	// Some principles:
	// You want your max charge point to be 90 or less, so that the +10 that we add as a buffer
	// when charging, does not exceed 100. The reason its bad if you exceed 100, is that you
	// can then be in a situation where the battery is full, but you're in SUB mode, so you're
	// just throwing away all your PV energy. So long as your battery goal remains above 100,
	// you'll remain in this state. So we rather make the late afternoon battery goal 90.
	// If we kick into charge mode, we will at least exit it once we hit 100.
	//
	// Our equalizer acts as a stopgap for this wonky logic, by ensuring that we charge to
	// 100% after 5pm. Ideally we wouldn't need that, but I haven't thought of a way yet
	// to avoid ping-ponging late in the afternoon without this technique.
	MinCharge[0] = {TimePoint(8, 0), 45, 35};
	MinCharge[1] = {TimePoint(16, 30), 90, 90};

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
		if (MinCharge[i].Soft < MinCharge[i].Hard) {
			// I added this check when I wrote the "if" statement where we check if we're below
			// our hard limit, and inside there, we say "wantSoft = true", because our assumption
			// is that if we've hit our hard limit, then we've also hit our soft limit. So I just
			// don't want to violate that assumption, even if the results are fairly benign.
			fprintf(stderr, "MinCharge soft limit must be greater than or equal to hard limit");
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

void Controller::Run() {
	auto lastStatus    = 0;
	auto lastChargeMsg = 0;
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
			fprintf(stderr, "hasGridPower: %s, avgSolarV: %.1f, OutputOverloaded: %s, BatteryOverloaded: %s, time: %d:%02d\n",
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
			float softBatteryGoal    = TimePoint::Interpolate(nowP, NMinCharge, MinChargeTimePoints, MinChargeSoft);
			float hardBatteryGoal    = TimePoint::Interpolate(nowP, NMinCharge, MinChargeTimePoints, MinChargeHard);
			float rawSoftBatteryGoal = softBatteryGoal;
			float rawHardBatteryGoal = hardBatteryGoal;
			softBatteryGoal          = Clamp(softBatteryGoal, 0.0f, 100.0f);
			hardBatteryGoal          = Clamp(hardBatteryGoal, 0.0f, 100.0f);

			bool isStormMode = StormModeUntil > now;
			if (isStormMode) {
				softBatteryGoal = max(softBatteryGoal, 90.0f);
				hardBatteryGoal = max(hardBatteryGoal, 80.0f);
			}

			// If we hit either of our thresholds within the last hour, then raise the target
			// to ensure that we overshoot it by some margin. Otherwise we ping pong along the bottom.
			// My initial instinct here was to clamp the goals to 100%. That turns out to be a bad
			// decision, because it means that when you're close to 100%, you start to ping pong between
			// "must charge" and "must not charge". There's actually nothing wrong with having a 105%
			// SOC goal, because it means that you'll hang out at 100% for a while, and by the time
			// you switch back to battery, the goal will be 95%, so you've got some headroom.
			if (now - LastSoftSwitch < 60 * 60) {
				softBatteryGoal += 10.0f;
			}
			if (now - LastHardSwitch < 60 * 60) {
				hardBatteryGoal += 10.0f;
			}

			if (avgBatteryP >= 100.0f)
				LastEqualizeAt = now;

			time_t secondsSinceLastEqualize = now - LastEqualizeAt;
			//bool   needEqualizeSoft         = secondsSinceLastEqualize >= HoursBetweenEqualize * 3600;     // 1x maximum period has passed
			//bool   needEqualizeHard         = secondsSinceLastEqualize >= HoursBetweenEqualize * 3600 * 2; // 2x maximum period has passed
			//bool isEqualizeTime = nowP.AbsoluteMinute	()

			if (nowP.Hour >= 17 && secondsSinceLastEqualize >= HoursBetweenEqualize * 3600) {
				// Ensure that we give the battery a chance to balance the cells, regardless of the SOC hourly goal.
				// Note that it is vital that we alter goalBatteryP up here, before any of the other decisions are made,
				// otherwise we can end up in a situation where we switch to SUB mode, and then immediately switch back to SBU,
				// because both sides of our "let's charge" and "we have enough" will be true at the same time.
				// It was due to this original bug that we added the MinSecondsBetweenSBUSwitches protection logic, to
				// prevent flipping between states too frequently. The inverter DID NOT enjoy this, and kept restarting itself.
				// We somewhat arbitrarily choose to do equalization after 5pm, because that happens to coincide with the time
				// when we expect the battery SOC to be close to 100%
				// Ensure that we stay in charging mode until we're equalized.
				// The SOC can never be 200, so this will force SUB and Grid Charge modes.
				softBatteryGoal = max(softBatteryGoal, 200.0f);
				hardBatteryGoal = max(hardBatteryGoal, 200.0f);
			}

			// A key thing about the voltronic MKS inverters is that once the battery is charged, and they're in SUB mode,
			// then they no longer use the solar power for anything besides running the inverter itself (around 50W).
			// For this reason, we want to be in SBU mode as much of the time as possible, so that we never waste sunlight.

			if (monitorIsAlive && now - lastChargeMsg > 10 * 60) {
				lastChargeMsg = now;
				//fprintf(stderr, "Charge - Current: %s, ChargeStartedInHour: %d, goalBatteryP: %d, batteryP: %d, solarW: %.0f, loadW: %.0f, earlyInDayOK: %s, lateInDayOK: %s, endOfDayOK: %s, sinceEqualize: %.0f\n",
				//        PowerSourceDescribe(CurrentPowerSource), ChargeStartedInHour, goalBatteryP, batteryP, avgSolarW, avgLoadW, earlyInDayOK ? "yes" : "no", lateInDayOK ? "yes" : "no", endOfDayOK ? "yes" : "no", (float) secondsSinceLastEqualize);
				fprintf(stderr, "Mode: %s, SwitchPowerSourceAt: %d, SwitchChargerPriorityAt: %d, softBatteryGoal: %.1f (%.1f), hardBatteryGoal: %.1f (%.1f), batteryP: %.1f\n",
				        PowerSourceDescribe(CurrentPowerSource), int(now - SwitchPowerSourceAt), int(now - SwitchChargerPriorityAt),
				        softBatteryGoal, rawSoftBatteryGoal, hardBatteryGoal, rawHardBatteryGoal, batteryP);
				fprintf(stderr, "LastSoftSwitch: %d, LastHardSwitch: %d, LastAttemptedSourceSwitch: %d, LastAttemptedChargerSwitch: %d\n",
				        int(now - LastSoftSwitch), int(now - LastHardSwitch), int(now - LastAttemptedSourceSwitch), int(now - LastAttemptedChargerSwitch));
				fprintf(stderr, "Storm mode remaining: %d\n", int(StormModeUntil - now));
				fprintf(stderr, "solarW: %.0f, loadW: %.0f, sinceEqualize: %d\n", avgSolarW, avgLoadW, (int) secondsSinceLastEqualize);
				fflush(stderr);
			}

			// Don't emit log messages in these 3 cases, because we may be busy with a cooloff period, if we've recently
			// made a change to any of these parameters. If we are busy with that cooloff, then we would emit these log
			// messages repeatedly until the cooloff period ends.
			if (batteryP < hardBatteryGoal) {
				// We've hit our hard limit. We must charge at all costs.
				desiredSource         = PowerSource::SUB;
				desiredChargePriority = ChargerPriority::UtilitySolar;
				wantHardSwitch        = true;
				wantSoftSwitch        = true; // Being below our hard threshold implies that we're also below our soft threshold
			} else if (batteryP < softBatteryGoal) {
				// We've hit our soft limit. Switch loads to grid, to avoid battery cycling.
				// It's more efficient to power loads directly from the grid, then using the
				// grid to charge the battery, and then powering loads from the battery. I don't
				// know what the full round-trip efficiency is, but voltronic inverter claim
				// 90% peak, and I don't know if that's round-trip or one way. I suspect the full
				// round trip is more like 80%, and that doesn't include battery wear and tear.
				desiredSource         = PowerSource::SUB;
				desiredChargePriority = ChargerPriority::SolarOnly;
				wantSoftSwitch        = true;
			} else {
				// Our battery charge is good. We can switch to battery + solar only.
				desiredSource         = PowerSource::SBU;
				desiredChargePriority = ChargerPriority::SolarOnly;
			}

			bool switchSuccess = false;

			if (desiredChargePriority != CurrentChargePriority &&
			    now - SwitchChargerPriorityAt > 5 * 60 &&
			    now - LastAttemptedChargerSwitch > 10 &&
			    monitorIsAlive) {
				fprintf(stderr, "battery: %.1f, soft goal: %.1f, hard goal: %.1f\n", batteryP, softBatteryGoal, hardBatteryGoal);
				fprintf(stderr, "Switching charger priority from %s to %s\n", ChargerPriorityToString(CurrentChargePriority), ChargerPriorityToString(desiredChargePriority));
				LastAttemptedChargerSwitch = now;
				bool ok                    = true;
				if (!EnableInverterStateChange) {
					fprintf(stderr, "EnableInverterStateChange is false, so not actually running command\n");
				} else {
					ok = Monitor->RunInverterCmd(ChargerPriorityToString(desiredChargePriority));
				}
				if (ok) {
					switchSuccess           = true;
					CurrentChargePriority   = desiredChargePriority;
					SwitchChargerPriorityAt = now;
				} else {
					fprintf(stderr, "Switching charger priority failed\n");
				}
			}

			if (desiredSource != CurrentPowerSource &&
			    now - SwitchPowerSourceAt > 5 * 60 &&
			    now - LastAttemptedSourceSwitch > 10 &&
			    monitorIsAlive) {
				fprintf(stderr, "battery: %.1f, soft goal: %.1f, hard goal: %.1f\n", batteryP, softBatteryGoal, hardBatteryGoal);
				fprintf(stderr, "Switching power source from %s to %s\n", PowerSourceDescribe(CurrentPowerSource), PowerSourceDescribe(desiredSource));
				LastAttemptedSourceSwitch = now;
				bool ok                   = true;
				if (!EnableInverterStateChange) {
					fprintf(stderr, "EnableInverterStateChange is false, so not actually running command\n");
				} else {
					ok = Monitor->RunInverterCmd(PowerSourceToString(desiredSource));
				}
				if (ok) {
					switchSuccess       = true;
					CurrentPowerSource  = desiredSource;
					SwitchPowerSourceAt = now;
				} else {
					fprintf(stderr, "Switching power source failed\n");
				}
			}

			if (switchSuccess && wantSoftSwitch)
				LastSoftSwitch = now;

			if (switchSuccess && wantHardSwitch)
				LastHardSwitch = now;

		} // if (monitorIsAlive && EnableAutoCharge)

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

void Controller::SetStormMode(int hours) {
	if (hours <= 0) {
		StormModeUntil = 0;
		return;
	}
	time_t now     = time(nullptr);
	StormModeUntil = now + hours * 3600;
}

} // namespace homepower