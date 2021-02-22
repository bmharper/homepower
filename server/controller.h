#pragma once

#include <thread>
#include <atomic>
#include <time.h>

#include "commands.h"

namespace homepower {

class Monitor;

enum class HeavyLoadMode {
	Off,
	Grid,
	Inverter,
};

const char* ModeToString(HeavyLoadMode mode);

struct TimePoint {
	int Hour   = 0;
	int Minute = 0;
	TimePoint(int hour = 0, int min = 0) : Hour(hour), Minute(min) {}
	bool operator<(const TimePoint& b) const {
		return Hour < b.Hour || (Hour == b.Hour && Minute < b.Minute);
	}
	bool operator>(const TimePoint& b) const {
		return Hour > b.Hour || (Hour == b.Hour && Minute > b.Minute);
	}
	static TimePoint Now(int timezoneOffsetMinutes);
};

// Cooloff represents a time period that doubles every time we make
// an incorrect decision.
struct Cooloff {
	time_t Default    = 60;      // When we think these are stable, then cooloff period returns to Default
	time_t Current    = 60;      // Current backoff time
	time_t Max        = 60 * 15; // Maximum backoff time
	time_t LastSwitch = 0;       // Last time that we switched, or wanted to be in the undesired state

	// Call notify at every time period, to inform Cooloff of the current state
	void Notify(time_t now, bool wantDesiredState) {
		if (wantDesiredState && now - LastSwitch > Current * 2 && Current != Default) {
			// since we want to be in desiredState, and our last switch was more than Current*2 ago,
			// we know that we've been in the desiredState for long enough to reset our backoff period.
			Current = Default;
		} else if (!wantDesiredState) {
			// keep walking LastSwitch up, because we've just observed that the system is overloaded
			LastSwitch = now;
		}
	}
	// CanSwitch returns true if we can switch to our desired state
	bool CanSwitch(time_t now) const {
		return now - LastSwitch > Current;
	}
	// Call switching every time you change state
	void Switching(time_t now, bool toDesiredState) {
		if (!toDesiredState)
			Current = std::min(Current * 2, Max);
		LastSwitch = now;
	}
};

class Controller {
public:
	int       GpioPinGrid                = 0;
	int       GpioPinInverter            = 1;
	int       SleepMilliseconds          = 20;               // 50hz = 20ms cycle time. Hager ESC225 have 25ms switch off time, and 10ms switch on time.
	int       TimezoneOffsetMinutes      = 120;              // 120 = UTC+2 (Overridden by constructor)
	int       MinSolarHeavyV             = 160;              // Minimum solar voltage before we'll put heavy loads on it
	int       MinSolarBatterySourceV     = 190;              // Minimum solar voltage before we'll place the system in SBU mode. Poor proxy for actual PvW output capability.
	int       MaxLoadBatteryModeW        = 1500;             // Maximum load for "SBU" mode
	float     MinBatteryV_SBU            = 26.0f;            // Minimum battery voltage for "SBU" mode
	int       MaxSolarDeficit_HeavyLoads = 200;              // Switch off heavy loads if our load demand is 200 watts more than our PV supply
	int       MaxSolarDeficit_SBU        = 200;              // Switch from SBU to SUB if solar is not keeping up with demand
	TimePoint SolarOnAt                  = TimePoint(7, 0);  // Ignore any solar voltage before this time
	TimePoint SolarOffAt                 = TimePoint(18, 0); // Ignore any solar voltage after this time

	Controller(homepower::Monitor* monitor);
	void Start();
	void Stop();
	void SetHeavyLoadMode(HeavyLoadMode m, bool forceWrite = false);

private:
	std::thread       Thread;
	std::atomic<bool> MustExit;
	Monitor*          Monitor              = nullptr;
	HeavyLoadMode     CurrentHeavyLoadMode = HeavyLoadMode::Off;
	PowerSource       CurrentPowerSource   = PowerSource::Unknown;
	Cooloff           SourceCooloff;
	Cooloff           HeavyCooloff;

	void      Run();
	TimePoint Now();
};

} // namespace homepower
