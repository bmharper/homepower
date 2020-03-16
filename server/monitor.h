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

	int               SampleWriteInterval    = 60; // Write to database once every N samples.
	int               SecondsBetweenSamples  = 5;
	int               OverloadThresholdWatts = 2900;
	int               GridVoltageThreshold   = 200; // Grid voltage below this is considered "grid off"
	std::atomic<bool> IsInitialized;                // Set to true once we've made our first successful reading
	std::atomic<bool> IsOverloaded;                 // Signalled when inverter usage is higher than OverloadThresholdWatts
	std::atomic<bool> HasGridPower;                 // True if the grid is on
	std::atomic<int>  SolarV;                       // Solar voltage
	std::atomic<bool> IsHeavyOnInverter;            // Set by Controller - true when heavy loads are on the inverter

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
	std::thread         Thread;
	std::atomic<bool>   MustExit;
	int                 RecordNext = 0;

	void Run();
	bool ReadInverter(bool saveReading);
	bool MakeRecord(std::string inp, Record& r);
	bool CommitReadings();
};

} // namespace homepower