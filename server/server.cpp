#include "controller.h"
#include "string.h"
#include "monitor.h"
#include "http.h"
#include <unistd.h>
#include <sstream>

using namespace std;

bool equals(const char* a, const char* b) {
	return strcmp(a, b) == 0;
}

vector<string> split(const string& s, char delim) {
	vector<string> elems;
	stringstream   ss(s);
	string         item;
	while (getline(ss, item, delim)) {
		elems.push_back(item);
	}
	return elems;
}

string join(const vector<string>& a, string delim) {
	string s;
	for (size_t i = 0; i < a.size(); i++) {
		if (i > 0) {
			s += delim;
		}
		s += a[i];
	}
	return s;
}

int main(int argc, char** argv) {
	bool runController    = false;
	bool enableAutoCharge = false;
	bool debug            = false;
	//bool               enablePowerSourceTimer     = false;
	bool               showHelp = false;
	homepower::Monitor monitor;
	int                defaultInverterWatts       = monitor.InverterSustainedW;
	int                defaultBatteryWattHours    = monitor.BatteryWh;
	int                defaultSampleWriteInterval = monitor.SampleWriteInterval;
	int                minBatterySOC              = homepower::Controller::DefaultMinBatterySOC;
	int                maxBatterySOC              = homepower::Controller::DefaultMaxBatterySOC;
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if (equals(arg, "-c")) {
			runController = true;
		} else if (equals(arg, "-?") || equals(arg, "-h") || equals(arg, "--help")) {
			showHelp = true;
		} else if (equals(arg, "-a")) {
			enableAutoCharge = true;
			//} else if (equals(arg, "-t")) {
			//	enablePowerSourceSwitching = true;
			//	enablePowerSourceTimer     = true;
		} else if (equals(arg, "-d")) {
			debug = true;
		} else if (i + 1 < argc && (equals(arg, "-i") || equals(arg, "--inv"))) {
			monitor.Inverter.Devices = split(argv[i + 1], ',');
			i++;
		} else if (i + 1 < argc && (equals(arg, "-o"))) {
			monitor.InverterSustainedW = atoi(argv[i + 1]);
			if (monitor.InverterSustainedW < 100 || monitor.InverterSustainedW > 30000) {
				fprintf(stderr, "Invalid inverter watts. Must be between 100 and 30000.\n");
				return 1;
			}
			i++;
		} else if (i + 1 < argc && (equals(arg, "-b"))) {
			monitor.BatteryWh = atoi(argv[i + 1]);
			if (monitor.BatteryWh < 1000 || monitor.BatteryWh > 50000) {
				fprintf(stderr, "Invalid battery watt hours. Must be between 1000 and 50000.\n");
				return 1;
			}
			i++;
		} else if (i + 1 < argc && (equals(arg, "-p"))) {
			auto parts = split(argv[i + 1], ':');
			if (parts.size() != 5) {
				fprintf(stderr, "Invalid Postgres specification. Must be in the form host:port:db:user:password\n");
				return 1;
			}
			monitor.PostgresHost     = parts[0];
			monitor.PostgresPort     = parts[1];
			monitor.PostgresDB       = parts[2];
			monitor.PostgresUsername = parts[3];
			monitor.PostgresPassword = parts[4];
			monitor.DBMode           = homepower::DBModes::Postgres;
			i++;
		} else if (i + 1 < argc && (equals(arg, "-l"))) {
			monitor.SQLiteFilename = argv[i + 1];
			monitor.DBMode         = homepower::DBModes::SQLite;
			i++;
		} else if (i + 1 < argc && (equals(arg, "--min"))) {
			minBatterySOC = atoi(argv[i + 1]);
			i++;
		} else if (i + 1 < argc && (equals(arg, "--max"))) {
			maxBatterySOC = atoi(argv[i + 1]);
			i++;
		} else if (i + 1 < argc && (equals(arg, "-s"))) {
			monitor.SampleWriteInterval = atoi(argv[i + 1]);
			if (monitor.SampleWriteInterval < 1 || monitor.SampleWriteInterval > 1000) {
				fprintf(stderr, "Invalid sample write interval. Must be between 1 and 1000.\n");
				return 1;
			}
			i++;
		} else if (i + 1 < argc && (equals(arg, "-u"))) {
			monitor.Inverter.UsbRestartScript = argv[i + 1];
			i++;
		} else {
			fprintf(stderr, "Unknown argument '%s'\n", arg);
			showHelp = true;
		}
	}

	if (minBatterySOC <= 0 || minBatterySOC >= 100) {
		fprintf(stderr, "Invalid min battery SOC '%d'. Valid values are 0 < SOC < 90\n", minBatterySOC);
		return 1;
	}
	if (maxBatterySOC <= minBatterySOC) {
		fprintf(stderr, "Invalid max battery SOC '%d'. Must be greater than min SOC (%d)\n", maxBatterySOC, minBatterySOC);
		return 1;
	}
	if (maxBatterySOC > 90) {
		// See justification in constructor of Controller class
		fprintf(stderr, "Invalid max battery SOC '%d'. Max 90\n", maxBatterySOC);
		return 1;
	}
	if (enableAutoCharge && !runController) {
		fprintf(stderr, "Auto battery charge is meaningless if the controller is not enabled\n");
		return 1;
	}

	if (showHelp) {
		fprintf(stderr, "server - Monitor Axpert/Voltronic inverter, and write stats to Postgres database\n");
		fprintf(stderr, " -c                Run controller, which switches heavy loads using GPIO pins 17 and 18\n");
		fprintf(stderr, " -a                Enable auto battery charge, switching between SBU and SUB\n");
		fprintf(stderr, " -d                Enable debug mode, which will not actually send any GPIO commands or\n");
		fprintf(stderr, "                   inverter state change commands. Used for debugging logic without\n");
		fprintf(stderr, "                   affecting a live system.\n");
		//fprintf(stderr, " -t                Enable source switching between SBU and SUB on timer (implies -s)\n");
		fprintf(stderr, " -o <watts>        Invert output power in watts. Default %d\n", defaultInverterWatts);
		fprintf(stderr, " -b <watt-hours>   Size of battery in watt-hours. Default %d\n", defaultBatteryWattHours);
		fprintf(stderr, " -i --inv <device> Specify inverter device communication channel\n");
		fprintf(stderr, "                   (eg /dev/hidraw0 for direct USB, or /dev/ttyUSB0 for RS232-to-USB adapter).\n");
		fprintf(stderr, "                   Multiple devices can be separated with commas (for redundancy),\n");
		fprintf(stderr, "                   eg /dev/hidraw0,/dev/ttyUSB0\n");
		fprintf(stderr, "                   Default device %s\n", join(monitor.Inverter.Devices, ",").c_str());
		fprintf(stderr, " -p <postgres>     Postgres connection string separated by colons host:port:db:user:password\n");
		fprintf(stderr, " -l <sqlite>       Sqlite DB filename (specify /dev/null as SQLite filename to disable any DB writes)\n");
		fprintf(stderr, " -s <samples>      Sample write interval. Can be raised to limit SSD writes. Default %d\n", defaultSampleWriteInterval);
		fprintf(stderr, " --min <soc>       Minimum battery SOC before charging from grid. Default %d\n", (int) homepower::Controller::DefaultMinBatterySOC);
		fprintf(stderr, " --max <soc>       Maximum expected battery SOC at end of day. Default %d\n", (int) homepower::Controller::DefaultMaxBatterySOC);
		fprintf(stderr, " -u <script>       Shell script to invoke if USB port seems to be dead\n");
		return 1;
	}

	if (debug) {
		// For example data, see the comment block below
		monitor.Inverter.DebugResponseFile = "/home/ben/tmp/qpigs.txt";
	}

	//homepower::Controller controller(&monitor, false, true);
	//controller.BakeChargeLimits();
	//controller.PrintChargeLimits();

	monitor.Start();
	bool ok = true;
	if (runController) {
		homepower::Controller controller(&monitor, !debug, !debug);
		controller.EnableAutoCharge  = enableAutoCharge;
		controller.MinCharge[0].Hard = minBatterySOC;
		controller.MinCharge[0].Soft = minBatterySOC + 10;
		controller.SetHeavyLoadState(homepower::HeavyLoadState::Grid);
		if (controller.Start()) {
			ok = homepower::RunHttpServer(controller);
			controller.Stop();
		} else {
			ok = false;
		}
	} else {
		// should ideally listen for SIGHUP or something
		while (true) {
			sleep(10);
		}
	}
	monitor.Stop();
	return ok ? 0 : 1;
}

/*
Example QPIGS output:

(235.1 50.1 229.7 50.0 0620 0574 011 381 50.90 032 082 0046 09.0 273.8 00.00 00000 00010010 00 00 02431 010

Interpreted:
{
    "ACInHz": 50.099998474121094,
    "ACInV": 235.10000610351563,
    "ACOutHz": 50.0,
    "ACOutV": 229.6999969482422,
    "BatChA": 32.0,
    "BatP": 82.0,
    "BatV": 50.900001525878906,
    "BusV": 381.0,
    "LoadP": 11.0,
    "LoadVA": 620.0,
    "LoadW": 574.0,
    "PvA": 9.0,
    "PvV": 273.79998779296875,
    "PvW": 2431.0,
    "Raw": "(235.1 50.1 229.7 50.0 0620 0574 011 381 50.90 032 082 0046 09.0 273.8 00.00 00000 00010010 00 00 02431 010",
    "Temp": 46.0,
    "Unknown1": 0.0,
    "Unknown2": "00000",
    "Unknown3": "00010010",
    "Unknown4": "00",
    "Unknown5": "00",
    "Unknown6": "010"
}		
*/
