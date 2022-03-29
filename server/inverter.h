#pragma once

#include <string>
#include "../json.hpp"

namespace homepower {

// Inverter talks to the Axpert/Voltronic inverter over RS232 or USB
// This was originally a standalone program, but opening and closing
// the serial port adds a lot of overhead.
// A single command/query execution took 1.57 seconds on a Raspberry Pi 4.
// After moving this functionality into a library that could keep the file
// handle open, a query takes about 0.08 seconds on a Raspberry Pi 4.
class Inverter {
public:
	// These are the process exit codes
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

		nlohmann::json ToJSON() const;
	};

	std::string Device      = "/dev/hidraw0"; // Name of device to open, such as /dev/hidraw0 or /dev/ttyUSB0
	int         FD          = -1;             // File handle for talking to inverter
	double      RecvTimeout = 2;              // Max timeout I've seen in practice is 1.5 seconds, on a raspberry Pi 1

	~Inverter();
	bool Open();
	void Close();
	// Execute functions will automatically Open() if necessary
	Response Execute(Record_QPIGS& response);
	Response Execute(std::string cmd, std::string& response);
	Response Execute(std::string cmd);

	template <typename ResponseRecord>
	Response ExecuteT(std::string cmd, ResponseRecord& response);

	bool Interpret(const std::string& resp, Record_QPIGS& out);

	static std::string DescribeResponse(Response r);

private:
	std::string RawToPrintable(const std::string& raw);
};

template <typename ResponseRecord>
inline Inverter::Response Inverter::ExecuteT(std::string cmd, ResponseRecord& response) {
	std::string r;
	auto        err = Execute(cmd, r);
	if (err != Inverter::Response::OK)
		return err;
	if (!Interpret(r, response)) {
		fprintf(stderr, "Don't understand response to %s: [%s]\n", cmd.c_str(), RawToPrintable(r).c_str());
		return Inverter::Response::DontUnderstand;
	}
	return Inverter::Response::OK;
}

} // namespace homepower