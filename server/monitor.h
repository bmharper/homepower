#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <time.h>

#include "commands.h"

namespace homepower {

class Monitor {
public:
	// This is altered in the constructor, to try and find it relative to the current binary
	std::string InverterPath = "/home/pi/homepower/build/inverter";

	int                SampleWriteInterval    = 20;   // Write to database once every N samples.
	int                SecondsBetweenSamples  = 5;    // Record data every N seconds
	int                OverloadThresholdWatts = 2500; // The inverter is overloaded if the output load goes beyond this
	int                GridVoltageThreshold   = 200;  // Grid voltage below this is considered "grid off"
	std::atomic<bool>  IsInitialized;                 // Set to true once we've made our first successful reading
	std::atomic<bool>  IsOverloaded;                  // Signalled when inverter usage is higher than OverloadThresholdWatts
	std::atomic<bool>  HasGridPower;                  // True if the grid is on
	std::atomic<int>   SolarV;                        // Solar voltage
	std::atomic<int>   AvgSolarV;                     // Average Solar voltage (over last X minutes)
	std::atomic<int>   MaxLoadW;                      // Max load watts in last X window
	std::atomic<int>   AvgLoadW;                      // Average load watts over last 5 samples
	std::atomic<float> BatteryV;                      // Battery voltage
	//std::atomic<bool>  PVIsTooWeakForLoads;           // Set true if we're very sure that the PV is not capable of powering the current load

	std::atomic<bool>        IsHeavyOnInverter;  // Set by Controller - true when heavy loads are on the inverter
	std::atomic<PowerSource> CurrentPowerSource; // Set by Controller

	Monitor();
	void Start();
	void Stop();

	// Run 'InverterPath' with the given cmd, and return process exit code.
	// Stdout is returned in 'stdout'
	int RunInverterQuery(std::string cmd, std::string& stdout);

	// Execute a command that does not produce any output besides "(ACK",
	bool RunInverterCmd(std::string cmd);

private:
	struct Record {
		time_t Time;
		float  ACInV;
		float  ACInHz;
		float  ACOutV;
		float  ACOutHz;
		float  LoadVA;
		float  LoadW;
		float  LoadP;
		float  BatP;
		float  BatChA;
		float  BatV;
		float  Temp;
		float  PvA;
		float  PvV;
		float  PvW;
		float  Unknown1; // Similar to PvW on Bernie's inverter
		bool   Heavy;
	};
	std::mutex          InverterLock; // This is held whenever talking to the inverter
	std::vector<Record> Records;
	std::vector<float>  SolarVHistory;
	std::vector<float>  LoadWHistory;
	std::vector<float>  PvWHistory;
	std::thread         Thread;
	std::atomic<bool>   MustExit;
	int                 RecordNext             = 0;
	int                 SolarVHistorySize      = 30;
	int                 BatteryModeHistorySize = 60;

	void Run();
	bool ReadInverterStats(bool saveReading);
	void UpdateStats(const Record& r);
	//void ComputePVStrength();
	bool        MakeRecord(std::string inp, Record& r);
	bool        CommitReadings();
	std::string ProcessPath();
};

} // namespace homepower