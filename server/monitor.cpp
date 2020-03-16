#include "monitor.h"
#include <unistd.h>
#include <stdio.h>
#include "../json.hpp"

using namespace std;

namespace homepower {

Monitor::Monitor() {
	IsInitialized     = false;
	IsOverloaded      = false;
	HasGridPower      = true;
	SolarV            = 0;
	MustExit          = false;
	IsHeavyOnInverter = false;
	RecordNext        = 0; // Send one sample as soon as we come online
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

void Monitor::Run() {
	auto lastSaveTime = 0;
	while (!MustExit) {
		bool saveReading = time(nullptr) - lastSaveTime > SecondsBetweenSamples;
		bool readOK      = ReadInverter(saveReading);
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

// trim space from end
static string TrimSpace(const string& s) {
	string x = s;
	while (x.size() != 0 && (x[x.size() - 1] == '\n' || x[x.size() - 1] == ' ' || x[x.size() - 1] == '\t')) {
		x.erase(x.end() - 1, x.end());
	}
	return x;
}

bool Monitor::ReadInverter(bool saveReading) {
	string cmd = InverterPath + " /dev/hidraw0 QPIGS";
	auto   fd  = popen(cmd.c_str(), "r");
	if (!fd) {
		printf("Failed to call inverter: %d\n", errno);
		return false;
	}
	int    start = time(nullptr);
	string inp;
	while (time(nullptr) - start < 3) {
		char buf[3000];
		int  n = fread(buf, 1, sizeof(buf) - 1, fd);
		if (n > 0) {
			inp.append(buf, n);
			auto trimmed = TrimSpace(inp);
			if (trimmed.size() > 5 && trimmed[trimmed.size() - 1] == '}')
				break;
		}
	}
	pclose(fd);
	Record r;
	r.Heavy = IsHeavyOnInverter;
	if (MakeRecord(TrimSpace(inp), r)) {
		if (saveReading)
			Records.push_back(r);
		IsInitialized = true;
		IsOverloaded  = r.LoadW > (float) OverloadThresholdWatts;
		HasGridPower  = r.ACInV > (float) GridVoltageThreshold;
		SolarV        = (int) r.PvV;
		return true;
	} else {
		//printf("read: %s\n", inp.c_str());
		return false;
	}
}

static double GetDbl(const nlohmann::json& j, const char* key) {
	if (j.find(key) == j.end())
		return 0;
	return j[key].get<double>();
}

bool Monitor::MakeRecord(std::string inp, Record& r) {
	//printf("inverter output: [%s]\n", inp.c_str());
	try {
		auto j    = nlohmann::json::parse(inp);
		r.Time    = time(nullptr);
		r.ACInV   = GetDbl(j, "ACInV");
		r.ACInHz  = GetDbl(j, "ACInHz");
		r.ACOutV  = GetDbl(j, "ACOutV");
		r.ACOutHz = GetDbl(j, "ACOutHz");
		r.LoadW   = GetDbl(j, "LoadW");
		r.BatChA  = GetDbl(j, "BatChA");
		r.BatV    = GetDbl(j, "BatV");
		r.Temp    = GetDbl(j, "Temp");
		r.PvV     = GetDbl(j, "PvV");
		r.PvW     = GetDbl(j, "PvW");
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
	sql += "batChA,";
	sql += "batV,";
	sql += "temp,";
	sql += "pvV,";
	sql += "pvW,";
	sql += "heavy";
	sql += ") VALUES ";
	for (size_t i = 0; i < Records.size(); i++) {
		const auto& r = Records[i];
		sql += "(";
		sql += "to_timestamp(";
		AddDbl(sql, r.Time, false);
		sql += "),";
		AddDbl(sql, r.ACInV);
		AddDbl(sql, r.ACInHz);
		AddDbl(sql, r.ACOutV);
		AddDbl(sql, r.ACOutHz);
		AddDbl(sql, r.LoadW);
		AddDbl(sql, r.BatChA);
		AddDbl(sql, r.BatV);
		AddDbl(sql, r.Temp);
		AddDbl(sql, r.PvV);
		AddDbl(sql, r.PvW);
		AddBool(sql, r.Heavy, false);
		sql += ")";
		if (i != Records.size() - 1)
			sql += ",";
	}
	//printf("Send:\n%s\n", sql.c_str());
	string cmd = "PGPASSWORD=homepower psql --host localhost --username pi --dbname power --command \"" + sql + "\"";
	return system(cmd.c_str()) == 0;
}

} // namespace homepower