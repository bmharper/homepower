#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <termios.h>
#include <time.h>
#include "inverter.h"

/*

QPIGS:



(000.0 00.0 228.2 50.0 0346 0337 011 429 27.00 000 095 0038 01.3 248.1 00.00 00001 10010000 00 00 00336 010
 ACInV      AcOutV     VA        Load%   BattV     Bat%     BattA?                                SolW      
       AcInHz     AcOutHz   LoadW    BusV      BatChgA          SolV


(000.0  00.0    228.2   50.0     0346    0337   011    429   27.00  000     095   0038  01.3  248.1  00.00  00001   10010000  00  00  00336       010
 AcInV  AcInHz  AcOutV  AcOutHz  LoadVA  LoadW  Load%  BusV  BatV   BatChA  Bat%  Temp  PvA   PvV                                     PvW 

*/

using namespace std;

namespace homepower {

const char* InverterModelDescribe(InverterModel v) {
	switch (v) {
	case InverterModel::Unknown: return "Unknown";
	case InverterModel::King_6200: return "King_6200";
	case InverterModel::MKS2_5600: return "MKS2_5600";
	}
	return "Unknown_Enum";
}

uint16_t CRC(const uint8_t* pin, size_t len) {
	uint16_t crc_ta[16] = {
	    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
	    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef};

	auto     ptr = pin;
	uint16_t crc = 0;

	while (len-- != 0) {
		uint8_t da = ((uint8_t) (crc >> 8)) >> 4;
		crc <<= 4;
		crc ^= crc_ta[da ^ (*ptr >> 4)];
		da = ((uint8_t) (crc >> 8)) >> 4;
		crc <<= 4;
		crc ^= crc_ta[da ^ (*ptr & 0x0f)];
		ptr++;
	}

	uint8_t bCRCLow  = crc;
	uint8_t bCRCHign = (uint8_t) (crc >> 8);

	if (bCRCLow == 0x28 || bCRCLow == 0x0d || bCRCLow == 0x0a)
		bCRCLow++;
	if (bCRCHign == 0x28 || bCRCHign == 0x0d || bCRCHign == 0x0a)
		bCRCHign++;

	crc = ((uint16_t) bCRCHign) << 8;
	crc += bCRCLow;
	return crc;
}

string FinishMsg(const string& raw) {
	string   b   = raw;
	uint16_t crc = CRC((const uint8_t*) raw.data(), raw.size());
	b += char(crc >> 8);
	b += char(crc & 0xff);
	b += char(0x0d);
	return b;
}

Inverter::Response ValidateResponse(string& msg) {
	size_t len = msg.size();
	if (len < 4)
		return Inverter::Response::FailRecvTooShort;
	size_t i = len - 1;
	for (; i != -1 && msg[i] == 0; i--) {
	}
	if (i == -1 || i < 4)
		return Inverter::Response::FailRecvTooShort;

	len = i + 1;
	//printf("Testing length of %d\n", len);
	uint16_t crc  = CRC((const uint8_t*) msg.data(), len - 3);
	uint8_t  crc1 = crc >> 8;
	uint8_t  crc2 = crc & 0xff;
	if (crc1 == msg[len - 3] && crc2 == msg[len - 2]) {
		msg = msg.substr(0, len - 3);
		return Inverter::Response::OK;
	}
	return Inverter::Response::FailRecvCRC;
}

void DumpMsg(const string& raw) {
	printf("Message: [");
	for (size_t i = 0; i < raw.size(); i++) {
		int c = raw[i];
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == ' ' || c == '(' || c == ')' || c == '.')
			printf("%c", c);
		else
			printf("%02X", c);
	}
	printf("]\n");
}

bool SendMsg(int fd, const string& raw) {
	string      msg    = FinishMsg(raw);
	const char* out    = msg.data();
	size_t      remain = msg.size();
	//DumpMsg(msg);
	do {
		int n = write(fd, out, remain);
		if (n <= 0)
			return false;
		//printf("wrote %d/%d bytes\n", n, (int) msg.size());
		out += n;
		remain -= n;
	} while (remain != 0);
	return true;
}

double GetTime() {
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

Inverter::Response RecvMsg(int fd, double timeout, string& msg) {
	char               buf[1024];
	auto               start   = GetTime();
	Inverter::Response lastErr = Inverter::Response::FailRecvTooShort;

	// I tried this pause, to try and work around sporadic failures on my MKS 4, but this pause didn't help
	//usleep(100000);

	while (true) {
		int n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			//printf("read %d bytes\n", n);
			msg.append((const char*) buf, n);
			lastErr = ValidateResponse(msg);
			if (lastErr == Inverter::Response::OK)
				return lastErr;
		}
		/*
		if (n == 0 && msg.size() > 100) {
			// This is a weird thing that I see very frequently on my 2022 Kodak MKS 4.
			// The first character is missing from the response.
			// OK... that was with my Raspberry Pi 4. I have no idea what that was...
			// Back to an original Raspberry Pi 1, and the problem is gone.
			auto test = msg;
			if (test[0] != '(') {
				printf("Added (\n");
				test = "(" + test;
			}
			if (test[test.length() - 1] == 0x0d) {
				printf("Chopped final 0x0d\n");
				test.erase(test.end() - 1);
			}
			if (ValidateResponse(test) == Inverter::Response::OK) {
				printf("Fixed!!\n");
				msg = test;
				return Inverter::Response::OK;
			}
		}
		*/
		if (GetTime() - start > timeout)
			return lastErr;
		usleep(20000);
	}
}

Inverter::~Inverter() {
	Close();
}

// Interpret a known command
bool Inverter::Interpret(const std::string& resp, Record_QPIGS& out) {
	// (000.0  00.0    228.2   50.0     0346    0337   011    429   27.00  000     095   0038  01.3  248.1  00.00  00001   10010000  00  00  00336       010
	//  AcInV  AcInHz  AcOutV  AcOutHz  LoadVA  LoadW  Load%  BusV  BatV   BatChA  Bat%  Temp  PvA   PvV                                     PvW
	double acInV   = 0;
	double acInHz  = 0;
	double acOutV  = 0;
	double acOutHz = 0;
	double loadVA  = 0;
	double loadW   = 0;
	double loadP   = 0;
	double busV    = 0;
	double batV    = 0;
	double batChA  = 0;
	double batP    = 0;
	double temp    = 0;
	double pvA     = 0;
	double pvV     = 0;
	double pvW     = 0;
	double n[1]    = {0};
	char   s[5][40];
	int    tok = sscanf(resp.c_str() + 1, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %s %s %s %s %lf %s",
	                    &acInV, &acInHz, &acOutV, &acOutHz, &loadVA, &loadW, &loadP, &busV, &batV, &batChA, &batP, &temp, &pvA, &pvV,
	                    n + 0, (char*) (s + 0), (char*) (s + 1), (char*) (s + 2), (char*) (s + 3), &pvW, (char*) (s + 4));
	if (tok == 21) {
		out.Raw      = resp;
		out.Time     = time(nullptr);
		out.ACInV    = acInV;
		out.ACInHz   = acInHz;
		out.ACOutV   = acOutV;
		out.ACOutHz  = acOutHz;
		out.LoadVA   = loadVA;
		out.LoadW    = loadW;
		out.LoadP    = loadP;
		out.BusV     = busV;
		out.BatP     = batP;
		out.BatChA   = batChA;
		out.BatV     = batV;
		out.Temp     = temp;
		out.PvA      = pvA;
		out.PvV      = pvV;
		out.PvW      = pvW;
		out.Unknown1 = n[0];
		out.Unknown2 = s[0];
		out.Unknown3 = s[1];
		out.Unknown4 = s[2];
		out.Unknown5 = s[3];
		out.Unknown6 = s[4];
		return true;
	}
	return false;
}

bool Inverter::Interpret(const std::string& resp, InverterModel& out) {
	// First character is "("
	auto name = resp.substr(1);
	if (name == "KING-6200")
		out = InverterModel::King_6200;
	else if (name == "MKS2-5600")
		out = InverterModel::MKS2_5600;
	else
		return false;

	return true;
}

bool Inverter::Open() {
	if (DebugResponseFile != "")
		return true;

	Close();

	if (Devices.size() > 1)
		CurrentDevice = (CurrentDevice + 1) % Devices.size();

	if (CurrentDevice < 0)
		CurrentDevice = 0;

	auto device = Devices[CurrentDevice];

	FD = open(device.c_str(), O_RDWR | O_NONBLOCK);
	if (FD == -1) {
		// Reduce spam by only emitting the error if it's different from previous
		if (errno != LastOpenFailErr) {
			LastOpenFailErr = errno;
			fprintf(stderr, "Unable to open device file '%s' (errno=%d %s)\n", device.c_str(), errno, strerror(errno));
		}
		if (errno == ENOENT) {
			// I get this error when the USB port is dead (I believe it's my inverter that's at fault). A power down + power up
			// of the USB port solves this. We allow the user to specify an arbitrary shell script that we execute in this condition.
			RestartUsbAuto();
		}
		return false;
	}

	UsbRestartFailCount = 0;
	LastOpenFailErr     = 0;

	// If this looks like an RS232-to-USB adapter, then set the serial port parameters
	if (device.find("ttyUSB") != -1) {
		// 2400 is the only speed that seems to work
		speed_t baud = B2400;

		// Speed settings (in this case, 2400 8N1)
		struct termios settings;
		int            r = 0;
		if ((r = tcgetattr(FD, &settings)) != 0) {
			fprintf(stderr, "tcgetattr failed with %d\n", r);
			Close();
			return false;
		}

		// baud rate
		r = cfsetospeed(&settings, baud);
		if ((r = cfsetospeed(&settings, baud)) != 0) {
			fprintf(stderr, "cfsetospeed failed with %d\n", r);
			Close();
			return false;
		}
		cfmakeraw(&settings);        // It's vital to set this to RAW mode (instead of LINE)
		settings.c_cflag &= ~PARENB; // no parity
		settings.c_cflag &= ~CSTOPB; // 1 stop bit
		settings.c_cflag &= ~CSIZE;
		settings.c_cflag |= CS8 | CLOCAL; // 8 bits
		// settings.c_lflag = ICANON;         // canonical mode
		settings.c_oflag &= ~OPOST; // remove post-processing

		if ((r = tcsetattr(FD, TCSANOW, &settings)) != 0) {
			fprintf(stderr, "tcsetattr failed with %d\n", r);
			Close();
			return false;
		}
		tcflush(FD, TCOFLUSH);
		//tcdrain(fd);
	}
	return true;
}

void Inverter::Close() {
	if (FD == -1)
		return;
	close(FD);
	FD = -1;
}

Inverter::Response Inverter::Execute(Record_QPIGS& response, int maxRetries) {
	return ExecuteT("QPIGS", response, maxRetries);
}

Inverter::Response Inverter::Execute(string cmd, int maxRetries) {
	string resp;
	return Execute(cmd, resp, maxRetries);
}

Inverter::Response Inverter::Execute(string cmd, std::string& response, int maxRetries) {
	if (DebugResponseFile != "") {
		FILE* f = fopen(DebugResponseFile.c_str(), "rb");
		if (!f) {
			fprintf(stderr, "Inverter::Execute() returning FailOpenFile because DebugResponseFile is '%s'\n", DebugResponseFile.c_str());
			response = "Failed to open debug file " + DebugResponseFile;
			return Response::FailOpenFile;
		}
		char buf[1024];
		while (true) {
			size_t n = fread(buf, 1, 1024, f);
			response.append(buf, n);
			if (n < 1024)
				break;
		}
		fclose(f);
		return Response::OK;
	}

	auto res = Response::DontUnderstand;
	for (int retry = 0; retry <= maxRetries; retry++) {
		response = "";

		if (retry != 0)
			usleep(100000);

		if (FD == -1) {
			if (!Open()) {
				// Don't log an error here, because Open() will already emit a more specific error.
				res = Response::FailOpenFile;
				continue;
			}
		}

		res = Response::DontUnderstand;
		if (SendMsg(FD, cmd)) {
			res = RecvMsg(FD, RecvTimeout, response);
			if (res == Response::OK) {
				if (response == "(ACK") {
					res = Response::OK;
					break;
				} else if (response == "(NAK") {
					fprintf(stderr, "NAK (Not Acknowledged). Either this command is unrecognized, or this is likely a CRC failure, so something wrong with the COM port or BAUD rate, etc\n");
					res = Response::NAK;
					break;
				} else {
					res = Response::OK;
					break;
				}
			} else {
				fprintf(stderr, "RecvMsg Fail '%s' (%d): [%s]\n", DescribeResponse(res).c_str(), (int) response.size(), RawToPrintable(response).c_str());
				// re-open port
				Close();
			}
		} else {
			// re-open port
			Close();
			res = Response::FailWriteFile;
		}
	}

	return res;
}

string Inverter::RawToPrintable(const string& raw) {
	string r;
	for (size_t i = 0; i < raw.size(); i++) {
		unsigned c = (unsigned char) raw[i];
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == ' ' ||
		    c == '-' ||
		    c == '_' ||
		    c == '(' ||
		    c == ')' ||
		    c == '.') {
			r += raw[i];
		} else {
			char buf[4];
			sprintf(buf, ".%02X", c);
			r += buf;
		}
	}
	return r;
}

std::string Inverter::DescribeResponse(Response r) {
	// SYNC-RESPONSE-CODES
	switch (r) {
	case Response::OK: return "OK";
	case Response::InvalidCommand: return "InvalidCommand";
	case Response::FailOpenFile: return "FailOpenFile";
	case Response::FailRecvCRC: return "FailRecvCRC";
	case Response::FailRecvTooShort: return "FailRecvTooShort";
	case Response::FailWriteFile: return "FailWriteFile";
	case Response::DontUnderstand: return "DontUnderstand";
	case Response::NAK: return "NAK";
	};
	return "Unknown";
}

void Inverter::RestartUsbAuto() {
	if (UsbRestartScript == "")
		return;

	// max interval of 256 seconds
	int delayShift = std::min(UsbRestartFailCount, 8);
	if (time(nullptr) - LastUsbRestartAt < (1 << delayShift))
		return;

	LastUsbRestartAt    = time(nullptr);
	UsbRestartFailCount = std::min(UsbRestartFailCount + 1, 10000);

	fprintf(stderr, "Restarting USB port with script '%s'\n", UsbRestartScript.c_str());
	int res = system(UsbRestartScript.c_str());
	if (res != 0)
		fprintf(stderr, "USB restart failed with exit code %d\n", res);
}

} // namespace homepower