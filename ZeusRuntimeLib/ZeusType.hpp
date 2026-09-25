
#ifndef ZeusType_hpp
#define ZeusType_hpp

#include <variant>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <cstdint>
#include <climits>

namespace ZeusType {

    // Zeus 任意精度大整数实现
    // 使用十进制向量存储，每个元素存储一位数字（0-9）
    // 简单但足够用于脚本语言
    class BigInteger {
    private:
        std::vector<uint8_t> digits;  // 低位在前，每个元素 0-9（仅大整数路径使用）
        bool negative = false;        // 符号位（仅大整数路径使用）

        // P0 快路径：小整数直接存 int64，避免 vector 堆分配
        bool small = true;
        int64_t smallVal = 0;

        // 将小整数展开为十进制 digits（用于溢出回退到大整数路径）
        void expandSmallToBig() {
            if (!small) return;

            small = false;
            int64_t val = smallVal;
            negative = val < 0;
            uint64_t absVal = 0;

            if (negative) {
                absVal = static_cast<uint64_t>(-(val + 1)) + 1;
            } else {
                absVal = static_cast<uint64_t>(val);
            }

            digits.clear();

            if (absVal == 0) {
                digits.push_back(0);
            } else {
                while (absVal > 0) {
                    digits.push_back(static_cast<uint8_t>(absVal % 10));
                    absVal /= 10;
                }
            }
        }

        // 返回大整数表示的拷贝（小整数先展开，不修改自身）
        BigInteger toBigCopy() const {
            if (!small) return *this;

            BigInteger b;
            b.small = false;
            int64_t val = smallVal;
            b.negative = val < 0;
            uint64_t absVal = 0;

            if (b.negative) {
                absVal = static_cast<uint64_t>(-(val + 1)) + 1;
            } else {
                absVal = static_cast<uint64_t>(val);
            }

            b.digits.clear();

            if (absVal == 0) {
                b.digits.push_back(0);
            } else {
                while (absVal > 0) {
                    b.digits.push_back(static_cast<uint8_t>(absVal % 10));
                    absVal /= 10;
                }
            }

            return b;
        }

        static bool addOverflowI64(int64_t a, int64_t b, int64_t& out) {
            if (b > 0) {
                if (a > INT64_MAX - b) return true;
            } else if (b < 0) {
                if (a < INT64_MIN - b) return true;
            }

            out = a + b;
            return false;
        }

        static bool subOverflowI64(int64_t a, int64_t b, int64_t& out) {
            if (b < 0) {
                if (a > INT64_MAX + b) return true;
            } else if (b > 0) {
                if (a < INT64_MIN + b) return true;
            }

            out = a - b;
            return false;
        }
        
        // 移除前导零（仅大整数路径；小整数直接返回）
        void removeLeadingZeros() {
            if (small) return;
            while (digits.size() > 1 && digits.back() == 0) {
                digits.pop_back();
            }
            // 零没有符号
            if (digits.size() == 1 && digits[0] == 0) {
                negative = false;
            }
        }
        
        // 比较绝对值大小（忽略符号）
        // 返回：1 if |this| > |other|, -1 if |this| < |other|, 0 if equal
        int compareAbs(const BigInteger& other) const {
            if (digits.size() != other.digits.size()) {
                return digits.size() > other.digits.size() ? 1 : -1;
            }
            for (int i = digits.size() - 1; i >= 0; --i) {
                if (digits[i] != other.digits[i]) {
                    return digits[i] > other.digits[i] ? 1 : -1;
                }
            }
            return 0;
        }
        
        // 加法：忽略符号，返回 |this| + |other|
        BigInteger addAbs(const BigInteger& other) const {
            BigInteger result;
            result.small = false;
            result.digits.clear();
            int carry = 0;
            size_t maxLen = std::max(digits.size(), other.digits.size());

            for (size_t i = 0; i < maxLen || carry; ++i) {
                int sum = carry;
                if (i < digits.size()) sum += digits[i];
                if (i < other.digits.size()) sum += other.digits[i];

                result.digits.push_back(sum % 10);
                carry = sum / 10;
            }

            result.negative = false;
            return result;
        }

        // 减法：假设 |this| >= |other|，返回 |this| - |other|
        BigInteger subAbs(const BigInteger& other) const {
            BigInteger result;
            result.small = false;
            result.digits.clear();
            int borrow = 0;
            
            for (size_t i = 0; i < digits.size(); ++i) {
                int diff = digits[i] - borrow;
                if (i < other.digits.size()) diff -= other.digits[i];
                
                if (diff < 0) {
                    diff += 10;
                    borrow = 1;
                } else {
                    borrow = 0;
                }
                
                result.digits.push_back(diff);
            }
            
            result.removeLeadingZeros();
            result.negative = false;
            return result;
        }
        
    public:
        BigInteger() : negative(false), small(true), smallVal(0) {}

        BigInteger(int64_t val) : negative(false), small(true), smallVal(val) {}

        // 兼容 int 字面量，避免重载歧义
        BigInteger(int val) : negative(false), small(true), smallVal(val) {}

        BigInteger(const std::string& str) {
            if (str.empty()) {
                throw std::invalid_argument("Empty string for BigInteger");
            }

            // P0：先尝试按 int64 解析，命中则走小整数路径（零分配）
            bool maybeSmall = str.size() <= 19 || (str.size() == 20 && str[0] == '-');

            if (maybeSmall) {
                try {
                    size_t pos = 0;
                    long long parsed = std::stoll(str, &pos, 10);

                    if (pos == str.size()) {
                        negative = false;
                        small = true;
                        smallVal = static_cast<int64_t>(parsed);
                        digits.clear();
                        return;
                    }
                } catch (...) {
                    // 回退到大整数路径
                }
            }

            small = false;
            size_t start = 0;
            negative = false;

            if (str[0] == '-') {
                negative = true;
                start = 1;
            }

            digits.clear();
            for (int i = str.length() - 1; i >= static_cast<int>(start); --i) {
                if (str[i] < '0' || str[i] > '9') {
                    throw std::invalid_argument("Invalid character in BigInteger string: " + str);
                }
                digits.push_back(str[i] - '0');
            }

            removeLeadingZeros();
        }

        std::string toString() const {
            if (small) return std::to_string(smallVal);

            std::stringstream ss;
            if (negative && !isZero()) ss << "-";
            for (int i = digits.size() - 1; i >= 0; --i) {
                ss << static_cast<int>(digits[i]);
            }
            return ss.str();
        }

        bool isSmallInt() const { return small; }
        int64_t smallIntValue() const { return smallVal; }

        bool isZero() const {
            if (small) return smallVal == 0;

            return digits.size() == 1 && digits[0] == 0;
        }

        // 无抛版本：小整数直接返回，大整数按位换算（溢出返回 false）
        bool tryToInt64(int64_t& out) const {
            if (small) {
                out = smallVal;
                return true;
            }

            if (digits.size() > 19) return false;

            uint64_t result = 0;
            uint64_t multiplier = 1;
            const uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;

            for (size_t i = 0; i < digits.size(); ++i) {
                result += static_cast<uint64_t>(digits[i]) * multiplier;

                if (result > limit) return false;

                if (i + 1 < digits.size()) {
                    if (multiplier > 1000000000000000000ULL) return false;

                    multiplier *= 10;
                }
            }

            if (negative) {
                if (result == 9223372036854775808ULL) {
                    out = INT64_MIN;
                    return true;
                }

                out = -static_cast<int64_t>(result);
                return true;
            }

            out = static_cast<int64_t>(result);
            return true;
        }

        // 尝试转换为 int64_t（如果超出范围则抛出异常）
        // Convert to int64_t or throw on overflow.
        int64_t toInt64() const {
            if (small) return smallVal;

            int64_t out = 0;

            if (tryToInt64(out)) return out;

            throw std::overflow_error("BigInteger too large for int64_t");
        }

        // 判断是否能安全转换为 int64_t
        // Check whether value fits in int64_t.
        bool fitsInInt64() const {
            if (small) return true;

            int64_t ignored = 0;
            return tryToInt64(ignored);
        }

        bool operator<(const BigInteger& other) const {
            if (small && other.small) return smallVal < other.smallVal;

            if (small != other.small) {
                return toBigCopy() < other.toBigCopy();
            }

            if (negative != other.negative) return negative;

            int cmp = compareAbs(other);
            return negative ? (cmp > 0) : (cmp < 0);
        }

        bool operator==(const BigInteger& other) const {
            if (small && other.small) return smallVal == other.smallVal;

            if (!small && !other.small) {
                return negative == other.negative && digits == other.digits;
            }

            // 混合形态：至少一侧是小整数，转大整数表示后比较
            // （乘法等暂走大整数路径，6(big) 必须等于 6(small)）
            // 递归必终止：转换后两侧均为大整数，命中上一分支
            return toBigCopy() == other.toBigCopy();
        }

        bool operator!=(const BigInteger& other) const { return !(*this == other); }
        bool operator>(const BigInteger& other) const { return other < *this; }
        bool operator<=(const BigInteger& other) const { return !(other < *this); }
        bool operator>=(const BigInteger& other) const { return !(*this < other); }
        
        BigInteger operator+(const BigInteger& other) const {
            // P0：双小整数直接 int64 加法（零分配），溢出才回退大整数
            if (small && other.small) {
                int64_t sum = 0;

                if (!addOverflowI64(smallVal, other.smallVal, sum)) {
                    return BigInteger(sum);
                }

                return toBigCopy() + other.toBigCopy();
            }

            if (small != other.small) {
                return toBigCopy() + other.toBigCopy();
            }

            if (negative == other.negative) {
                BigInteger result = addAbs(other);
                result.negative = negative;

                int64_t sv = 0;

                if (result.tryToInt64(sv)) return BigInteger(sv);

                return result;
            } else {
                int cmp = compareAbs(other);
                if (cmp == 0) return BigInteger(0);

                if (cmp > 0) {
                    BigInteger result = subAbs(other);
                    result.negative = negative;

                    int64_t sv = 0;

                    if (result.tryToInt64(sv)) return BigInteger(sv);

                    return result;
                } else {
                    BigInteger result = other.subAbs(*this);
                    result.negative = other.negative;

                    int64_t sv = 0;

                    if (result.tryToInt64(sv)) return BigInteger(sv);

                    return result;
                }
            }
        }

        BigInteger operator-(const BigInteger& other) const {
            // P0：双小整数直接 int64 减法（零分配）
            if (small && other.small) {
                int64_t diff = 0;

                if (!subOverflowI64(smallVal, other.smallVal, diff)) {
                    return BigInteger(diff);
                }

                return toBigCopy() - other.toBigCopy();
            }

            if (small != other.small) {
                return toBigCopy() - other.toBigCopy();
            }

            BigInteger negOther = other;
            negOther.negative = !other.negative;
            return *this + negOther;
        }
        
        BigInteger operator*(const BigInteger& other) const {
            // P0：乘法仍走大整数路径（小整数先展开，保证正确性）
            BigInteger lhs = toBigCopy();
            BigInteger rhs = other.toBigCopy();
            const auto& a = lhs.digits;
            const auto& b = rhs.digits;

            BigInteger result;
            result.small = false;
            result.digits.assign(a.size() + b.size(), 0);

            for (size_t i = 0; i < a.size(); ++i) {
                int carry = 0;
                for (size_t j = 0; j < b.size() || carry; ++j) {
                    int64_t prod = result.digits[i + j] + a[i] * (j < b.size() ? b[j] : 0) + carry;
                    result.digits[i + j] = prod % 10;
                    carry = prod / 10;
                }
            }

            result.negative = lhs.negative != rhs.negative;
            result.removeLeadingZeros();

            int64_t sv = 0;

            if (result.tryToInt64(sv)) return BigInteger(sv);

            return result;
        }

        BigInteger operator/(const BigInteger& other) const {
            if (other.isZero()) throw std::runtime_error("Division by zero");

            // P0：除法走大整数路径（小整数先展开）
            if (small || other.small) {
                return toBigCopy() / other.toBigCopy();
            }

            BigInteger dividend = *this;
            BigInteger divisor = other;
            dividend.negative = false;
            divisor.negative = false;

            if (dividend < divisor) return BigInteger(0);

            BigInteger quotient;
            quotient.small = false;
            quotient.digits.assign(dividend.digits.size(), 0);

            BigInteger current;
            current.small = false;
            current.digits.clear();
            
            for (int i = dividend.digits.size() - 1; i >= 0; --i) {
                if (current.small) {
                    current = current * BigInteger(10) +
                              BigInteger(static_cast<int>(dividend.digits[i]));
                } else {
                    current.digits.insert(current.digits.begin(), dividend.digits[i]);
                    current.removeLeadingZeros();
                }
                
                int x = 0;
                int l = 0, r = 9;
                while (l <= r) {
                    int m = (l + r) / 2;
                    BigInteger t = divisor * BigInteger(m);
                    if (t <= current) {
                        x = m;
                        l = m + 1;
                    } else {
                        r = m - 1;
                    }
                }
                
                quotient.digits[i] = x;
                current = current - divisor * BigInteger(x);
            }
            
            quotient.negative = negative != other.negative;
            quotient.removeLeadingZeros();

            int64_t sv = 0;

            if (quotient.tryToInt64(sv)) return BigInteger(sv);

            return quotient;
        }
        
        BigInteger operator%(const BigInteger& other) const {
            BigInteger quotient = *this / other;
            return *this - quotient * other;
        }
        
        BigInteger operator-() const {
            if (small) {
                // INT64_MIN 取反溢出，回退大整数
                if (smallVal == INT64_MIN) {
                    BigInteger b = toBigCopy();
                    b.negative = !b.negative;
                    if (b.isZero()) b.negative = false;
                    return b;
                }
                BigInteger r;
                r.small = true;
                r.smallVal = -smallVal;
                r.negative = false;
                return r;
            }
            BigInteger result = *this;
            if (!isZero()) result.negative = !negative;
            return result;
        }
    };

    // BigDecimal: 任意精度十进制浮点数
    // 内部表示: value = integerPart + fractionalPart / 10^scale
    // 例如: 3.14159 表示为 integerPart=3, fractionalPart=14159, scale=5
    class BigDecimal {
    private:
        BigInteger integerPart;      // 整数部分
        BigInteger fractionalPart;   // 小数部分（不含小数点）
        int scale;                   // 小数位数
        bool negative;               // 符号

        // 规范化：移除尾部的0，调整scale
        void normalize() {
            if (scale == 0) {
                fractionalPart = BigInteger(0);
                return;
            }

            std::string fracStr = fractionalPart.toString();
            
            // 移除前导0（因为fractionalPart可能被BigInteger规范化）
            while (fracStr.length() < scale) {
                fracStr = "0" + fracStr;
            }

            // 移除尾部0
            while (scale > 0 && fracStr.back() == '0') {
                fracStr.pop_back();
                scale--;
            }

            if (scale == 0) {
                fractionalPart = BigInteger(0);
            } else {
                fractionalPart = BigInteger(fracStr);
            }

            if (integerPart == BigInteger(0) && fractionalPart == BigInteger(0)) {
                negative = false;
            }
        }

    public:
        BigDecimal() : integerPart(0), fractionalPart(0), scale(0), negative(false) {}
        
        explicit BigDecimal(int value) 
            : integerPart(value), fractionalPart(0), scale(0), negative(value < 0) {
            if (value < 0) {
                integerPart = BigInteger(0) - integerPart;
            }
        }
        
        explicit BigDecimal(int64_t value) 
            : integerPart(value), fractionalPart(0), scale(0), negative(value < 0) {
            if (value < 0) {
                integerPart = BigInteger(0) - integerPart;
            }
        }
        
        // 从BigInteger构造
        explicit BigDecimal(const BigInteger& value) 
            : integerPart(value), fractionalPart(0), scale(0), negative(false) {
            if (value < BigInteger(0)) {
                negative = true;
                integerPart = BigInteger(0) - value;  // 取绝对值
            }
        }
        
        explicit BigDecimal(const std::string& str) {
            if (str.empty()) {
                throw std::invalid_argument("Empty string for BigDecimal");
            }
            
            size_t start = 0;
            negative = false;
            
            if (str[0] == '-') {
                negative = true;
                start = 1;
            }
            
            size_t dotPos = str.find('.', start);
            if (dotPos == std::string::npos) {
                integerPart = BigInteger(str.substr(start));
                fractionalPart = BigInteger(0);
                scale = 0;
            } else {
                std::string intPart = str.substr(start, dotPos - start);
                std::string fracPart = str.substr(dotPos + 1);
                
                if (intPart.empty()) {
                    integerPart = BigInteger(0);
                } else {
                    integerPart = BigInteger(intPart);
                }
                
                if (fracPart.empty()) {
                    fractionalPart = BigInteger(0);
                    scale = 0;
                } else {
                    fractionalPart = BigInteger(fracPart);
                    scale = fracPart.length();
                }
            }
            
            normalize();
        }
        
        explicit BigDecimal(double value) {
            // 简单转换：使用字符串表示
            std::stringstream ss;
            ss << value;
            *this = BigDecimal(ss.str());
        }
        
        std::string toString() const {
            std::string intStr = integerPart.toString();
            std::string fracStr = fractionalPart.toString();
            
            if (scale == 0) {
                return negative ? "-" + intStr : intStr;
            }
            
            while (fracStr.length() < scale) {
                fracStr = "0" + fracStr;
            }
            
            std::string result = intStr + "." + fracStr;
            return negative ? "-" + result : result;
        }
        
        double toDouble() const {
            return std::stod(toString());
        }
        
        bool isZero() const { return integerPart == BigInteger(0) && fractionalPart == BigInteger(0); }
        
        BigDecimal abs() const {
            BigDecimal result = *this;
            result.negative = false;
            return result;
        }
        
        BigDecimal operator-() const {
            BigDecimal result = *this;
            if (!isZero()) result.negative = !negative;
            return result;
        }
        
        BigDecimal operator+(const BigDecimal& other) const {
            if (negative == other.negative) {
                BigDecimal result = addAbsolute(other, negative);
                return result;
            } else {
                return *this - (-other);
            }
        }
        
        BigDecimal operator-(const BigDecimal& other) const {
            if (negative != other.negative) {
                BigDecimal result = addAbsolute(other, negative);
                return result;
            } else {
                int cmp = absCompare(other);
                if (cmp == 0) return BigDecimal(0);
                
                if (cmp > 0) {
                    BigDecimal result = subtractAbsolute(other, negative);
                    return result;
                } else {
                    BigDecimal result = other.subtractAbsolute(*this, !negative);
                    return result;
                }
            }
        }
        
        BigDecimal operator*(const BigDecimal& other) const {
            BigDecimal result;
            
            // 合并整数和小数部分
            BigInteger thisTotal = integerPart;
            for (int i = 0; i < scale; i++) {
                thisTotal = thisTotal * BigInteger(10);
            }
            thisTotal = thisTotal + fractionalPart;
            
            BigInteger otherTotal = other.integerPart;
            for (int i = 0; i < other.scale; i++) {
                otherTotal = otherTotal * BigInteger(10);
            }
            otherTotal = otherTotal + other.fractionalPart;
            
            BigInteger product = thisTotal * otherTotal;
            int totalScale = scale + other.scale;
            
            // 重新分割
            BigInteger divisor(1);
            for (int i = 0; i < totalScale; i++) {
                divisor = divisor * BigInteger(10);
            }
            
            BigInteger intPart = product / divisor;
            BigInteger fracPart = product % divisor;
            
            result.integerPart = intPart;
            result.fractionalPart = fracPart;
            result.scale = totalScale;
            result.negative = negative != other.negative;
            result.normalize();
            
            return result;
        }
        
        BigDecimal operator/(const BigDecimal& other) const {
            return divide(other, 16);
        }
        
        BigDecimal divide(const BigDecimal& other, int precision) const {
            if (other.isZero()) throw std::runtime_error("Division by zero");
            
            BigDecimal result;
            
            BigInteger dividend = integerPart;
            for (int i = 0; i < scale; i++) {
                dividend = dividend * BigInteger(10);
            }
            dividend = dividend + fractionalPart;
            
            for (int i = 0; i < precision; i++) {
                dividend = dividend * BigInteger(10);
            }
            
            BigInteger divisor = other.integerPart;
            for (int i = 0; i < other.scale; i++) {
                divisor = divisor * BigInteger(10);
            }
            divisor = divisor + other.fractionalPart;
            
            BigInteger quotient = dividend / divisor;
            int scaleResult = precision + scale - other.scale;
            
            BigInteger scaleDivisor(1);
            for (int i = 0; i < scaleResult; i++) {
                scaleDivisor = scaleDivisor * BigInteger(10);
            }
            
            BigInteger intPart = quotient / scaleDivisor;
            BigInteger fracPart = quotient % scaleDivisor;
            
            result.integerPart = intPart;
            result.fractionalPart = fracPart;
            result.scale = scaleResult;
            result.negative = negative != other.negative;
            result.normalize();
            
            return result;
        }
        
        BigDecimal operator%(const BigDecimal& other) const {
            BigDecimal quotient = *this / other;
            BigDecimal floored = quotient.floor();
            return *this - (floored * other);
        }
        
        BigDecimal floor() const {
            if (negative && fractionalPart != BigInteger(0)) {
                return BigDecimal(-(integerPart + BigInteger(1)));
            }
            return BigDecimal(negative ? -integerPart : integerPart);
        }
        
        bool operator==(const BigDecimal& other) const {
            return negative == other.negative &&
                   integerPart == other.integerPart &&
                   fractionalPart == other.fractionalPart &&
                   scale == other.scale;
        }
        
        bool operator!=(const BigDecimal& other) const { return !(*this == other); }
        
        bool operator<(const BigDecimal& other) const {
            if (negative != other.negative) return negative;
            
            int cmp = absCompare(other);
            return negative ? (cmp > 0) : (cmp < 0);
        }
        
        bool operator>(const BigDecimal& other) const { return other < *this; }
        bool operator<=(const BigDecimal& other) const { return !(other < *this); }
        bool operator>=(const BigDecimal& other) const { return !(*this < other); }
        
        int absCompare(const BigDecimal& other) const {
            if (integerPart != other.integerPart) {
                return integerPart < other.integerPart ? -1 : 1;
            }
            
            BigInteger thisFrac = fractionalPart;
            for (int i = 0; i < (other.scale - scale); i++) {
                thisFrac = thisFrac * BigInteger(10);
            }
            
            BigInteger otherFrac = other.fractionalPart;
            for (int i = 0; i < (scale - other.scale); i++) {
                otherFrac = otherFrac * BigInteger(10);
            }
            
            if (thisFrac == otherFrac) return 0;
            return thisFrac < otherFrac ? -1 : 1;
        }
        
        BigDecimal addAbsolute(const BigDecimal& other, bool resultNegative) const {
            BigDecimal result;
            
            BigInteger thisFrac = fractionalPart;
            for (int i = 0; i < (other.scale - scale); i++) {
                thisFrac = thisFrac * BigInteger(10);
            }
            
            BigInteger otherFrac = other.fractionalPart;
            for (int i = 0; i < (scale - other.scale); i++) {
                otherFrac = otherFrac * BigInteger(10);
            }
            
            BigInteger fracSum = thisFrac + otherFrac;
            BigInteger carry(0);
            BigInteger scaleBase(1);
            int resultScale = std::max(scale, other.scale);
            
            for (int i = 0; i < resultScale; i++) {
                scaleBase = scaleBase * BigInteger(10);
            }
            
            if (fracSum >= scaleBase) {
                carry = BigInteger(1);
                fracSum = fracSum - scaleBase;
            }
            
            result.integerPart = integerPart + other.integerPart + carry;
            result.fractionalPart = fracSum;
            result.scale = resultScale;
            result.negative = resultNegative;
            result.normalize();
            
            return result;
        }
        
        BigDecimal subtractAbsolute(const BigDecimal& other, bool resultNegative) const {
            BigDecimal result;
            
            BigInteger thisFrac = fractionalPart;
            for (int i = 0; i < (other.scale - scale); i++) {
                thisFrac = thisFrac * BigInteger(10);
            }
            
            BigInteger otherFrac = other.fractionalPart;
            for (int i = 0; i < (scale - other.scale); i++) {
                otherFrac = otherFrac * BigInteger(10);
            }
            
            BigInteger borrow(0);
            BigInteger scaleBase(1);
            int resultScale = std::max(scale, other.scale);
            
            for (int i = 0; i < resultScale; i++) {
                scaleBase = scaleBase * BigInteger(10);
            }
            
            if (thisFrac < otherFrac) {
                borrow = BigInteger(1);
                thisFrac = thisFrac + scaleBase;
            }
            
            result.integerPart = integerPart - other.integerPart - borrow;
            result.fractionalPart = thisFrac - otherFrac;
            result.scale = resultScale;
            result.negative = resultNegative;
            result.normalize();
            
            return result;
        }
    };

    class String {
        
    };

    class Integer {

    };

    class Float {

    };

    class Boolean {

    };

    class Null {

    };

    struct ObjectData; // 以后放真正键值容器（需要完整 UniversalVariant）
    struct ArrayData;  // 以后放真正顺序容器

    class Object {
    public:
        std::shared_ptr<ObjectData> impl;

        Object() = default;
    };

    class Array {
    public:
        std::shared_ptr<ArrayData> impl;

        Array() = default;
    };

    class Function {

    };

    struct ClosureHandle    { int id; };
    struct NativeFuncHandle { int id; };

    // SharedHandle：共享堆变量句柄，用于跨 VM 共享
    struct SharedHandle {
        std::string name;  // 共享变量名称
    };

    // Bytes 类型：字节数组（类似 Python 的 bytes，作为基础类型）
    class Bytes {
    public:
        std::vector<uint8_t> data;

        Bytes() = default;
        Bytes(const std::vector<uint8_t>& data) : data(data) {}
        Bytes(const std::string& str) {
            for (char c : str) {
                data.push_back(static_cast<uint8_t>(c));
            }
        }

        bool operator==(const Bytes& other) const {
            return data == other.data;
        }
    };

    inline int bigIntegerToInt(const BigInteger& big) {
        if (!big.fitsInInt64()) {
            throw std::overflow_error("BigInteger too large for int");
        }
        int64_t val = big.toInt64();
        if (val < INT_MIN || val > INT_MAX) {
            throw std::overflow_error("BigInteger too large for int");
        }
        return static_cast<int>(val);
    }

    using Variant = std::variant<String, Integer, Float, Boolean, Null, Object, Array, Function, Bytes>;

    // Non-owning pointer to an object laid out and allocated by the LLVM emitter.
    // The shared state tracks deletion; copying a dynamic value never owns the object.
    struct LLVMClassObjectState {
        std::atomic<void*> object{ nullptr };
        std::string typeName;
        uint64_t debugId = 0;
    };
    struct LLVMClassHandle {
        std::shared_ptr<LLVMClassObjectState> state;
    };
    
    // 面向对象：类与对象句柄（仅保存 id，便于在 VM 内部查表）
    struct ClassHandle  { int id; };
    struct ObjectHandle { 
        int id; 
        int generation = 0;  // generation 用于检测 slot 复用后的悬空引用
    };
    struct NamespaceHandle { int id; };  // 包命名空间句柄
    
    // 共享类/对象句柄：存储在共享堆上，通过名称访问
    struct SharedClassHandle { std::string name; };   // 共享类定义
    struct SharedObjectHandle { std::string name; };  // 共享对象实例
    
    // Socket 句柄：网络套接字
    struct SocketHandle { int id; };
    
    // 可变参数传递结构（轻量级，仅用于有*args或**kwargs的函数）
    enum class ArgType { ARG, KWARG };
    struct ArgInfo {
        int bindVarIndex;  // 参数绑定的变量索引（位置参数为序号，命名参数为-1）
        std::string name;  // 命名参数的名称（位置参数为空）
        ArgType type;      // ARG或KWARG
    };
    
    // 模块代理对象：用于引用映射包的全局变量，而非复制值
    // 例如: __get_module("ZeusBuiltins.cppfunctions") 返回 ModuleProxy{"ZeusBuiltins.cppfunctions"}
    // 访问 proxy.io 时，自动映射到全局变量 "ZeusBuiltins.cppfunctions.io"
    struct ModuleProxy {
        std::string modulePath;  // 模块路径，如 "ZeusBuiltins.cppfunctions"

        bool operator==(const ModuleProxy& other) const {
            return modulePath == other.modulePath;
        }
    };
    
}
#endif
