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
	printf("Offset to GMT is %d minutes\n", TimezoneOffsetMinutes);

	auto now = Now();
	printf("Time now (local): %d:%02d\n", now.Hour, now.Minute);
}

void Controller::Start() {
	MustExit = false;
	Thread   = thread([&]() {
        printf("Controller started\n");
        Run();
        printf("Controller exited\n");
    });
}

void Controller::Stop() {
	MustExit = true;
	Thread.join();
}

void Controller::SetHeavyLoadMode(HeavyLoadMode m, bool forceWrite) {
	if (CurrentHeavyLoadMode == m && !forceWrite)
		return;

	printf("Set mode to %s\n", ModeToString(m));

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
		PowerSource   desiredSource = PowerSource::SolarUtilityBattery;

		bool monitorIsAlive    = Monitor->IsInitialized;
		bool isSolarTime       = nowP > SolarOnAt && nowP < SolarOffAt;
		int  solarV            = Monitor->AvgSolarV;
		bool hasGridPower      = Monitor->HasGridPower;
		bool haveSolarHeavyV   = solarV > MinSolarHeavyV;
		bool haveBatterySolarV = solarV > MinSolarBatterySourceV;
		bool loadIsLow         = Monitor->MaxLoadW < MaxLoadBatteryModeW;
		bool pvTooWeak         = Monitor->PVIsTooWeakForLoads;
		bool batteryGoodForSBU = Monitor->BatteryV >= MinBatteryV_SBU;
		if (monitorIsAlive && !Monitor->IsOverloaded && isSolarTime && haveSolarHeavyV && hasGridPower) {
			desiredPMode = HeavyLoadMode::Inverter;
		} else {
			if (monitorIsAlive && time(nullptr) - lastStatus > 10 * 60) {
				lastStatus = time(nullptr);
				fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveSolarHeavyV(%d): %s, IsOverloaded: %s (time %d:%02d)\n",
				        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveSolarHeavyV ? "yes" : "no",
				        Monitor->IsOverloaded ? "yes" : "no",
				        nowP.Hour, nowP.Minute);
				fflush(stderr);
			}
		}

		if (isSolarTime && hasGridPower && haveBatterySolarV && loadIsLow && !pvTooWeak && batteryGoodForSBU) {
			desiredSource = PowerSource::SolarBatteryUtility;
		} else {
			if (time(nullptr) - lastPVStatus > 10 * 60) {
				lastPVStatus = time(nullptr);
				fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveBatterySolarV(%d): %s, pvTooWeak: %s, batteryGoodForSBU: %s\n",
				        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveBatterySolarV ? "yes" : "no", pvTooWeak ? "yes" : "no", batteryGoodForSBU ? "yes" : "no");
			}
		}

		if (desiredSource != CurrentPowerSource && (now - LastSourceSwitch > CooloffSeconds || desiredSource == PowerSource::SolarUtilityBattery)) {
			fprintf(stderr, "Switching inverter from %s to %s\n", PowerSourceDescribe(CurrentPowerSource), PowerSourceDescribe(desiredSource));
			if (Monitor->RunInverterCmd(string("POP") + PowerSourceToString(desiredSource))) {
				if (CurrentPowerSource == PowerSource::SolarBatteryUtility && desiredSource == PowerSource::SolarUtilityBattery) {
					// When switching from Battery to Utility, give 1 second to adjust to the grid phase, in case
					// we're also about to switch the heavy loads from Inverter back to Grid. I have NO IDEA
					// whether this is necessary, or if 1 second is long enough, or whether the VM III even loses
					// phase lock with the grid when in SBU mode. From my measurements of ACinHz vs ACoutHz, I'm
					// guesing that the VM III does indeed keep it's phase close to the grid, even when in SBU mode.
					fprintf(stderr, "Pausing for 1 second, after switching back to grid\n");
					sleep(1);
				}
				CurrentPowerSource          = desiredSource;
				Monitor->CurrentPowerSource = CurrentPowerSource;
				LastSourceSwitch            = now;
			} else {
				fprintf(stderr, "Switching inverter mode failed\n");
			}
		}

		if (desiredPMode != CurrentHeavyLoadMode) {
			if (desiredPMode == HeavyLoadMode::Grid || now - LastHeavySwitch > CooloffSeconds) {
				SetHeavyLoadMode(desiredPMode);
				LastHeavySwitch = now;
			}
		}

		int millisecond = 1000;
		usleep(100 * millisecond);
	}
}

TimePoint Controller::Now() {
	return TimePoint::Now(TimezoneOffsetMinutes);
}

} // namespace homepower