#include <cstdint>

namespace {

struct Sensor {
    int32_t level = 0;
    int32_t (*setLevel)(Sensor*, int32_t) = nullptr;
    void (*bump)(Sensor*, int32_t) = nullptr;
};

Sensor g_sensor;

int32_t sensorSetLevel(Sensor* sensor, int32_t level) {
    sensor->level = level;
    return level * 2;
}

void sensorBump(Sensor* sensor, int32_t delta) {
    sensor->level += delta;
}

int32_t binaryAdd(int32_t left, int32_t right) {
    return left + right;
}

} // namespace

extern "C" int64_t zeus_test_indirect_binary_add() {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(&binaryAdd));
}

extern "C" int64_t zeus_test_indirect_sensor() {
    g_sensor.setLevel = &sensorSetLevel;
    g_sensor.bump = &sensorBump;
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(&g_sensor));
}

// Stands in for reading a function-pointer field out of a C struct.
extern "C" int64_t zeus_test_indirect_sensor_fn(int32_t which) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(
        which == 0 ? g_sensor.setLevel : nullptr));
}

extern "C" int64_t zeus_test_indirect_sensor_bump() {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_sensor.bump));
}

extern "C" int32_t zeus_test_indirect_sensor_level() {
    return g_sensor.level;
}
