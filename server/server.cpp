#include "controller.h"
#include "string.h"
#include "monitor.h"
#include "http.h"
#include <unistd.h>
#include <sstream>

bool equals(const char* a, const char* b) {
	return strcmp(a, b) == 0;
}

std::vector<std::string> split(const std::string& s, char delim) {
	std::vector<std::string> elems;
	std::stringstream        ss(s);
	std::string              item;
	while (std::getline(ss, item, delim)) {
		elems.push_back(item);
	}
	return elems;
}

int main(int argc, char** argv) {
	bool runController    = false;
	bool enableAutoCharge = false;
	//bool               enablePowerSourceTimer     = false;
	bool               showHelp = false;
	homepower::Monitor monitor;
	int                defaultInverterWatts    = monitor.InverterSustainedW;
	int                defaultBatteryWattHours = monitor.BatteryWh;
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
		} else if (i + 1 < argc && (equals(arg, "-i") || equals(arg, "--inv"))) {
			monitor.Inverter.Device = argv[i + 1];
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
		} else {
			fprintf(stderr, "Unknown argument '%s'\n", arg);
			showHelp = true;
		}
	}

	if (showHelp) {
		fprintf(stderr, "server - Monitor Axpert/Voltronic inverter, and write stats to Postgres database\n");
		fprintf(stderr, " -c                Run controller, which switches heavy loads using GPIO pins 17 and 18\n");
		fprintf(stderr, " -a                Enable auto battery charge, switching between SBU and SUB\n");
		//fprintf(stderr, " -t                Enable source switching between SBU and SUB on timer (implies -s)\n");
		fprintf(stderr, " -o <watts>        Invert output power in watts. Default %d\n", defaultInverterWatts);
		fprintf(stderr, " -b <watt-hours>   Size of battery in watt-hours. Default %d\n", defaultBatteryWattHours);
		fprintf(stderr, " -i --inv <device> Specify inverter device communication channel\n");
		fprintf(stderr, "                   (eg /dev/hidraw0 for direct USB, or /dev/ttyUSB0\n");
		fprintf(stderr, "                   for RS232-to-USB adapter). Default %s\n", monitor.Inverter.Device.c_str());
		fprintf(stderr, " -p <postgres>     Postgres connection string separated by colons host:port:db:user:password\n");
		fprintf(stderr, " -l <sqlite>       Sqlite DB\n");
		return 1;
	}

	// The following line is useful when developing offline
	// Also, uncomment lines at the top of makefile, to enable debug info.
	bool debug = true;

	if (debug)
		monitor.Inverter.DebugResponseFile = "/home/ben/tmp/qpigs.txt";

	monitor.Start();
	bool ok = true;
	if (runController) {
		homepower::Controller controller(&monitor, !debug);
		controller.EnableAutoCharge = enableAutoCharge;
		//controller.EnablePowerSourceTimer  = enablePowerSourceTimer;
		controller.SetHeavyLoadMode(homepower::HeavyLoadMode::Grid);
		controller.Start();
		ok = homepower::RunHttpServer(controller);
		controller.Stop();
	} else {
		// should ideally listen for SIGHUP or something
		while (true) {
			sleep(10);
		}
	}
	monitor.Stop();
	return ok ? 0 : 1;
}