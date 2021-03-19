#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <time.h>

#include "commands.h"
#include "inverter.h"

namespace homepower {

class Monitor {
public:
	int                SampleWriteInterval    = 50;   // Write to database once every N samples (we rate-limit this to improve SSD endurance).
	int                SecondsBetweenSamples  = 2;    // Record data every N seconds
	int                OverloadThresholdWatts = 2950; // The inverter is overloaded if the output load goes beyond this
	int                GridVoltageThreshold   = 200;  // Grid voltage below this is considered "grid off"
	std::atomic<bool>  IsInitialized;                 // Set to true once we've made our first successful reading
	std::atomic<bool>  IsOverloaded;                  // Signalled when inverter usage is higher than OverloadThresholdWatts
	std::atomic<bool>  HasGridPower;                  // True if the grid is on
	std::atomic<int>   SolarV;                        // Solar voltage
	std::atomic<int>   AvgSolarV;                     // Average Solar voltage (over last X minutes)
	std::atomic<int>   MaxLoadW;                      // Max load watts in last X window
	std::atomic<int>   AvgLoadW;                      // Average load watts over last 5 samples
	std::atomic<float> BatteryV;                      // Battery voltage
	std::atomic<int>   SolarDeficitW;                 // Watts of load minus Watts of solar (will be zero if it looks like solar is powering loads 100%)

	std::atomic<bool>        IsHeavyOnInverter;  // Set by Controller - true when heavy loads are on the inverter
	std::atomic<PowerSource> CurrentPowerSource; // Set by Controller

	std::mutex InverterLock; // This is held whenever talking to the Inverter
	Inverter   Inverter;     // You must hold InverterLock when talking to Inverter

	Monitor();
	void Start();
	void Stop();

	// Run 'InverterPath' with the given cmd, and return process exit code.
	// Stdout is returned in 'stdout'
	int RunInverterQuery(std::string cmd, std::string& stdout);

	// Execute a command that does not produce any output besides "(ACK",
	bool RunInverterCmd(std::string cmd);

private:
	std::vector<Inverter::Record_QPIGS> Records;
	std::vector<float>                  SolarVHistory;
	std::vector<float>                  LoadWHistory;
	std::vector<float>                  SolarWHistory;
	std::vector<float>                  GridVHistory;
	std::thread                         Thread;
	std::atomic<bool>                   MustExit;
	int                                 RecordNext             = 0;
	int                                 GridVHistorySize       = 3;
	int                                 SolarVHistorySize      = 30;
	int                                 BatteryModeHistorySize = 60;

	void Run();
	bool ReadInverterStats(bool saveReading);
	void UpdateStats(const Inverter::Record_QPIGS& r);
	void ComputeSolarDeficit();
	bool CommitReadings();
};

} // namespace homepower