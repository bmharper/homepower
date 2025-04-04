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
	case HeavyLoadMode::Grid: return "Grid";
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
	MinCharge[0] = {TimePoint(8, 0), DefaultMinBatterySOC + 10, DefaultMinBatterySOC};
	MinCharge[1] = {TimePoint(16, 30), DefaultMaxBatterySOC, DefaultMaxBatterySOC};

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
		float minBatteryP    = Monitor->MinBatteryP;
		bool  hasGridPower   = Monitor->HasGridPower;
		float avgSolarW      = Monitor->AvgSolarW;
		float avgLoadW       = Monitor->AvgLoadW;
		float heavyLoadW     = Monitor->HeavyLoadWatts; // This is an estimate that is only updated when we switch heavy loads on and off.

		HeavyLoadLock.lock();
		auto heavyMode  = CurrentHeavyLoadMode;
		auto heavyState = CurrentHeavyLoadState;
		HeavyLoadLock.unlock();

		////////////////////////////////////////////////////////////////////////////////////////////////////////////
		// Compute our hard and soft battery SOC goals
		////////////////////////////////////////////////////////////////////////////////////////////////////////////
		float rawSoftBatteryGoal = TimePoint::Interpolate(nowP, NMinCharge, MinChargeTimePoints, MinChargeSoft);
		float rawHardBatteryGoal = TimePoint::Interpolate(nowP, NMinCharge, MinChargeTimePoints, MinChargeHard);
		float softBatteryGoal    = rawSoftBatteryGoal;
		float hardBatteryGoal    = rawHardBatteryGoal;
		softBatteryGoal          = Clamp(softBatteryGoal, 0.0f, 100.0f);
		hardBatteryGoal          = Clamp(hardBatteryGoal, 0.0f, 100.0f);

		bool isStormMode = now < StormModeUntil;
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
		if (now - LastSoftSwitch < 60 * 60)
			softBatteryGoal += 10.0f;

		if (now - LastHardSwitch < 60 * 60)
			hardBatteryGoal += 10.0f;

		// Why not 100? Because some batteries (Pylontech UP5000) often fail to report 100, but get "stuck" at 99.
		// UPDATE. Now they're only reporting 98. I'm starting to worry! This is not normal, but something wrong
		// with at least one of the batteries in that pair.
		if (minBatteryP >= 98.0f)
			LastEqualizeAt = now;

		time_t secondsSinceLastEqualize = now - LastEqualizeAt;

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
		////////////////////////////////////////////////////////////////////////////////////////////////////////////
		// Hard/soft battery SOC goals END
		////////////////////////////////////////////////////////////////////////////////////////////////////////////

		if (monitorIsAlive) {
			// Prevent hysteresis when our solar power is very similar to our loads, and we keep flip-flopping
			// our heavy loads between grid and inverter.
			// The idea here is: When we're in grid mode, make it harder to get out of it into inverter mode, by raising the bar.
			// HOWEVER! we have the additional problem that when the battery is 100% charged, then solar power will often
			// be just about 15% more than the loads. So we can't use a big factor here, otherwise the observed solar
			// power will never be high enough. So we push things down in the opposite direction, making loadFactor
			// 0.7 in the low mode, and 1.1 to escape out of that.
			// Also, biasing things slightly in favour of using battery more often happens to work well for me,
			// in the absence of a sophisticated solar irradiation + consumption prediction system.
			float loadFactor = 0.7;
			if (heavyState != HeavyLoadState::Inverter)
				loadFactor = 1.1f;

			// Compute an estimate of the total loads, including heavy loads.
			// If the heavy loads are currently on the inverter, then we don't need to guess, as the total is simply
			// the load watts that we observe. But if the heavy loads are switched off, or are on the grid, then we
			// must add in our estimate of the heavy load circuit to compute the total load.
			float estimatedTotalLoadW = avgLoadW;
			if (heavyState != HeavyLoadState::Inverter)
				estimatedTotalLoadW += heavyLoadW;

			bool solarExceedsLoads = avgSolarW > estimatedTotalLoadW * loadFactor;

			// This doesn't work. At night it just cycles between Inverter and Grid, quite often.
			// This is the end for me, for hackish optimization. Time to build a real predictor!
			//bool haveExcessBattery = batteryP > softBatteryGoal + 5.0f;

			// This is a grace factor added so that we can do things like run a washing machine in the morning,
			// even if the load exceeds the solar capacity for a while. The thing we're trying to exclude here
			// is the situation where it's getting late in the day, and we're just needlessly draining the battery,
			// only to have to charge it later in the evening. In these situations, we'd rather use grid power to
			// reduce the charging/discharging losses.
			// In my house, heavy loads in the afternoon are almost always the airconditioners.
			bool earlyInDayAndBatteryOK = nowP.Hour >= 7 && nowP.Hour <= 15 && batteryP >= 45.0f;

			//if (solarExceedsLoads || haveExcessBattery) {
			if (heavyMode == HeavyLoadMode::Grid) {
				// Always use grid power for heavy loads
				desiredHeavyState = HeavyLoadState::Grid;
			} else if (solarExceedsLoads) {
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

		if (monitorIsAlive && EnableAutoCharge) {
			// A key thing about the voltronic MKS inverters is that once the battery is charged, and they're in SUB mode,
			// then they no longer use the solar power for anything besides running the inverter itself (around 50W).
			// For this reason, we want to be in SBU mode as much of the time as possible, so that we never waste sunlight.

			if (monitorIsAlive && now - lastChargeMsg > 3 * 60) {
				lastChargeMsg = now;
				//fprintf(stderr, "Charge - Current: %s, ChargeStartedInHour: %d, goalBatteryP: %d, batteryP: %d, solarW: %.0f, loadW: %.0f, earlyInDayOK: %s, lateInDayOK: %s, endOfDayOK: %s, sinceEqualize: %.0f\n",
				//        PowerSourceDescribe(CurrentPowerSource), ChargeStartedInHour, goalBatteryP, batteryP, avgSolarW, avgLoadW, earlyInDayOK ? "yes" : "no", lateInDayOK ? "yes" : "no", endOfDayOK ? "yes" : "no", (float) secondsSinceLastEqualize);
				fprintf(stderr, "Mode: %s, SwitchPowerSourceAt: %d, SwitchChargerPriorityAt: %d, softBatteryGoal: %.1f (%.1f), hardBatteryGoal: %.1f (%.1f), batteryP: %.1f\n",
				        PowerSourceDescribe(CurrentPowerSource), int(now - SwitchPowerSourceAt), int(now - SwitchChargerPriorityAt),
				        softBatteryGoal, rawSoftBatteryGoal, hardBatteryGoal, rawHardBatteryGoal, batteryP);
				fprintf(stderr, "LastSoftSwitch: %d, LastHardSwitch: %d, LastAttemptedSourceSwitch: %d, LastAttemptedChargerSwitch: %d\n",
				        int(now - LastSoftSwitch), int(now - LastHardSwitch), int(now - LastAttemptedSourceSwitch), int(now - LastAttemptedChargerSwitch));
				fprintf(stderr, "Storm mode remaining: %d\n", int(StormModeUntil - now));
				fprintf(stderr, "solarW: %.0f, loadW: %.0f, sinceEqualize: %d, heavyLoadW: %.0f\n", avgSolarW, avgLoadW, (int) secondsSinceLastEqualize, heavyLoadW);
				fflush(stderr);
			}

			// Don't emit log messages in these 3 cases, because we may be busy with a cooloff period, if we've recently
			// made a change to any of these parameters. If we are busy with that cooloff, then we would emit these log
			// messages repeatedly until the cooloff period ends.
			if (batteryP < hardBatteryGoal) {
				// We've hit our hard limit. We must charge at all costs.
				desiredSource         = PowerSource::SUB;
				desiredChargePriority = ChargerPriority::UtilitySolar;
			} else if (batteryP < softBatteryGoal) {
				// We've hit our soft limit. Switch loads to grid, to avoid battery cycling.
				// It's more efficient to power loads directly from the grid, than using the
				// grid to charge the battery, and then powering loads from the battery. I don't
				// know what the full round-trip efficiency is, but voltronic inverter claim
				// 90% peak, and I don't know if that's round-trip or one way. I suspect the full
				// round trip is more like 80%, and that doesn't include battery wear and tear.
				desiredSource         = PowerSource::SUB;
				desiredChargePriority = ChargerPriority::SolarOnly;
			} else {
				// Our battery charge is good. We can switch to battery + solar only.
				desiredSource         = PowerSource::SBU;
				desiredChargePriority = ChargerPriority::SolarOnly;
			}

			// When we reach our soft or hard target, we remove the +10% bias by setting
			// LastHardSwitch / LastSoftSwitch = 0.
			// If we don't do this, then we end up ping-ponging between the two states,
			// as the +10% bias just ends up becoming the new permanent target.

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
					CurrentChargePriority   = desiredChargePriority;
					SwitchChargerPriorityAt = now;
					if (desiredChargePriority == ChargerPriority::UtilitySolar)
						LastHardSwitch = now;
					else
						LastHardSwitch = 0;
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
					CurrentPowerSource  = desiredSource;
					SwitchPowerSourceAt = now;
					if (desiredSource == PowerSource::SUB)
						LastSoftSwitch = now;
					else
						LastSoftSwitch = 0;
				} else {
					fprintf(stderr, "Switching power source failed\n");
				}
			}
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