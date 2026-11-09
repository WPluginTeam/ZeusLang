#include <stdio.h>

void pinMode(long long pin, long long mode) {
    printf("[C MCU] pinMode(pin=%lld, mode=%lld)\n", pin, mode);
}

void digitalWrite(long long pin, long long val) {
    printf("[C MCU] digitalWrite(pin=%lld, val=%lld)\n", pin, val);
}

long long digitalRead(long long pin) {
    printf("[C MCU] digitalRead(pin=%lld) -> 1\n", pin);
    return 1;
}

long long addNumbers(long long a, long long b) {
    printf("[C MCU] addNumbers(%lld, %lld) -> %lld\n", a, b, a + b);
    return a + b;
}
