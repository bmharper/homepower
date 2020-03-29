#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <time.h>

namespace homepower {

class Monitor {
public:
	std::string InverterPath = "/home/pi/homepower/build/inverter";

	int               SampleWriteInterval       = 20;   // Write to database once every N samples.
	int               SecondsBetweenSamples     = 5;    // Record data every N seconds
	int               OverloadThresholdWatts    = 2950; // The inverter is overloaded if the output load goes beyond this
	int               BatteryModeThresholdWatts = 1000; // (NOT IMPLEMENTED) If load has spiked beyond this in last 10 minutes, then don't go onto "Battery Priority" mode
	int               GridVoltageThreshold      = 200;  // Grid voltage below this is considered "grid off"
	std::atomic<bool> IsInitialized;                    // Set to true once we've made our first successful reading
	std::atomic<bool> IsOverloaded;                     // Signalled when inverter usage is higher than OverloadThresholdWatts
	std::atomic<bool> HasGridPower;                     // True if the grid is on
	std::atomic<int>  SolarV;                           // Solar voltage
	std::atomic<int>  AvgSolarV;                        // Average Solar voltage (over last X minutes)
	std::atomic<bool> LoadTooHighForBatteryMode;        // (NOT IMPLEMENTED) Reflects the historic power draw, and it's relation to BatteryModeThresholdWatts

	std::atomic<bool> IsHeavyOnInverter; // Set by Controller - true when heavy loads are on the inverter

	Monitor();
	void Start();
	void Stop();

private:
	struct Record {
		time_t Time;
		float  ACInV;
		float  ACInHz;
		float  ACOutV;
		float  ACOutHz;
		float  LoadW;
		float  BatChA;
		float  BatV;
		float  Temp;
		float  PvV;
		float  PvW;
		bool   Heavy;
	};
	std::vector<Record> Records;
	std::vector<float>  SolarVHistory;
	std::thread         Thread;
	std::atomic<bool>   MustExit;
	int                 AverageWindow = 30;
	int                 RecordNext    = 0;

	void Run();
	bool ReadInverter(bool saveReading);
	void UpdateStats(const Record& r);
	bool MakeRecord(std::string inp, Record& r);
	bool CommitReadings();
};

} // namespace homepower