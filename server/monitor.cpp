#include "monitor.h"
#include <unistd.h>
#include <stdio.h>
#include <float.h>
#include <algorithm>
#include "../json.hpp"

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
	AvgSolarV           = 0;
	MustExit            = false;
	IsHeavyOnInverter   = false;
	CurrentPowerSource  = PowerSource::Unknown;
	BatteryV            = 0;
	BatteryP            = 0;
	RecordNext          = 0; // Send one sample as soon as we come online

	// If Records is full, and we can't talk to the DB, then we drop records.
	// A record is 272 bytes, so 256 * 275 = about 64kb
	Records.Initialize(256);

	// We want 5 minutes of history, so if we sample once every 2 seconds, then that is
	// 30 * 5 = 150. Rounded up to next power of 2, we get 256.
	SolarVHistory.Initialize(256);
	LoadWHistory.Initialize(256);
	DeficitWHistory.Initialize(256);
	SolarWHistory.Initialize(256);
	GridVHistory.Initialize(256);
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
	auto lastSaveTime = 0;
	while (!MustExit) {
		bool saveReading = time(nullptr) - lastSaveTime > SecondsBetweenSamples;
		bool readOK      = false;
		for (int attempt = 0; attempt < 3 && !MustExit; attempt++) {
			readOK = ReadInverterStats(saveReading);
			if (readOK)
				break;
		}
		if (readOK && saveReading) {
			lastSaveTime = time(nullptr);
		}
		RecordNext--;
		if (RecordNext <= 0 && Records.Size() != 0) {
			if (CommitReadings()) {
				Records.Clear();
				RecordNext = SampleWriteInterval;
			}
		}
		sleep(1);
	};
}

bool Monitor::ReadInverterStats(bool saveReading) {
	//printf("Reading QPIGS %f\n", (double) clock() / (double) CLOCKS_PER_SEC);
	lock_guard<mutex>      lock(InverterLock);
	Inverter::Record_QPIGS record;
	auto                   res = Inverter.ExecuteT("QPIGS", record);
	//printf("Reading QPIGS %f done\n", (double) clock() / (double) CLOCKS_PER_SEC);
	if (res != Inverter::Response::OK) {
		fprintf(stderr, "Failed to run inverter query. Error = %d\n", (int) res);
		return false;
	}
	record.Heavy = IsHeavyOnInverter;
	if (saveReading)
		Records.Add(record);
	UpdateStats(record);
	return true;
}

void Monitor::UpdateStats(const Inverter::Record_QPIGS& r) {
	IsInitialized = true;

	time_t now = time(nullptr);

	GridVHistory.Add({now, r.ACInV});
	SolarVHistory.Add({now, r.PvV});

	AvgSolarV = Average(now - 60, SolarVHistory);

	LoadWHistory.Add({now, r.LoadW});
	DeficitWHistory.Add({now, std::max(0.0f, r.LoadW - r.PvW)});

	SolarWHistory.Add({now, r.PvW});

	// These numbers are roughly drawn from my Voltronic 5.6kw MKS 4 inverter (aka MKS IV),
	// but tweaked to be more conservative.
	bool outputOverload = false;
	if (Average(now - 10, LoadWHistory) > (float) InverterSustainedW * 0.97f) {
		outputOverload = true;
	} else if (Average(now - 5, LoadWHistory) > (float) InverterSustainedW * 1.3f) {
		outputOverload = true;
	} else if (r.LoadW > (float) InverterSustainedW * 1.7f) {
		outputOverload = true;
	}

	IsOutputOverloaded = outputOverload;

	//printf("Output:  %4.0f %4.0f %4.0f vs %4.0f %4.0f %4.0f, Overloaded: %s\n", Average(now - 10, LoadWHistory), Average(now - 5, LoadWHistory), r.LoadW,
	//       (float) InverterSustainedW * 0.97f, (float) InverterSustainedW * 1.3f, (float) InverterSustainedW * 1.7f, outputOverload ? "yes" : "no");

	// These numbers are drawn from my Pylontech UP5000 battery
	bool batteryOverloaded = false;
	if (Average(now - 4 * 60, DeficitWHistory) > (float) BatteryWh * 0.5f) {
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

	SolarV   = (int) r.PvV;
	BatteryV = r.BatV;
	BatteryP = r.BatP;

	//if (!HasGridPower)
	//	printf("Don't have grid power %f, %f\n", r.ACInHz, (float) GridVoltageThreshold);
}

static double GetDbl(const nlohmann::json& j, const char* key) {
	if (j.find(key) == j.end())
		return 0;
	return j[key].get<double>();
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

bool Monitor::CommitReadings() {
	if (Records.Size() == 0)
		return true;

	bool postgres = DBMode == DBModes::Postgres;

	string create = CreateSchemaSQL;
	std::replace(create.begin(), create.end(), '\n', ' ');

	string sql;

	if (postgres) {
		sql += "SET LOCAL synchronous_commit TO OFF; ";
		sql += create;
	} else {
		time_t now    = time(nullptr);
		string create = CreateSchemaSQL;
		std::replace(create.begin(), create.end(), '\n', ' ');
		sql += create + " ";
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
	uint32_t nRecords = Records.Size();
	for (size_t i = 0; i < nRecords; i++) {
		const auto& r = Records.Peek(i);
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
	return system(cmd.c_str()) == 0;
}

} // namespace homepower