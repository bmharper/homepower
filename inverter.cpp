#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>

/*

QPIGS:



(000.0 00.0 228.2 50.0 0346 0337 011 429 27.00 000 095 0038 01.3 248.1 00.00 00001 10010000 00 00 00336 010
 ACInV      AcOutV     VA        Load%   BattV     Bat%     BattA?                                SolW      
       AcInHz     AcOutHz   LoadW    BusV      BatChgA          SolV


(000.0  00.0    228.2   50.0     0346    0337   011    429   27.00  000     095   0038  01.3  248.1  00.00  00001   10010000  00  00  00336       010
 AcInV  AcInHz  AcOutV  AcOutHz  LoadVA  LoadW  Load%  BusV  BatV   BatChA  Bat%  Temp  PvA   PvV                                     PvW 

*/

using namespace std;

// Max timeout I've seen in practice is 1.5 seconds, on a raspberry Pi 1
const double RecvTimeout = 2;

uint16_t CRC(const uint8_t* pin, size_t len) {
	uint16_t crc_ta[16] = {
	    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
	    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef};

	auto     ptr = pin;
	uint16_t crc = 0;

	while (len-- != 0) {
		uint8_t da = ((uint8_t)(crc >> 8)) >> 4;
		crc <<= 4;
		crc ^= crc_ta[da ^ (*ptr >> 4)];
		da = ((uint8_t)(crc >> 8)) >> 4;
		crc <<= 4;
		crc ^= crc_ta[da ^ (*ptr & 0x0f)];
		ptr++;
	}

	uint8_t bCRCLow  = crc;
	uint8_t bCRCHign = (uint8_t)(crc >> 8);

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

bool IsValidResponse(string& msg) {
	size_t len = msg.size();
	if (len < 4)
		return false;
	size_t i = len - 1;
	for (; i != -1 && msg[i] == 0; i--) {
	}
	if (i == -1 || i < 4)
		return false;

	len = i + 1;
	//printf("Testing length of %d\n", len);
	uint16_t crc  = CRC((const uint8_t*) msg.data(), len - 3);
	uint8_t  crc1 = crc >> 8;
	uint8_t  crc2 = crc & 0xff;
	if (crc1 == msg[len - 3] && crc2 == msg[len - 2]) {
		msg = msg.substr(0, len - 3);
		return true;
	}
	return false;
}

bool SendMsg(int fd, const string& raw) {
	string      msg    = FinishMsg(raw);
	const char* out    = msg.data();
	size_t      remain = msg.size();
	do {
		int n = write(fd, out, remain);
		if (n <= 0)
			return false;
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

bool RecvMsg(int fd, string& msg) {
	char buf[1024];
	auto start = GetTime();
	while (true) {
		int n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			msg.append((const char*) buf, n);
			if (IsValidResponse(msg))
				return true;
		}
		if (GetTime() - start > RecvTimeout)
			return false;
		usleep(100);
	}
}

// Interpret a known command, and return JSON representing it.
// Upon failure, return an empty string
string Interpret(string cmd, string resp) {
	if (cmd == "QPIGS") {
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
		double n[6]    = {0};
		int    tok     = sscanf(resp.c_str() + 1, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
                         &acInV, &acInHz, &acOutV, &acOutHz, &loadVA, &loadW, &loadP, &busV, &batV, &batChA, &batP, &temp, &pvA, &pvV, n + 0, n + 1, n + 2, n + 3, n + 4, &pvW, n + 5);
		if (tok == 21) {
				}
	}
	return "";
}

bool Execute(string device, string cmd) {
	int fd = open(device.c_str(), O_RDWR | O_NONBLOCK);
	if (fd == -1) {
		fprintf(stderr, "Unable to open device file '%s' (errno=%d %s)\n", device.c_str(), errno, strerror(errno));
		return false;
	}

	if (SendMsg(fd, cmd)) {
		string resp;
		if (RecvMsg(fd, resp)) {
			printf("%s\n", resp.c_str());
		} else {
			size_t len = resp.size();
			if (len > 10)
				fprintf(stderr, "Fail: %s\nLast 10 bytes: %d %d %d %d %d %d %d %d %d %d\n", resp.c_str(), resp[len - 10], resp[len - 9], resp[len - 8], resp[len - 7], resp[len - 6], resp[len - 5], resp[len - 4], resp[len - 3], resp[len - 2], resp[len - 1]);
			else
				fprintf(stderr, "Fail: %s\n", resp.c_str());
		}
	}

	close(fd);
	return true;
}

void ShowHelp() {
	printf("inverter <device> <cmd>\n");
	printf("  example device = /dev/hidraw0\n");
	printf("  example cmd    = QPIGS\n");
}

int main(int argc, char** argv) {
	if (argc < 3) {
		ShowHelp();
		return 1;
	}
	string device = argv[1];
	string cmd    = argv[2];
	Execute(device, cmd);
	return 0;
}
