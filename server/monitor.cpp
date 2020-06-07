#include "monitor.h"
#include <unistd.h>
#include <stdio.h>
#include "../json.hpp"

using namespace std;

namespace homepower {

// Add a sample to a history of samples, chopping off the oldest samples, if the size exceeds maxHistorySize
template <typename T>
void AddToHistory(size_t maxHistorySize, vector<T>& history, T sample) {
	history.push_back(sample);
	if (history.size() > maxHistorySize)
		history.erase(history.begin(), history.begin() + history.size() - (size_t) maxHistorySize);
}

template <typename T>
double Average(const vector<T>& history, unsigned maxSamples = -1) {
	double   sum      = 0;
	unsigned nsamples = 0;
	for (size_t i = history.size() - 1; i != -1 && nsamples < maxSamples; i--) {
		sum += (double) history[i];
		nsamples++;
	}
	return sum / (double) nsamples;
}

template <typename T>
double Maximum(const vector<T>& history) {
	double maximum = -9e99;
	for (auto v : history)
		maximum = v > maximum ? v : maximum;
	return maximum;
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
	IsInitialized     = false;
	IsOverloaded      = false;
	HasGridPower      = true;
	SolarV            = 0;
	AvgSolarV         = 0;
	MaxLoadW          = OverloadThresholdWatts - 1;
	AvgLoadW          = OverloadThresholdWatts - 1;
	MustExit          = false;
	IsHeavyOnInverter = false;
	//PVIsTooWeakForLoads       = true;
	CurrentPowerSource = PowerSource::Unknown;
	BatteryV           = 0;
	RecordNext         = 0; // Send one sample as soon as we come online

	// assume:
	// .../homepower/build/server/server
	// .../homepower/build/inverter
	// here we're going from 'server' to 'inverter'
	auto myPath    = ProcessPath();
	auto suffixPos = myPath.rfind("/server/server");
	if (suffixPos == myPath.size() - 14) {
		InverterPath = myPath.substr(0, myPath.size() - 14) + "/inverter";
	}
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

int Monitor::RunInverterQuery(std::string cmd, std::string& stdout) {
	lock_guard<mutex> lock(InverterLock);
	string            fullCmd = InverterPath + " /dev/hidraw0 " + cmd;
	auto              fd      = popen(fullCmd.c_str(), "r");
	if (!fd) {
		printf("Failed to launch inverter command (%s): %d\n", fullCmd.c_str(), errno);
		return 1;
	}
	int    start = time(nullptr);
	string inp;
	while (time(nullptr) - start < 3) {
		char buf[3000];
		int  n = fread(buf, 1, sizeof(buf) - 1, fd);
		if (n > 0) {
			inp.append(buf, n);
			auto trimmed = TrimSpace(inp);
			if (trimmed.size() > 5 && trimmed[trimmed.size() - 1] == '}') {
				// JSON output (for query commands)
				break;
			}
			if (trimmed == "OK") {
				// Change state commands (eg POP01)
				break;
			}
		}
	}
	stdout = TrimSpace(inp);
	return pclose(fd);
}

bool Monitor::RunInverterCmd(std::string cmd) {
	string out;
	int    res = RunInverterQuery(cmd, out);
	if (res != 0 || out != "OK") {
		fprintf(stderr, "Command '%s' failed with %d, '%s'\n", cmd.c_str(), res, out.c_str());
		return false;
	}
	return true;
}

void Monitor::Run() {
	auto lastSaveTime = 0;
	while (!MustExit) {
		bool saveReading = time(nullptr) - lastSaveTime > SecondsBetweenSamples;
		bool readOK      = ReadInverterStats(saveReading);
		if (readOK && saveReading) {
			lastSaveTime = time(nullptr);
		}
		RecordNext--;
		if (RecordNext <= 0 && Records.size() != 0) {
			if (CommitReadings()) {
				Records.clear();
				RecordNext = SampleWriteInterval;
			}
		}
		sleep(2);
	};
}

bool Monitor::ReadInverterStats(bool saveReading) {
	string out;
	int    res = RunInverterQuery("QPIGS", out);
	if (res != 0) {
		fprintf(stderr, "Failed to run inverter query. Exit code = %d\n", res);
		return false;
	}
	Record r;
	r.Heavy = IsHeavyOnInverter;
	if (MakeRecord(TrimSpace(out), r)) {
		if (saveReading)
			Records.push_back(r);
		UpdateStats(r);
		return true;
	} else {
		//printf("read: %s\n", inp.c_str());
		return false;
	}
}

void Monitor::UpdateStats(const Record& r) {
	IsInitialized = true;
	IsOverloaded  = r.LoadW > (float) OverloadThresholdWatts;
	HasGridPower  = r.ACInV > (float) GridVoltageThreshold;
	SolarV        = (int) r.PvV;
	BatteryV      = r.BatV;

	AddToHistory(SolarVHistorySize, SolarVHistory, r.PvV);
	AvgSolarV = Average(SolarVHistory);

	AddToHistory(BatteryModeHistorySize, LoadWHistory, r.LoadW);
	AddToHistory(BatteryModeHistorySize, PvWHistory, r.PvW);
	MaxLoadW = (int) Maximum(LoadWHistory);
	AvgLoadW = (int) Average(LoadWHistory, 5);

	//ComputePVStrength();
}

// What we're looking for here, is a situation where the PvW is consistently
// lower than the LoadW. However, some caveats apply. In particular, if the LoadW
// is lower than about 300W, then the PvW will often be very low (eg 20 W).
// I don't know why this is, but we need to account for it. So basically
// what we're looking for here is a situation where we're drawing at least
// 300W, and the PvW is substantially lower than the load.
// The above comments apply when the system is in SUB mode.
// However, when we're in SBU mode, then the readings seem to be more accurate,
// and the PvW tracks the LoadW much better. So when we're in SBU mode, then
// we trust the numbers more, and we reduce the thresholds.
// SCREW IT -- this doesn't work reliably.
/*
void Monitor::ComputePVStrength() {
	int  minValidSamples = 10;
	bool debug           = false;
	if (AvgSolarV == 0) {
		if (debug)
			fprintf(stderr, "AvgSolarV = 0\n");
		PVIsTooWeakForLoads = true;
		return;
	}
	if (LoadWHistory.size() < (int) minValidSamples) {
		// not enough information
		if (debug)
			fprintf(stderr, "Only %d samples in LoadWHistory\n", (int) LoadWHistory.size());
		return;
	}
	if (LoadWHistory.size() != PvWHistory.size()) {
		fprintf(stderr, "Expected LoadWHistory to be same size as PvWHistory\n");
		return;
	}
	int nValid   = 0;
	int nTooWeak = 0;
	for (size_t i = LoadWHistory.size() - 1; i != -1 && nValid < minValidSamples; i--) {
		if (PvWHistory[i] == 0) {
			// zero PvW is a definite signal that we're not producing anything
			nTooWeak++;
			nValid++;
		} else {
			if (CurrentPowerSource == PowerSource::SBU) {
				// In this case, our readings are accurate, and if the PvW is less than the LoadW,
				// then we're not meeting our needs.
				if (LoadWHistory[i] - PvWHistory[i] > 100)
					nTooWeak++;
				nValid++;
			} else {
				// In this case, our readings are inaccurate, and the system often seems to generate
				// less PvW than is needed.
				if (LoadWHistory[i] < 200) {
					// Below 200, we definitely can't see much
					nValid++;
				} else if (LoadWHistory[i] > 300) {
					if (LoadWHistory[i] - PvWHistory[i] > 100)
						nTooWeak++;
					nValid++;
				}
			}
		}
	}
	if (nValid < minValidSamples) {
		// not enough information
		if (debug)
			fprintf(stderr, "Only %d valid samples in load history (need at least %d)\n", nValid, minValidSamples);
		return;
	}
	double pTooWeak  = (double) nTooWeak / (double) nValid;
	bool   isTooWeak = pTooWeak > 0.5;
	if (isTooWeak != PVIsTooWeakForLoads) {
		fprintf(stderr, "Changing PVIsTooWeakForLoads to %s. nTooWeak: %d, nValid: %d\n", isTooWeak ? "true" : "false", nTooWeak, nValid);
	}
	if (debug)
		fprintf(stderr, "isTooWeak: %s, nTooWeak: %d, nValid: %d\n", isTooWeak ? "true" : "false", nTooWeak, nValid);
	PVIsTooWeakForLoads = isTooWeak;
}
*/

static double GetDbl(const nlohmann::json& j, const char* key) {
	if (j.find(key) == j.end())
		return 0;
	return j[key].get<double>();
}

bool Monitor::MakeRecord(std::string inp, Record& r) {
	//printf("inverter output: [%s]\n", inp.c_str());
	try {
		auto j     = nlohmann::json::parse(inp);
		r.Time     = time(nullptr);
		r.ACInV    = GetDbl(j, "ACInV");
		r.ACInHz   = GetDbl(j, "ACInHz");
		r.ACOutV   = GetDbl(j, "ACOutV");
		r.ACOutHz  = GetDbl(j, "ACOutHz");
		r.LoadW    = GetDbl(j, "LoadW");
		r.LoadVA   = GetDbl(j, "LoadVA");
		r.LoadP    = GetDbl(j, "LoadP");
		r.BatChA   = GetDbl(j, "BatChA");
		r.BatV     = GetDbl(j, "BatV");
		r.BatP     = GetDbl(j, "BatP");
		r.Temp     = GetDbl(j, "Temp");
		r.PvV      = GetDbl(j, "PvV");
		r.PvA      = GetDbl(j, "PvA");
		r.PvW      = GetDbl(j, "PvW");
		r.Unknown1 = GetDbl(j, "Unknown1");
		return true;
	} catch (nlohmann::json::exception& e) {
		printf("Failed to decode JSON record [%s]\n", inp.c_str());
		return false;
	}
	return true;
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

bool Monitor::CommitReadings() {
	string sql;
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
	for (size_t i = 0; i < Records.size(); i++) {
		const auto& r = Records[i];
		sql += "(";
		sql += "to_timestamp(";
		AddDbl(sql, r.Time, false);
		sql += ") AT TIME ZONE 'UTC',";
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
		if (i != Records.size() - 1)
			sql += ",";
	}
	//printf("Send:\n%s\n", sql.c_str());
	string cmd = "PGPASSWORD=homepower psql --host localhost --username pi --dbname power --command \"" + sql + "\"";
	return system(cmd.c_str()) == 0;
}

std::string Monitor::ProcessPath() {
	char buf[2048];
	buf[0] = 0;
	int r  = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (r < 0)
		return buf;

	if (r < sizeof(buf))
		buf[r] = 0;
	else
		buf[sizeof(buf) - 1] = 0;
	return buf;
}

} // namespace homepower