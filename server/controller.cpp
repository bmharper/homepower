#include <time.h>
#include <unistd.h>
#include <wiringPi.h>
#include "controller.h"
#include "monitor.h"

using namespace std;

namespace homepower {

const char* ModeToString(HeavyLoadMode mode) {
	switch (mode) {
	case HeavyLoadMode::Off: return "Off";
	case HeavyLoadMode::Grid: return "Grid";
	case HeavyLoadMode::Inverter: return "Inverter";
	}
}

TimePoint TimePoint::Now(int timezoneOffsetMinutes) {
	auto t = time(nullptr);
	tm   tt;
	gmtime_r(&t, &tt);
	int sec = tt.tm_hour * 3600 + tt.tm_min * 60 + tt.tm_sec;
	sec += timezoneOffsetMinutes * 60;
	if (sec < 0)
		sec += 24 * 3600;
	sec = sec % (24 * 3600);
	TimePoint tp;
	tp.Hour   = sec / 3600;
	tp.Minute = (sec - tp.Hour * 3600) / 60;
	return tp;
}

Controller::Controller(homepower::Monitor* monitor) {
	// I don't know of any way to read the state of the GPIO output pins using WiringPI,
	// so in order to know our state at startup, we need to create it.
	// This seems like a conservative thing to do anyway.
	// I'm sure there is a way to read the state using other mechanisms, but I don't
	// have any need for that, because this server is intended to come on and stay
	// on for months, without a restart.
	Monitor  = monitor;
	MustExit = false;
	wiringPiSetup();
	pinMode(GpioPinGrid, OUTPUT);
	pinMode(GpioPinInverter, OUTPUT);
	digitalWrite(GpioPinGrid, 0);
	digitalWrite(GpioPinInverter, 0);
	CurrentHeavyLoadMode = HeavyLoadMode::Off;

	time_t    t  = time(NULL);
	struct tm lt = {0};
	localtime_r(&t, &lt);
	TimezoneOffsetMinutes = (int) (lt.tm_gmtoff / 60);
	fprintf(stderr, "Offset to GMT is %d minutes\n", TimezoneOffsetMinutes);

	auto now = Now();
	fprintf(stderr, "Time now (local): %d:%02d\n", now.Hour, now.Minute);
}

void Controller::Start() {
	MustExit = false;
	Thread   = thread([&]() {
        fprintf(stderr, "Controller started\n");
        fprintf(stderr, "EnablePowerSourceSwitch: %s\n", EnablePowerSourceSwitch ? "yes" : "no");
        if (EnablePowerSourceSwitch) {
            fprintf(stderr, "Switch to SUB at: %d:%02d\n", TimerSUB.Hour, TimerSUB.Minute);
            fprintf(stderr, "Switch to SBU at: %d:%02d\n", TimerSBU.Hour, TimerSBU.Minute);
        }
        Run();
        fprintf(stderr, "Controller exited\n");
    });
}

void Controller::Stop() {
	MustExit = true;
	Thread.join();
}

void Controller::SetHeavyLoadMode(HeavyLoadMode m, bool forceWrite) {
	if (CurrentHeavyLoadMode == m && !forceWrite)
		return;

	fprintf(stderr, "Set heavy load mode to %s\n", ModeToString(m));

	timespec pause;
	pause.tv_sec  = 0;
	pause.tv_nsec = SleepMilliseconds * 1000 * 1000;

	if (m == HeavyLoadMode::Inverter) {
		digitalWrite(GpioPinGrid, 0);
		nanosleep(&pause, nullptr);
		digitalWrite(GpioPinInverter, 1);
		Monitor->IsHeavyOnInverter = true;
	} else if (m == HeavyLoadMode::Grid) {
		digitalWrite(GpioPinInverter, 0);
		nanosleep(&pause, nullptr);
		digitalWrite(GpioPinGrid, 1);
		Monitor->IsHeavyOnInverter = false;
	} else if (m == HeavyLoadMode::Off) {
		digitalWrite(GpioPinInverter, 0);
		digitalWrite(GpioPinGrid, 0);
		Monitor->IsHeavyOnInverter = false;
	}

	CurrentHeavyLoadMode = m;
}

void Controller::Run() {
	auto lastStatus   = 0;
	auto lastPVStatus = 0;
	while (!MustExit) {
		time_t        now           = time(nullptr);
		auto          nowP          = Now();
		HeavyLoadMode desiredPMode  = HeavyLoadMode::Grid;
		PowerSource   desiredSource = PowerSource::SUB;
		int           solarDeficitW = Monitor->SolarDeficitW;

		float batteryV             = Monitor->BatteryV;
		int   maxLoadW             = Monitor->MaxLoadW;
		int   avgLoadW             = Monitor->AvgLoadW;
		bool  monitorIsAlive       = Monitor->IsInitialized;
		bool  isSolarTime          = nowP > SolarOnAt && nowP < SolarOffAt;
		int   solarV               = Monitor->AvgSolarV;
		bool  hasGridPower         = Monitor->HasGridPower;
		bool  solarIsPoweringLoads = solarDeficitW < MaxSolarDeficit_HeavyLoads;
		bool  solarPowerGoodForSBU = solarDeficitW < MaxSolarDeficit_SBU;
		bool  haveSolarHeavyV      = solarV > MinSolarHeavyV;
		bool  haveBatterySolarV    = solarV > MinSolarBatterySourceV;
		bool  loadIsLow            = avgLoadW < MaxLoadBatteryModeW;
		//bool pvTooWeak         = Monitor->PVIsTooWeakForLoads;
		bool batteryGoodForSBU = batteryV >= MinBatteryV_SBU;
		if (monitorIsAlive && !Monitor->IsOverloaded && isSolarTime && haveSolarHeavyV && (hasGridPower || solarIsPoweringLoads)) {
			desiredPMode = HeavyLoadMode::Inverter;
		} else {
			if (!hasGridPower) {
				// When the grid is off, and we don't have enough solar power, we switch all non-essential devices off.
				// This prevents them from being subject to a spike when the grid is switched back on again.
				// We assume that this grid spike only lasts a few milliseconds, and by the time we've detected
				// that the grid is back on, the spike has subsided. In other words, we make no attempt to add
				// an extra delay before switching the grid back on.
				desiredPMode = HeavyLoadMode::Off;
			}

			if (monitorIsAlive && time(nullptr) - lastStatus > 10 * 60) {
				lastStatus = time(nullptr);
				fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveSolarHeavyV(%d): %s, SolarDeficitW: %d (max %d), IsOverloaded: %s (time %d:%02d)\n",
				        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveSolarHeavyV ? "yes" : "no",
				        solarDeficitW, MaxSolarDeficit_HeavyLoads,
				        Monitor->IsOverloaded ? "yes" : "no",
				        nowP.Hour, nowP.Minute);
				fflush(stderr);
			}
		}

		// New logic, since I have 4.5 kWh of LiFePO batteries (Pylontech UP5000).
		// In this case, we want to switch to grid power between sundown and *some night time*. Then, we're on battery mode all through that night,
		// and all through the next day, until sundown comes again.
		// Note that unlike all the rest of our logic, which is stateless... this logic is stateful.
		// We only emit a decision ON THE MINUTE when the switch it supposed to happen.
		// The reason I do it like this, is so that I can walk to my inverter and manually change it
		// for whatever reason (eg I am gaming late at night, or I know that it's going to rain the next
		// morning, and/or there's lots of load shedding).
		bool enableSwitch = false;
		if (EnablePowerSourceSwitch) {
			if (nowP == TimerSUB) {
				enableSwitch  = true;
				desiredSource = PowerSource::SUB;
			} else if (nowP == TimerSBU) {
				enableSwitch  = true;
				desiredSource = PowerSource::SBU;
			}
		}

		/*
		// Old logic that tried to minimize battery usage (when I had AGM batteries)
		if (isSolarTime && hasGridPower && haveBatterySolarV && loadIsLow && batteryGoodForSBU && solarPowerGoodForSBU) {
			desiredSource = PowerSource::SBU;
		} else {
			if (time(nullptr) - lastPVStatus > 60) {
				lastPVStatus = time(nullptr);
				//fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveBatterySolarV(%d): %s, pvTooWeak: %s, batteryGoodForSBU: %s\n",
				//        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveBatterySolarV ? "yes" : "no", pvTooWeak ? "yes" : "no", batteryGoodForSBU ? "yes" : "no");
				fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveBatterySolarV(%d): %s, loadIsLow(%d): %s, solarDeficitW: %d, batteryGoodForSBU(%.2f): %s\n",
				        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveBatterySolarV ? "yes" : "no", avgLoadW,
				        loadIsLow ? "yes" : "no",
				        solarDeficitW,
				        batteryV, batteryGoodForSBU ? "yes" : "no");
			}
		}
		*/

		// NOTE: I am disabling SourceCooloff, now that I have a 4.8 kwh lithium ion battery.
		// With the lithium ion battery, I only switch between SUB and SBU on a timer.
		// I plan on revisiting this decision... but need a more robust decision mechanism.

		if (enableSwitch &&
		    desiredSource != CurrentPowerSource &&
		    monitorIsAlive) {
			//(SourceCooloff.CanSwitch(now) || desiredSource == PowerSource::SUB)) { // only applicable to old AGM logic
			fprintf(stderr, "Switching inverter from %s to %s\n", PowerSourceDescribe(CurrentPowerSource), PowerSourceDescribe(desiredSource));
			if (Monitor->RunInverterCmd(string("POP") + PowerSourceToString(desiredSource))) {
				if (CurrentPowerSource == PowerSource::SBU && desiredSource == PowerSource::SUB) {
					// When switching from Battery to Utility, give 1 second to adjust to the grid phase, in case
					// we're also about to switch the heavy loads from Inverter back to Grid. I have NO IDEA
					// whether this is necessary, or if 1 second is long enough, or whether the VM III even loses
					// phase lock with the grid when in SBU mode. From my measurements of ACinHz vs ACoutHz, I'm
					// guessing that the VM III does indeed keep it's phase close to the grid, even when in SBU mode.
					fprintf(stderr, "Pausing for 1 second, after switching back to grid\n");
					sleep(1);
				}
				//SourceCooloff.Switching(now, desiredSource == PowerSource::SBU);
				CurrentPowerSource          = desiredSource;
				Monitor->CurrentPowerSource = CurrentPowerSource;
			} else {
				fprintf(stderr, "Switching inverter mode failed\n");
			}
		}

		//SourceCooloff.Notify(now, desiredSource == PowerSource::SBU);

		if (desiredPMode != CurrentHeavyLoadMode) {
			if (desiredPMode == HeavyLoadMode::Grid || desiredPMode == HeavyLoadMode::Off || HeavyCooloff.CanSwitch(now)) {
				HeavyCooloff.Switching(now, desiredPMode == HeavyLoadMode::Inverter);
				SetHeavyLoadMode(desiredPMode);
			}
		}

		HeavyCooloff.Notify(now, desiredPMode == HeavyLoadMode::Inverter);

		int millisecond = 1000;
		usleep(100 * millisecond);
	}
}

TimePoint Controller::Now() {
	return TimePoint::Now(TimezoneOffsetMinutes);
}

} // namespace homepower