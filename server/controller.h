#pragma once

#include <thread>
#include <atomic>
#include <mutex>
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
	bool operator==(const TimePoint& b) const {
		return Hour == b.Hour && Minute == b.Minute;
	}
	bool operator!=(const TimePoint& b) const {
		return !(*this == b);
	}
	// This is the same as ==, but I prefer the explicit wording
	bool EqualsMinutePrecision(const TimePoint& b) const {
		return Hour == b.Hour && Minute == b.Minute;
	}
	static TimePoint Now(int timezoneOffsetMinutes);
};

// Cooloff represents a time period that doubles every time we make
// an incorrect decision.
// A key reason why this exists is because we have imperfect knowledge.
// We don't know how much power is going to be used by
// the optional circuits until we flip them on.
// For definitions, we think of a conservative state and an optimistic state.
// When we think all is quiet, then we switch to the optimistic state. Only after doing that,
// can we detect that we might have been wrong. Every time we're forced to switch
// from our optimistic state back to our conservative state, we double the cooloff period.
struct Cooloff {
	time_t DefaultCooloffPeriod = 60;      // When we think things are stable, then cooloff period returns to Default
	time_t CooloffPeriod        = 60;      // Current backoff time
	time_t MaxCooloffPeriod     = 60 * 15; // Maximum backoff time
	time_t LastAlarm            = 0;       // Last time that we needed to switch to the conservative state

	// Inform the system that everything appears to be fine
	void SignalFine(time_t now) {
		if (now - LastAlarm > CooloffPeriod * 2) {
			// Since there's no alarm, and our last switch was more than CooloffPeriod*2 ago,
			// we know that we've been in the desiredState for long enough to reset our backoff period.
			CooloffPeriod = DefaultCooloffPeriod;
		}
	}

	// Inform the system that we've needed to switch back to the conservative state (i.e. our optimism was wrong)
	void SignalAlarm(time_t now) {
		LastAlarm     = now;
		CooloffPeriod = std::min(CooloffPeriod * 2, MaxCooloffPeriod);
	}

	// IsGood returns true if we're out of the alarm period
	bool IsGood(time_t now) const {
		return now - LastAlarm > CooloffPeriod;
	}
};

class Controller {
public:
	bool      EnableGpio              = true;              // This can be disabled for debugging
	int       GpioPinGrid             = 17;                // GPIO/BCM pin number set to 1 when switching heavy loads to grid
	int       GpioPinInverter         = 18;                // GPIO/BCM pin number set to 1 when switching heavy loads to inverter
	int       SwitchSleepMilliseconds = 10;                // 50hz = 20ms cycle time. Hager ESC225 have 25ms switch off time, and 10ms switch on time, which is in ADDITION to this delay.
	int       TimezoneOffsetMinutes   = 120;               // 120 = UTC+2 (Overridden by constructor)
	int       MinSolarHeavyV          = 180;               // Minimum solar voltage before we'll put heavy loads on it
	int       MinBatteryChargePercent = 20;                // Switch off heavy loads and switch to SUB when battery gets this low
	int       ChargeMinutes           = 120;               // If we detect that our battery is very low, then go back to charge mode for at least this long
	TimePoint SolarOnAt               = TimePoint(7, 0);   // Ignore any solar voltage before this time
	TimePoint SolarOffAt              = TimePoint(16, 45); // Ignore any solar voltage after this time
	TimePoint TimerSUB                = TimePoint(17, 15); // Switch to SUB at this time
	TimePoint TimerSBU                = TimePoint(21, 0);  // Switch to SBU at this time
	bool      EnablePowerSourceTimer  = false;             // Respect TimerSUB and TimerSBU
	bool      EnablePowerSourceSwitch = false;             // Enable switching between SBU and SUB. My VM III generally runs cooler when in SBU mode.

	// If true, then we keep heavy loads on even if we have no solar power (but we'll still switch them off if overloaded).
	// This is useful for stormy days where we're running on SUB mode, because there's no solar,
	// but we still want to be able to run the washing machine, and auxiliary plugs. This is a manually
	// invoked mode, and default off.
	std::atomic<bool> KeepHeavyOnWithoutSolar;

	Controller(homepower::Monitor* monitor, bool enableGpio);
	~Controller();
	void Start();
	void Stop();
	void SetHeavyLoadMode(HeavyLoadMode m, bool forceWrite = false);
	void ChangePowerSource(PowerSource source);

private:
	std::thread       Thread;
	std::atomic<bool> MustExit;
	Monitor*          Monitor              = nullptr;
	HeavyLoadMode     CurrentHeavyLoadMode = HeavyLoadMode::Off;
	PowerSource       CurrentPowerSource   = PowerSource::Unknown;
	Cooloff           SourceCooloff;
	Cooloff           HeavyCooloff;
	std::mutex        HeavyLoadLock;        // Guards access to SetHeavyLoadMode
	std::atomic<int>  ChangePowerSourceMsg; // Used by HTTP to signal to controller thread to change power source
	time_t            ChargeStartedAt = 0;  // Moment when we decided that we needed to start charging again

	void      Run();
	TimePoint Now();
};

} // namespace homepower
