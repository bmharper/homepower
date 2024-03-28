#pragma once

#include <algorithm>
#include <time.h>

namespace homepower {

template <typename T>
T Clamp(T v, T vmin, T vmax) {
	if (v < vmin)
		return vmin;
	if (v > vmax)
		return vmax;
	return v;
}

struct TimePoint {
	int Hour   = 0;
	int Minute = 0;
	TimePoint(int hour = 0, int min = 0) : Hour(hour), Minute(min) {}
	bool operator<(const TimePoint& b) const {
		return AbsoluteMinute() < b.AbsoluteMinute();
	}
	bool operator<=(const TimePoint& b) const {
		return AbsoluteMinute() <= b.AbsoluteMinute();
	}
	bool operator>(const TimePoint& b) const {
		return AbsoluteMinute() > b.AbsoluteMinute();
	}
	bool operator>=(const TimePoint& b) const {
		return AbsoluteMinute() >= b.AbsoluteMinute();
	}
	bool operator==(const TimePoint& b) const {
		return AbsoluteMinute() == b.AbsoluteMinute();
	}
	bool operator!=(const TimePoint& b) const {
		return !(*this == b);
	}

	int AbsoluteMinute() const {
		return Hour * 60 + Minute;
	}

	// Interpolate between the n values aV, at times aT, to find the value at time t.
	// Hour values are between 0 and 23, and Minute values are between 0 and 59.
	// We need to respect the wrap-around at midnight.
	static float Interpolate(TimePoint t, int n, TimePoint* aT, float* aV) {
		if (n == 0)
			return 0;
		if (n == 1)
			return aV[0];
		int i = 0;
		for (; i < n; i++) {
			if (t < aT[i])
				break;
		}
		if (i >= 1 && i < n) {
			// Regular interpolation within the array
			float t1    = (float) aT[i - 1].AbsoluteMinute();
			float t2    = (float) aT[i].AbsoluteMinute();
			float alpha = (float) (t.AbsoluteMinute() - t1) / (t2 - t1);
			return aV[i - 1] * (1 - alpha) + aV[i] * alpha;
		} else {
			// 24-hour wraparound
			float     t1    = (float) aT[n - 1].AbsoluteMinute();
			float     t2    = (float) aT[0].AbsoluteMinute() + 24 * 60;
			float     v1    = aV[n - 1];
			float     v2    = aV[0];
			TimePoint normT = t;
			if (i == 0) {
				// In this case t is before aT[0].
				// Here we normalize it so that it's after aT[n-1].
				normT.Hour += 24;
			}
			float alpha = (float) (normT.AbsoluteMinute() - t1) / (t2 - t1);
			return v1 * (1 - alpha) + v2 * alpha;
		}
	}

	static TimePoint Now(int timezoneOffsetMinutes) {
		auto t = time(nullptr);
		tm   tt;
		gmtime_r(&t, &tt);
		int sec = tt.tm_hour * 3600 + tt.tm_min * 60 + tt.tm_sec;
		sec += timezoneOffsetMinutes * 60;
		if (sec < 0)
			sec += 24 * 3600;
		sec = sec % (24 * 3600);
		TimePoint tp;
		tp.Hour   = Clamp(sec / 3600, 0, 23);
		tp.Minute = Clamp((sec - tp.Hour * 3600) / 60, 0, 59);
		return tp;
	}
};

// Cooloff represents a time period that doubles every time we make
// an incorrect decision.
// A key reason why this exists is because we have imperfect knowledge.
// We don't know how much power is going to be used by
// the optional circuits until we flip them on.
// For definitions, we think of a conservative state and an optimistic state.
// When we think all is quiet, then we switch to the optimistic state. Only after doing that,
// can we detect that we might have been wrong. Every time we're forced to switch
// from our optimistic state back to our conservative state, we double the cooloff period.
struct Cooloff {
	time_t DefaultCooloffPeriod = 2 * 6;   // When we think things are stable, then cooloff period returns to Default
	time_t CooloffPeriod        = 2 * 60;  // Current backoff time
	time_t MaxCooloffPeriod     = 15 * 60; // Maximum backoff time
	time_t LastAlarm            = 0;       // Last time that we needed to switch to the conservative state

	// Inform the system that everything appears to be fine
	void SignalFine(time_t now) {
		if (now - LastAlarm > CooloffPeriod * 2) {
			// Since there's no alarm, and our last switch was more than CooloffPeriod*2 ago,
			// we know that we've been in the desiredState for long enough to reset our backoff period.
			CooloffPeriod = DefaultCooloffPeriod;
		}
	}

	// Inform the system that we've needed to switch back to the conservative state (i.e. our optimism was wrong)
	void SignalAlarm(time_t now) {
		LastAlarm     = now;
		CooloffPeriod = std::min(CooloffPeriod * 2, MaxCooloffPeriod);
	}

	// IsGood returns true if we're out of the alarm period
	bool IsGood(time_t now) const {
		return now - LastAlarm > CooloffPeriod;
	}
};

} // namespace homepower