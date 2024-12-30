#pragma once

#include <time.h>
#include <stdint.h>
#include <float.h>
#include <algorithm>

#include "ringbuffer.h"

namespace homepower {

struct History {
	time_t Time;
	float  Value;
};

// Return the average value from the history buffer, going no further back than afterTime
inline double Average(time_t afterTime, const RingBuffer<History>& history) {
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

// Returns the time of the oldest sample in the buffer, or 0 if empty
inline time_t OldestTime(const RingBuffer<History>& history) {
	if (history.Size() == 0)
		return 0;
	return history.Peek(0).Time;
}

// Return the average value from the history buffer, in the time range minTime to maxTime
inline double Average(time_t minTime, time_t maxTime, const RingBuffer<History>& history) {
	double   sum      = 0;
	unsigned nsamples = 0;
	uint32_t idx      = history.Size() - 1;
	while (true) {
		if (idx == -1)
			break;
		auto sample = history.Peek(idx);
		if (sample.Time < minTime)
			break;
		if (sample.Time < maxTime) {
			sum += sample.Value;
			nsamples++;
		}
		idx--;
	}
	return nsamples == 0 ? 0 : sum / (double) nsamples;
}

inline float Minimum(time_t afterTime, const RingBuffer<History>& history) {
	float    minv = FLT_MAX;
	uint32_t idx  = history.Size() - 1;
	while (true) {
		if (idx == -1)
			break;
		auto sample = history.Peek(idx);
		if (sample.Time < afterTime)
			break;
		minv = std::min(minv, sample.Value);
		idx--;
	}
	return minv;
}

inline float Maximum(time_t afterTime, const RingBuffer<History>& history) {
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

} // namespace homepower
