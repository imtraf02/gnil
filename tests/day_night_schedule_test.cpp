#include "config/night_light_config.h"
#include "system/day_night_schedule.h"

#include <chrono>
#include <print>
#include <string_view>

namespace {
  int failures = 0;

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "day_night_schedule_test: FAIL: {}", message);
      ++failures;
    }
  }
}

int main() {
  using namespace day_night_schedule;

  expect(normalizedClock("00:00") == "00:00", "midnight should parse");
  expect(normalizedClock("23:59") == "23:59", "last minute should parse");
  expect(!normalizedClock("24:00").has_value(), "hour 24 should be rejected");
  expect(!normalizedClock("7:00").has_value(), "non-padded clocks should be rejected");

  const auto beforeSunset = evaluateCustomAtLocalTime("20:30", "07:00", 20 * 60 + 29);
  const auto atSunset = evaluateCustomAtLocalTime("20:30", "07:00", 20 * 60 + 30);
  const auto beforeSunrise = evaluateCustomAtLocalTime("20:30", "07:00", 6 * 60 + 59);
  const auto atSunrise = evaluateCustomAtLocalTime("20:30", "07:00", 7 * 60);
  expect(beforeSunset.has_value() && !beforeSunset->night, "minute before sunset should remain day");
  expect(atSunset.has_value() && atSunset->night, "sunset boundary should enter night");
  expect(beforeSunrise.has_value() && beforeSunrise->night, "minute before sunrise should remain night");
  expect(atSunrise.has_value() && !atSunrise->night, "sunrise boundary should enter day");
  expect(
      atSunset.has_value() && atSunset->untilBoundary == std::chrono::hours(10) + std::chrono::minutes(30),
      "overnight boundary duration should cross midnight correctly"
  );
  expect(
      !evaluateCustomAtLocalTime("20:30", "20:30", 21 * 60).has_value(),
      "equal sunset and sunrise must not create an all-day schedule"
  );

  LocationConfig custom;
  custom.customSchedule = true;
  custom.sunset = "20:30";
  custom.sunrise = "20:30";
  custom.latitude = 10.0;
  custom.longitude = 106.0;
  expect(!hasUsableCustomTimes(custom), "equal custom boundaries should be unusable");
  expect(
      !resolveScheduleTimes(custom, std::nullopt, std::nullopt).has_value(),
      "invalid custom mode must not silently fall back to coordinates"
  );

  custom.sunrise = "07:00";
  const auto customTimes = resolveScheduleTimes(custom, std::nullopt, std::nullopt);
  expect(customTimes.has_value() && customTimes->custom, "valid custom schedule should resolve");
  expect(
      customTimes.has_value() && customTimes->sunsetMinutes == 20 * 60 + 30
          && customTimes->sunriseMinutes == 7 * 60,
      "resolved custom boundary minutes are wrong"
  );

  LocationConfig geographic;
  geographic.latitude = 10.0;
  geographic.longitude = 106.0;
  const auto fallbackCoordinates = resolveCoordinates(geographic, 200.0, 500.0);
  expect(
      fallbackCoordinates.latitude == geographic.latitude && fallbackCoordinates.longitude == geographic.longitude,
      "invalid resolved coordinates should fall back to valid configured coordinates"
  );
  geographic.latitude = 91.0;
  geographic.longitude = 181.0;
  const auto invalidCoordinates = resolveCoordinates(geographic, std::nullopt, std::nullopt);
  expect(
      !invalidCoordinates.latitude.has_value() && !invalidCoordinates.longitude.has_value(),
      "out-of-range configured coordinates should be rejected"
  );

  return failures == 0 ? 0 : 1;
}
