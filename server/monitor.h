#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <time.h>

#include "commands.h"
#include "inverter.h"
#include "ringbuffer.h"
#include "monitorUtils.h"

namespace homepower {

enum class DBModes {
	Postgres,
	SQLite,
};

class Monitor {
public:
	int                SampleWriteInterval   = 12;   // Write to database once every N samples (can be rate-limited to improve SSD endurance).
	int                SecondsBetweenSamples = 1;    // Record data every N seconds
	int                InverterSustainedW    = 5600; // Rated sustained output power of inverter
	int                BatteryWh             = 4800; // Size of battery in watt-hours size of battery
	int                GridVoltageThreshold  = 200;  // Grid voltage below this is considered "grid off"
	std::atomic<bool>  IsInitialized;                // Set to true once we've made our first successful reading
	std::atomic<bool>  IsOutputOverloaded;           // Signalled when inverter usage is higher than OverloadThresholdWatts
	std::atomic<bool>  IsBatteryOverloaded;          // Signalled when we are drawing too much power from the battery
	std::atomic<bool>  HasGridPower;                 // True if the grid is on
	std::atomic<float> SolarV;                       // Instantaneous solar voltage
	std::atomic<float> AvgSolarV;                    // Average solar voltage over last 60 seconds
	std::atomic<float> AvgSolarW;                    // Average solar wattage over last 60 seconds
	std::atomic<float> AvgLoadW;                     // Average load wattage over last 60 seconds
	std::atomic<float> BatteryV;                     // Battery voltage
	std::atomic<float> BatteryP;                     // Battery charge percentage (0..100)
	std::atomic<float> AvgBatteryP;                  // Average battery charge percentage (0..100) over last 10 minutes.
	std::atomic<float> MinBatteryP;                  // Minimum battery charge percentage (0..100) over last 10 minutes. The 10 minutes is important for BMS equalization at 100% SOC.

	std::atomic<bool> IsHeavyOnInverter; // Set by Controller - true when heavy loads are on the inverter

	std::mutex          InverterLock; // This is held whenever talking to the Inverter
	homepower::Inverter Inverter;     // You must hold InverterLock when talking to Inverter

	DBModes DBMode = DBModes::SQLite; // Which database to write to

	std::string SQLiteFilename = "/mnt/ramdisk/readings.sqlite"; // When DBMode is SQLite, then we write to this sqlite DB

	std::string PostgresHost     = "localhost"; // When DBMode is Postgres, hostname
	std::string PostgresPort     = "5432";      // When DBMode is Postgres, port
	std::string PostgresDB       = "power";     // When DBMode is Postgres, db name
	std::string PostgresUsername = "pi";        // When DBMode is Postgres, username
	std::string PostgresPassword = "homepower"; // When DBMode is Postgres, password

	Monitor();
	void Start();
	void Stop();

	// Execute a command that does not produce any output besides "(ACK",
	bool RunInverterCmd(std::string cmd);

private:
	std::mutex                         RecordsLock;     // Guards access to Records
	RingBuffer<Inverter::Record_QPIGS> Records;         // Records queued to be written into DB. Guarded by RecordsLock
	RingBuffer<History>                SolarVHistory;   // Solar voltage
	RingBuffer<History>                LoadWHistory;    // Watts output by inverter
	RingBuffer<History>                DeficitWHistory; // Watts that we needed to draw from the battery or the grid to meet load. This is LoadWatt - SolarWatt
	RingBuffer<History>                SolarWHistory;   // Watts of solar power generated (could be going to battery or loads)
	RingBuffer<History>                GridVHistory;    // Grid voltage (for detecting if grid is live or not)
	RingBuffer<History>                BatVHistory;     // Battery voltage charge
	RingBuffer<History>                BatPHistory;     // Battery percentage charge
	std::thread                        Thread;
	std::atomic<bool>                  MustExit;
	bool                               HasWrittenToDB = false;

	//std::mutex                         DBThreadRecordsLock;
	//RingBuffer<Inverter::Record_QPIGS> DBThreadRecords; // Records queued to be written into DB, and owned by the DB thread

	void Run();
	void DBThread();
	bool ReadInverterStats(bool saveReading);
	void UpdateStats(const Inverter::Record_QPIGS& r);
	bool CommitReadings(RingBuffer<Inverter::Record_QPIGS>& records);
};

} // namespace homepower