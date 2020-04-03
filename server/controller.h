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

class Controller {
public:
	int       GpioPinGrid            = 0;
	int       GpioPinInverter        = 1;
	int       SleepMilliseconds      = 20;               // 50hz = 20ms cycle time. Hager ESC225 have 25ms switch off time, and 10ms switch on time.
	int       TimezoneOffsetMinutes  = 120;              // 120 = UTC+2 (Overridden by constructor)
	int       MinSolarHeavyV         = 160;              // Minimum solar voltage before we'll put heavy loads on it
	int       MinSolarBatterySourceV = 200;              // Minimum solar voltage before we'll place the system in SBU mode. Poor proxy for actual PvW output capability.
	int       MaxLoadBatteryModeW    = 800;              // Maximum load for "SBU" mode
	float     MinBatteryV_SBU        = 25.5f;            // Minimum battery voltage for "SBU" mode
	TimePoint SolarOnAt              = TimePoint(7, 0);  // Ignore any solar voltage before this time
	TimePoint SolarOffAt             = TimePoint(18, 0); // Ignore any solar voltage after this time

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
	time_t            LastHeavySwitch      = 0;
	time_t            LastSourceSwitch     = 0;
	time_t            CooloffSeconds       = 60; // Don't switch modes more often than this, unless it's urgent (eg need to switch off heavy loads, or stop using battery)

	void      Run();
	TimePoint Now();
};

} // namespace homepower