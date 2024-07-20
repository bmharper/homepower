#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <time.h>

#include "commands.h"
#include "controllerUtils.h"

namespace homepower {

class Monitor;

enum class HeavyLoadMode {
	AlwaysOn,    // Always keep heavy loads on (but power them from grid if we have no solar)
	OnWithSolar, // Only power heavy loads from battery when we have solar power (but power them from grid if available)
};

enum class HeavyLoadState {
	Off,
	Grid,
	Inverter,
};

// Used for specifying charge curves
struct MinChargePoint {
	TimePoint Time;
	float     Soft; // If battery charge is below this, then we run loads off grid instead of battery
	float     Hard; // If battery charge is below this, then we charge battery from grid
};

const char* HeavyLoadModeToString(HeavyLoadMode mode);
const char* HeavyLoadStateToString(HeavyLoadState mode);

class Controller {
public:
	bool EnableGpio                = true;  // This can be disabled for debugging
	bool EnableInverterStateChange = true;  // If false, then do not actually change any inverter state, but pretend that we do.
	int  GpioPinGrid               = 17;    // GPIO/BCM pin number set to 1 when switching heavy loads to grid
	int  GpioPinInverter           = 18;    // GPIO/BCM pin number set to 1 when switching heavy loads to inverter
	int  SwitchSleepMilliseconds   = 10;    // 50hz = 20ms cycle time. Hager ESC225 have 25ms closing delay, and 15ms opening delay.
	int  TimezoneOffsetMinutes     = 120;   // 120 = UTC+2 (Overridden by constructor)
	bool EnableAutoCharge          = false; // Enable switching grid/inverter modes, and solar/grid charge mode, depending on battery SOC

	// Maximum hours between battery equalization. Equalization implies being at 100% SOC for 10 minutes.
	// Note that it's good to have a peridd less than 24 hours between equalizations, otherwise the equaliztion
	// moment time can drift forward each day, if you're always equalizing by charging from the grid.
	int HoursBetweenEqualize = 22;

	// Minimum charge curve
	static const int MaxNMinChargePoints = 30;
	int              NMinCharge          = 2;        // Minimum 2, Maximum MaxNMinChargePoints (30)
	MinChargePoint   MinCharge[MaxNMinChargePoints]; // We interpolate between these points. If battery charge is below this, then we run loads off grid instead of battery

	Controller(homepower::Monitor* monitor, bool enableGpio, bool enableInverterStateChange);
	~Controller();
	bool Start();
	void Stop();
	void SetHeavyLoadMode(HeavyLoadMode m);
	void SetHeavyLoadState(HeavyLoadState m, bool forceWrite = false);
	void SetStormMode(int hours);
	bool BakeChargeLimits();
	void PrintChargeLimits();

private:
	std::thread         Thread;
	std::atomic<bool>   MustExit;
	homepower::Monitor* Monitor               = nullptr;
	HeavyLoadMode       CurrentHeavyLoadMode  = HeavyLoadMode::OnWithSolar;
	HeavyLoadState      CurrentHeavyLoadState = HeavyLoadState::Off;
	PowerSource         CurrentPowerSource    = PowerSource::Unknown;
	ChargerPriority     CurrentChargePriority = ChargerPriority::Unknown;
	Cooloff             HeavyCooloff;
	std::mutex          HeavyLoadLock;                  // Guards access to CurrentHeavyLoadMode and CurrentHeavyLoadState
	time_t              LastEqualizeAt             = 0; // Time when we last equalized (100% SOC for 10 minutes)
	time_t              SwitchPowerSourceAt        = 0; // Time when we last switched the power source
	time_t              SwitchChargerPriorityAt    = 0; // Time when we last switched the charger priority
	time_t              LastAttemptedSourceSwitch  = 0; // Last time we attempted to switch power source
	time_t              LastAttemptedChargerSwitch = 0; // Last time we attempted to switch charger priority
	time_t              LastSoftSwitch             = 0; // Time when we last changed modes because battery was lower than soft limit
	time_t              LastHardSwitch             = 0; // Time when we last changed modes because battery was lower than hard limit
	std::atomic<time_t> StormModeUntil;                 // Remain in storm mode until this time

	// The following 3 arrays are parallel
	TimePoint MinChargeTimePoints[MaxNMinChargePoints];
	float     MinChargeSoft[MaxNMinChargePoints];
	float     MinChargeHard[MaxNMinChargePoints];

	// Don't allow switching to SBU more than once every 5 minutes.
	// This is a safeguard against bugs that could lead us to flipping state too frequently.
	// Such a bug happened in practice.
	time_t MinSecondsBetweenSBUSwitches = 5 * 60;

	void      Run();
	TimePoint Now();
};

} // namespace homepower
