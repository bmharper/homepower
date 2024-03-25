#include "monitor.h"
#include <unistd.h>
#include <stdio.h>
#include <float.h>
#include <algorithm>

using namespace std;

namespace homepower {

// Return the average value from the history buffer, going no further back than afterTime
double Average(time_t afterTime, const RingBuffer<History>& history) {
	double   sum      = 0;
	unsigned nsamples = 0;
	uint32_t idx      = history.Size() - 1;
	while (true) {
		if (idx == -1)
			break;
		auto sample = history.Peek(idx);
		if (sample.Time < afterTime)
			break;
		sum += sample.Value;
		nsamples++;
		idx--;
	}
	return nsamples == 0 ? 0 : sum / (double) nsamples;
}

float Maximum(time_t afterTime, const RingBuffer<History>& history) {
	float    maxv = -FLT_MAX;
	uint32_t idx  = history.Size() - 1;
	while (true) {
		if (idx == -1)
			break;
		auto sample = history.Peek(idx);
		if (sample.Time < afterTime)
			break;
		maxv = std::max(maxv, sample.Value);
		idx--;
	}
	return maxv;
}

// trim space from end
static string TrimSpace(const string& s) {
	string x = s;
	while (x.size() != 0 && (x[x.size() - 1] == '\n' || x[x.size() - 1] == ' ' || x[x.size() - 1] == '\t')) {
		x.erase(x.end() - 1, x.end());
	}
	return x;
}

Monitor::Monitor() {
	IsInitialized       = false;
	IsOutputOverloaded  = false;
	IsBatteryOverloaded = false;
	HasGridPower        = true;
	SolarV              = 0;
	AvgSolarW           = 0;
	AvgSolarV           = 0;
	AvgBatteryP         = 0;
	MustExit            = false;
	IsHeavyOnInverter   = false;
	CurrentPowerSource  = PowerSource::Unknown;
	BatteryV            = 0;
	BatteryP            = 0;
	AvgLoadW            = 0;

	// If Records is full, and we can't talk to the DB, then we drop records.
	// A record is 272 bytes, so 256 * 275 = about 64kb
	Records.Initialize(256);

	// We want 5 minutes of history, so if we sample once every 2 seconds, then that is
	// 30 * 5 = 150. Rounded up to next power of 2, we get 256.
	// For LoadW and SolarW we want even more history, so we do 512 for them.
	// Screw it, might as well do 512 for all of them.
	SolarVHistory.Initialize(512);
	LoadWHistory.Initialize(512);
	DeficitWHistory.Initialize(512);
	SolarWHistory.Initialize(512);
	GridVHistory.Initialize(512);
	BatPHistory.Initialize(512);
	BatVHistory.Initialize(512);
}

void Monitor::Start() {
	Thread = thread([&]() {
		printf("Monitor started\n");
		Run();
		printf("Monitor exited\n");
	});
}

void Monitor::Stop() {
	MustExit = true;
	Thread.join();
}

bool Monitor::RunInverterCmd(std::string cmd) {
	lock_guard<mutex> lock(InverterLock);
	auto              res = Inverter.Execute(cmd);
	if (res != Inverter::Response::OK) {
		fprintf(stderr, "Command '%s' failed with %s\n", cmd.c_str(), Inverter::DescribeResponse(res).c_str());
		return false;
	}
	return true;
}

void Monitor::Run() {
	// Launch DB commit on a separate thread
	auto dbThreadFunc = [this]() -> void {
		this->DBThread();
	};
	auto dbThread = std::thread(dbThreadFunc);

	auto lastSaveTime = 0;
	while (!MustExit) {
		bool saveReading = time(nullptr) - lastSaveTime >= SecondsBetweenSamples;
		bool readOK      = false;
		for (int attempt = 0; attempt < 3 && !MustExit; attempt++) {
			readOK = ReadInverterStats(saveReading);
			if (readOK)
				break;
		}
		if (readOK && saveReading) {
			lastSaveTime = time(nullptr);
		}
		usleep(500 * 1000);
	};

	dbThread.join();
}

// DBThread runs on a separate thread to the monitor system, so that if our DB
// host goes down, we don't stall the monitoring.
void Monitor::DBThread() {
	// Our own queue. The ring buffer will automatically eat into it's tail
	// if it gets too full, so we don't need to do anything special if
	// we run out of private buffer space. The ring buffer will just drop
	// the oldest samples.
	RingBuffer<Inverter::Record_QPIGS> privateQueue;
	privateQueue.Initialize(256);

	while (!MustExit) {
		// Suck records out of 'Records', and move them into our private queue.
		RecordsLock.lock();
		while (Records.Size() != 0) {
			privateQueue.Add(Records.Next());
		}
		RecordsLock.unlock();

		// As soon as we have enough samples (or we have just one sample, and we've just booted up), send records to the DB
		if (privateQueue.Size() >= SampleWriteInterval || (privateQueue.Size() >= 1 && !HasWrittenToDB)) {
			if (CommitReadings(privateQueue)) {
				privateQueue.Clear();
			}
		}
		sleep(1);
	}
}

bool Monitor::ReadInverterStats(bool saveReading) {
	//printf("Reading QPIGS %f\n", (double) clock() / (double) CLOCKS_PER_SEC);
	lock_guard<mutex>      lock(InverterLock);
	Inverter::Record_QPIGS record;
	auto                   res = Inverter.ExecuteT("QPIGS", record);
	//printf("Reading QPIGS %f done\n", (double) clock() / (double) CLOCKS_PER_SEC);
	if (res != Inverter::Response::OK) {
		fprintf(stderr, "Failed to run inverter query. Error = %s\n", Inverter::DescribeResponse(res).c_str());
		return false;
	}
	record.Heavy = IsHeavyOnInverter;
	if (saveReading) {
		RecordsLock.lock();
		Records.Add(record);
		RecordsLock.unlock();
	}
	UpdateStats(record);
	return true;
}

// We need to be careful to filter out sporadic zero readings, which happen
// about once every two weeks or so. Initially, I would trust BatP's instantanous
// reading, but when it drops to zero for a single sample, then our controller
// freaks out and switches to charge mode.
void Monitor::UpdateStats(const Inverter::Record_QPIGS& r) {
	IsInitialized = true;

	time_t now = time(nullptr);

	GridVHistory.Add({now, r.ACInV});
	SolarVHistory.Add({now, r.PvV});
	BatPHistory.Add({now, r.BatP});
	BatVHistory.Add({now, r.BatV});

	AvgSolarV = Average(now - 60, SolarVHistory);

	LoadWHistory.Add({now, r.LoadW});
	DeficitWHistory.Add({now, std::max(0.0f, r.LoadW - r.PvW)});

	SolarWHistory.Add({now, r.PvW});

	float filteredSolarV = Maximum(now - 15, SolarVHistory);
	float filteredBatP   = Maximum(now - 30, BatPHistory);
	float filteredBatV   = Maximum(now - 30, BatVHistory);

	// These numbers are roughly drawn from my Voltronic 5.6kw MKS 4 inverter (aka MKS IV),
	// but tweaked to be more conservative.
	bool outputOverload = false;
	if (Average(now - 6, LoadWHistory) > (float) InverterSustainedW * 0.97f) {
		outputOverload = true;
	} else if (Average(now - 3, LoadWHistory) > (float) InverterSustainedW * 1.1f) {
		outputOverload = true;
	} else if (r.LoadW > (float) InverterSustainedW * 1.5f) {
		outputOverload = true;
	}

	IsOutputOverloaded = outputOverload;

	//printf("Output:  %4.0f %4.0f %4.0f vs %4.0f %4.0f %4.0f, Overloaded: %s\n", Average(now - 10, LoadWHistory), Average(now - 5, LoadWHistory), r.LoadW,
	//       (float) InverterSustainedW * 0.97f, (float) InverterSustainedW * 1.3f, (float) InverterSustainedW * 1.7f, outputOverload ? "yes" : "no");

	// These numbers are drawn from my Pylontech UP5000 battery, with a discharge C of about 0.5
	bool batteryOverloaded = false;
	if (Average(now - 2 * 60, DeficitWHistory) > (float) BatteryWh * 0.5f) {
		batteryOverloaded = true;
	} else if (Average(now - 60, DeficitWHistory) > (float) BatteryWh * 0.9f) {
		batteryOverloaded = true;
	} else if (Average(now - 15, DeficitWHistory) > (float) BatteryWh * 1.2f) {
		batteryOverloaded = true;
	} else if (Average(now - 5, DeficitWHistory) > (float) BatteryWh * 1.5f) {
		batteryOverloaded = true;
	}

	IsBatteryOverloaded = batteryOverloaded;

	//printf("Battery: %4.0f %4.0f %4.0f vs %4.0f %4.0f %4.0f, Overloaded: %s\n", Average(now - 4 * 60, DeficitWHistory), Average(now - 60, DeficitWHistory), Average(now - 15, DeficitWHistory),
	//       (float) BatteryWh * 0.5f, (float) BatteryWh * 0.9f, (float) BatteryWh * 1.5f, batteryOverloaded ? "yes" : "no");

	// Every now and then the inverter reports zero voltage from the grid for just a single
	// sample, and we don't want those blips to cause us to change state.
	HasGridPower = (float) Maximum(now - 5, GridVHistory) > (float) GridVoltageThreshold;

	SolarV      = filteredSolarV;
	BatteryV    = filteredBatV;
	BatteryP    = filteredBatP;
	AvgSolarW   = Average(now - 5 * 60, SolarWHistory);
	AvgLoadW    = Average(now - 5 * 60, LoadWHistory);
	AvgBatteryP = Average(now - 10 * 60, BatPHistory);

	//if (!HasGridPower)
	//	printf("Don't have grid power %f, %f\n", r.ACInHz, (float) GridVoltageThreshold);
}

static void AddDbl(string& s, double v, bool comma = true) {
	char buf[100];
	sprintf(buf, "%.3f", v);
	s += buf;
	if (comma)
		s += ",";
}

static void AddBool(string& s, bool v, bool comma = true) {
	char buf[100];
	sprintf(buf, "%s", v ? "true" : "false");
	s += buf;
	if (comma)
		s += ",";
}

// This content is duplicated inside dbcreate.sql
const char* CreateSchemaSQL = R"(
CREATE TABLE IF NOT EXISTS readings (
	time TIMESTAMP NOT NULL PRIMARY KEY,
	acInV REAL,
	acInHz REAL,
	acOutV REAL,
	acOutHz REAL,
	loadVA REAL,
	loadW REAL,
	loadP REAL,
	busV REAL,
	batV REAL,
	batChA REAL,
	batP REAL,
	temp REAL,
	pvA REAL,
	pvV REAL,
	pvW REAL,
	unknown1 REAL,
	heavy BOOLEAN
);
)";

bool Monitor::CommitReadings(RingBuffer<Inverter::Record_QPIGS>& records) {
	if (records.Size() == 0)
		return true;

	if (DBMode == DBModes::SQLite && SQLiteFilename == "/dev/null")
		return true;

	bool postgres = DBMode == DBModes::Postgres;

	string create = CreateSchemaSQL;
	std::replace(create.begin(), create.end(), '\n', ' ');

	string sql;

	if (postgres) {
		sql += "SET LOCAL synchronous_commit TO OFF; ";
		if (!HasWrittenToDB)
			sql += create;
	} else {
		time_t now = time(nullptr);
		if (!HasWrittenToDB)
			sql += create + " ";
		// We assume that our SQLite DB is on a ramdisk, so we limit it's size
		sql += "DELETE FROM readings WHERE time < ";
		AddDbl(sql, now - 3 * 24 * 3600, false);
		sql += "; ";
	}
	sql += "INSERT INTO readings (";
	sql += "time,";
	sql += "acInV,";
	sql += "acInHz,";
	sql += "acOutV,";
	sql += "acOutHz,";
	sql += "loadW,";
	sql += "loadVA,";
	sql += "loadP,";
	sql += "batChA,";
	sql += "batV,";
	sql += "batP,";
	sql += "temp,";
	sql += "pvV,";
	sql += "pvA,";
	sql += "pvW,";
	sql += "unknown1,";
	sql += "heavy";
	sql += ") VALUES ";
	uint32_t nRecords = records.Size();
	for (size_t i = 0; i < nRecords; i++) {
		const auto& r = records.Peek(i);
		sql += "(";
		if (postgres) {
			sql += "to_timestamp(";
			AddDbl(sql, r.Time, false);
			sql += ") AT TIME ZONE 'UTC',";
		} else {
			AddDbl(sql, r.Time);
		}
		AddDbl(sql, r.ACInV);
		AddDbl(sql, r.ACInHz);
		AddDbl(sql, r.ACOutV);
		AddDbl(sql, r.ACOutHz);
		AddDbl(sql, r.LoadW);
		AddDbl(sql, r.LoadVA);
		AddDbl(sql, r.LoadP);
		AddDbl(sql, r.BatChA);
		AddDbl(sql, r.BatV);
		AddDbl(sql, r.BatP);
		AddDbl(sql, r.Temp);
		AddDbl(sql, r.PvV);
		AddDbl(sql, r.PvA);
		AddDbl(sql, r.PvW);
		AddDbl(sql, r.Unknown1);
		AddBool(sql, r.Heavy, false);
		sql += ")";
		if (i != nRecords - 1)
			sql += ",";
	}
	// This happens every now and then.. maybe due to rounding error on the seconds.. I'm not actually sure.
	sql += " ON CONFLICT(time) DO NOTHING";
	//printf("Send:\n%s\n", sql.c_str());
	string cmd;
	if (postgres) {
		cmd = "PGPASSWORD=" + PostgresPassword +
		      " psql " +
		      " --host " + PostgresHost +
		      " --username " + PostgresUsername +
		      " --dbname " + PostgresDB +
		      " --port " + PostgresPort +
		      " --command \"" + sql + "\"";
	} else {
		cmd = "sqlite3 \"" + SQLiteFilename + "\" \"" + sql + "\"";
	}
	bool dbWriteOK = system(cmd.c_str()) == 0;
	if (dbWriteOK) {
		HasWrittenToDB = true;
	}
	return dbWriteOK;
}

} // namespace homepower