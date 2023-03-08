#include <time.h>
#include <unistd.h>
#include "../bcm2835/bcm2835.h"
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

Controller::Controller(homepower::Monitor* monitor, bool enableGpio) {
	// I don't know of any way to read the state of the GPIO output pins using WiringPI,
	// so in order to know our state at startup, we need to create it.
	// This seems like a conservative thing to do anyway.
	// I'm sure there is a way to read the state using other mechanisms, but I don't
	// have any need for that, because this server is intended to come on and stay
	// on for months, without a restart.
	Monitor                 = monitor;
	MustExit                = false;
	KeepHeavyOnWithoutSolar = false;
	EnableGpio              = enableGpio;
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
	CurrentHeavyLoadMode = HeavyLoadMode::Off;
	ChangePowerSourceMsg = (int) PowerSource::Unknown;

	time_t    t  = time(NULL);
	struct tm lt = {0};
	localtime_r(&t, &lt);
	TimezoneOffsetMinutes = (int) (lt.tm_gmtoff / 60);
	fprintf(stderr, "Offset to GMT is %d minutes\n", TimezoneOffsetMinutes);

	auto now = Now();
	fprintf(stderr, "Time now (local): %d:%02d\n", now.Hour, now.Minute);
}

Controller::~Controller() {
	if (EnableGpio) {
		bcm2835_close();
	}
}

void Controller::Start() {
	MustExit = false;
	Thread   = thread([&]() {
        fprintf(stderr, "Controller started\n");
        fprintf(stderr, "EnablePowerSourceSwitch: %s\n", EnablePowerSourceSwitch ? "yes" : "no");
        if (EnablePowerSourceSwitch && EnablePowerSourceTimer) {
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
	lock_guard<mutex> lock(HeavyLoadLock);

	if (CurrentHeavyLoadMode == m && !forceWrite)
		return;

	fprintf(stderr, "Set heavy load mode to %s\n", ModeToString(m));

	// Ideally, we'd use a switchover device that can do zero crossing,
	// which means the switch waits until the AC signal crosses over 0 voltage.
	// Since we can't control that, what we do is impose an extra delay
	// in between switching the old one off, and switching the new one on.
	// Because our contactors are physical devices with delays in them,
	// we don't need to add much extra delay to be safe.
	timespec pause;
	pause.tv_sec  = 0;
	pause.tv_nsec = SwitchSleepMilliseconds * 1000 * 1000;

	if (m == HeavyLoadMode::Inverter) {
		if (EnableGpio) {
			bcm2835_gpio_clr(GpioPinGrid);
		}
		nanosleep(&pause, nullptr);
		if (EnableGpio) {
			bcm2835_gpio_set(GpioPinInverter);
		}
		Monitor->IsHeavyOnInverter = true;
	} else if (m == HeavyLoadMode::Grid) {
		if (EnableGpio) {
			bcm2835_gpio_clr(GpioPinInverter);
		}
		nanosleep(&pause, nullptr);
		if (EnableGpio) {
			bcm2835_gpio_set(GpioPinGrid);
		}
		Monitor->IsHeavyOnInverter = false;
	} else if (m == HeavyLoadMode::Off) {
		if (EnableGpio) {
			bcm2835_gpio_clr(GpioPinInverter);
			bcm2835_gpio_clr(GpioPinGrid);
		}
		Monitor->IsHeavyOnInverter = false;
	}

	CurrentHeavyLoadMode = m;
}

void Controller::ChangePowerSource(PowerSource source) {
	ChangePowerSourceMsg = (int) source;
}

void Controller::Run() {
	auto lastStatus   = 0;
	auto lastPVStatus = 0;
	while (!MustExit) {
		time_t        now              = time(nullptr);
		auto          nowP             = Now();
		HeavyLoadMode desiredHeavyMode = HeavyLoadMode::Grid;
		PowerSource   desiredSource    = PowerSource::SUB;

		bool monitorIsAlive  = Monitor->IsInitialized;
		bool isSolarTime     = nowP > SolarOnAt && nowP < SolarOffAt;
		int  solarV          = Monitor->AvgSolarV;
		int  batteryP        = (int) Monitor->BatteryP;
		bool hasGridPower    = Monitor->HasGridPower;
		bool haveSolarHeavyV = solarV > MinSolarHeavyV;

		if (monitorIsAlive && !Monitor->IsBatteryOverloaded && !Monitor->IsOutputOverloaded && KeepHeavyOnWithoutSolar.load()) {
			desiredHeavyMode = HeavyLoadMode::Inverter;
		}

		if (monitorIsAlive && !Monitor->IsBatteryOverloaded && !Monitor->IsOutputOverloaded && isSolarTime && haveSolarHeavyV) {
			desiredHeavyMode = HeavyLoadMode::Inverter;

			if (now - ChargeStartedAt < ChargeMinutes * 60) {
				// we're still busy charging
				desiredHeavyMode = HeavyLoadMode::Grid;
			} else if (batteryP <= MinBatteryChargePercent) {
				// start charging
				ChargeStartedAt  = now;
				desiredHeavyMode = HeavyLoadMode::Grid;
				fprintf(stderr, "Battery is low, switch off heavy loads\n");
			}
		}

		if (desiredHeavyMode != HeavyLoadMode::Inverter) {
			if (!hasGridPower) {
				// When the grid is off, and we don't have enough solar power, we switch all non-essential devices off.
				// This prevents them from being subject to a spike when the grid is switched back on again.
				// We assume that this grid spike only lasts a few milliseconds, and by the time we've detected
				// that the grid is back on, the spike has subsided. In other words, we make no attempt to add
				// an extra delay before switching the grid back on.
				desiredHeavyMode = HeavyLoadMode::Off;
			}

			if (monitorIsAlive && now - lastStatus > 10 * 60) {
				lastStatus = now;
				fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveSolarHeavyV(%d): %s, OutputOverloaded: %s, BatteryOverloaded: %s (time %d:%02d)\n",
				        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveSolarHeavyV ? "yes" : "no",
				        Monitor->IsOutputOverloaded ? "yes" : "no",
				        Monitor->IsBatteryOverloaded ? "yes" : "no",
				        nowP.Hour, nowP.Minute);
				fflush(stderr);
			}
		}

		// New logic, since I have 4.5 kWh of LiFePO batteries (Pylontech UP5000).
		// In this case, we want to switch to grid power between sundown and *some night time*. Then, we're on battery mode all through that night,
		// and all through the next day, until sundown comes again.
		// Note that unlike all the rest of our logic, which is stateless... this logic is stateful.
		// We only emit a decision DURING THE MINUTE when the switch it supposed to happen.
		// The reason I do it like this, is so that I can walk to my inverter and manually change it
		// for whatever reason (eg I am gaming late at night, or I know that it's going to rain the next
		// morning, and/or there's lots of load shedding).
		desiredSource = CurrentPowerSource;

		if (EnablePowerSourceSwitch) {
			// Check if we have a 'please change to X mode' request from our HTTP server
			// This is sloppy use of an atomic variable, but our needs are simple.
			auto request = (PowerSource) ChangePowerSourceMsg.load();

			// Note that these 'minute precision' checks will execute repeatedly for an entire minute
			if (nowP.EqualsMinutePrecision(TimerSUB) && EnablePowerSourceTimer) {
				desiredSource = PowerSource::SUB;
			} else if (nowP.EqualsMinutePrecision(TimerSBU) && EnablePowerSourceTimer) {
				desiredSource = PowerSource::SBU;
			} else if (request != PowerSource::Unknown) {
				desiredSource        = request;
				ChangePowerSourceMsg = (int) PowerSource::Unknown; // signal that we've made the desired change
			} else {
				// If battery is too low, then switch back to SUB
				if (CurrentPowerSource == PowerSource::SBU && batteryP <= MinBatteryChargePercent) {
					ChargeStartedAt = now;
					desiredSource   = PowerSource::SUB;
					fprintf(stderr, "Battery is low, switch to SUB\n");
				}
			}
		}

		// NOTE: I am disabling SourceCooloff, now that I have a 4.8 kwh lithium ion battery.
		// With the lithium ion battery, I only switch between SUB and SBU on a timer.
		// I plan on revisiting this decision... but need a more robust decision mechanism.

		if (desiredSource != CurrentPowerSource && monitorIsAlive) {
			//(SourceCooloff.IsGood(now) || desiredSource == PowerSource::SUB)) { // only applicable to old AGM logic
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
				if (desiredSource != PowerSource::SBU)
					SourceCooloff.SignalAlarm(now);
				CurrentPowerSource          = desiredSource;
				Monitor->CurrentPowerSource = CurrentPowerSource;
			} else {
				fprintf(stderr, "Switching inverter mode failed\n");
			}
		}

		if (desiredSource == PowerSource::SBU)
			SourceCooloff.SignalFine(now);

		if (desiredHeavyMode != CurrentHeavyLoadMode) {
			if (desiredHeavyMode == HeavyLoadMode::Grid || desiredHeavyMode == HeavyLoadMode::Off || HeavyCooloff.IsGood(now)) {
				if (desiredHeavyMode != HeavyLoadMode::Inverter)
					HeavyCooloff.SignalAlarm(now);
				SetHeavyLoadMode(desiredHeavyMode);
			}
		}

		if (desiredHeavyMode == HeavyLoadMode::Inverter)
			HeavyCooloff.SignalFine(now);

		int millisecond = 1000;
		usleep(100 * millisecond);
	}
}

TimePoint Controller::Now() {
	return TimePoint::Now(TimezoneOffsetMinutes);
}

} // namespace homepower