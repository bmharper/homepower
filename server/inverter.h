#pragma once

#include <vector>
#include <string>

namespace homepower {

enum class InverterModel {
	Unknown,
	King_6200,
	MKS2_5600,
};

const char* InverterModelDescribe(InverterModel v);

// Inverter talks to the Axpert/Voltronic inverter over RS232 or USB
// This was originally a standalone program, but opening and closing
// the serial port adds a lot of overhead.
// A single command/query execution took 1.57 seconds on a Raspberry Pi 4.
// After moving this functionality into a library that could keep the file
// handle open, a query takes about 0.08 seconds on a Raspberry Pi 4.
class Inverter {
public:
	// These are the process exit codes
	// SYNC-RESPONSE-CODES
	enum class Response {
		OK               = 0,
		InvalidCommand   = 1,
		FailOpenFile     = 2,
		FailRecvCRC      = 3,
		FailRecvTooShort = 4,
		FailWriteFile    = 5,
		DontUnderstand   = 6,
		NAK              = 7,
	};

	struct Record_QPIGS {
		time_t      Time;
		std::string Raw;
		float       ACInV;
		float       ACInHz;
		float       ACOutV;
		float       ACOutHz;
		float       LoadVA;
		float       LoadW;
		float       LoadP;
		float       BatP;
		float       BatChA;
		float       BusV;
		float       BatV;
		float       Temp;
		float       PvA;
		float       PvV;
		float       PvW;
		float       Unknown1; // Similar to PvW on Bernie's inverter
		std::string Unknown2;
		std::string Unknown3;
		std::string Unknown4;
		std::string Unknown5;
		std::string Unknown6;
		bool        Heavy;
	};

	std::vector<std::string> Devices           = {"/dev/hidraw0"}; // Name of devices to use, such as /dev/hidraw0 or /dev/ttyUSB0. Multiple can be specified for redundancy.
	int                      CurrentDevice     = -1;               // Counter that increments through Devices
	int                      FD                = -1;               // File handle for talking to inverter
	double                   RecvTimeout       = 2;                // Max timeout I've seen in practice is 1.5 seconds, on a raspberry Pi 1
	std::string              DebugResponseFile = "";               // If not empty, then we don't actually talk to inverter, but read QPIGS response from this text file (this is for debugging/developing offline)
	std::string              UsbRestartScript  = "";               // Script that is invoked when USB port seems to be dead

	~Inverter();
	bool Open();
	void Close();

	// Execute functions will automatically Open() if necessary
	Response Execute(Record_QPIGS& response, int maxRetries);
	Response Execute(std::string cmd, std::string& response, int maxRetries);
	Response Execute(std::string cmd, int maxRetries);

	template <typename ResponseType>
	Response ExecuteT(std::string cmd, ResponseType& response, int maxRetries);

	bool Interpret(const std::string& resp, Record_QPIGS& out);
	bool Interpret(const std::string& resp, InverterModel& out);

	static std::string DescribeResponse(Response r);

private:
	int    LastOpenFailErr     = 0;
	int    UsbRestartFailCount = 0; // Number of times that USB restart script has failed
	time_t LastUsbRestartAt    = 0;

	std::string RawToPrintable(const std::string& raw);
	void        RestartUsbAuto();
};

template <typename ResponseType>
inline Inverter::Response Inverter::ExecuteT(std::string cmd, ResponseType& response, int maxRetries) {
	std::string r;
	auto        err = Execute(cmd, r, maxRetries);
	if (err != Inverter::Response::OK)
		return err;

	// First character in response is always "(".
	// "Interpret" functions assume length is at least 1 character long.
	if (r.length() < 2) {
		fprintf(stderr, "Response to %s is too short: [%s]\n", cmd.c_str(), RawToPrintable(r).c_str());
		return Inverter::Response::FailRecvTooShort;
	}

	if (!Interpret(r, response)) {
		fprintf(stderr, "Don't understand response to %s: [%s]\n", cmd.c_str(), RawToPrintable(r).c_str());
		return Inverter::Response::DontUnderstand;
	}
	return Inverter::Response::OK;
}

} // namespace homepower