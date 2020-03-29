#pragma once

#include <thread>
#include <atomic>
#include <time.h>

namespace homepower {

class Monitor;

enum class PowerMode {
	Off,
	Grid,
	Inverter,
};

const char* ModeToString(PowerMode mode);

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
	int       GpioPinGrid           = 0;
	int       GpioPinInverter       = 1;
	int       SleepMilliseconds     = 20;               // 50hz = 20ms cycle time. Hager ESC225 have 25ms switch off time, and 10ms switch on time.
	int       TimezoneOffsetMinutes = 120;              // 120 = UTC+2 (Overridden by constructor)
	int       MinSolarVoltage       = 160;              // Minimum solar voltage before we'll put heavy loads on it
	TimePoint SolarOnAt             = TimePoint(7, 0);  // Ignore any solar voltage before this time
	TimePoint SolarOffAt            = TimePoint(18, 0); // Ignore any solar voltage after this time

	Controller(homepower::Monitor* monitor);
	void Start();
	void Stop();
	void SetMode(PowerMode m, bool forceWrite = false);

private:
	std::thread       Thread;
	std::atomic<bool> MustExit;
	Monitor*          Monitor        = nullptr;
	PowerMode         Mode           = PowerMode::Off;
	time_t            LastSwitch     = 0;
	time_t            CooloffSeconds = 60; // Don't switch modes more often than this

	void      Run();
	TimePoint Now();
};

} // namespace homepower