#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ZeusTypeInfer {

    inline std::string encodeGenericTypeName(const std::string& typeName) {
        static const char digits[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(typeName.size() * 2);
        for (unsigned char byte : typeName) {
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0f]);
        }
        return encoded;
    }

    inline std::string genericSpecializationName(
        const std::string& baseName,
        const std::vector<std::string>& typeArguments
    ) {
        if (typeArguments.size() == 1) {
            return baseName + "$__zeus_type_" +
                encodeGenericTypeName(typeArguments.front());
        }

        std::string result = baseName + "$__zeus_types_";
        for (size_t i = 0; i < typeArguments.size(); ++i) {
            if (i != 0) result += "_";
            result += std::to_string(typeArguments[i].size()) + "x" +
                encodeGenericTypeName(typeArguments[i]);
        }
        return result;
    }

    inline int genericTypeNameHexValue(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    inline bool decodeGenericTypeNameHex(
        const std::string& encoded,
        size_t byteLength,
        std::string& decoded
    ) {
        if (byteLength > encoded.size() / 2 || encoded.size() != byteLength * 2) {
            return false;
        }
        std::string value;
        value.reserve(byteLength);
        for (size_t i = 0; i < encoded.size(); i += 2) {
            const int high = genericTypeNameHexValue(encoded[i]);
            const int low = genericTypeNameHexValue(encoded[i + 1]);
            if (high < 0 || low < 0) return false;
            value.push_back(static_cast<char>((high << 4) | low));
        }
        decoded = std::move(value);
        return true;
    }

    inline bool decodeGenericSpecializationName(
        const std::string& specializedName,
        std::string& baseName,
        std::vector<std::string>& typeArguments
    ) {
        const std::string singleMarker = "$__zeus_type_";
        const std::string multipleMarker = "$__zeus_types_";
        const size_t single = specializedName.rfind(singleMarker);
        const size_t multiple = specializedName.rfind(multipleMarker);
        const bool isSingle = single != std::string::npos &&
            (multiple == std::string::npos || single > multiple);
        const size_t marker = isSingle ? single : multiple;
        if (marker == std::string::npos) return false;

        std::string parsedBase = specializedName.substr(0, marker);
        if (parsedBase.empty()) return false;
        std::vector<std::string> parsedArguments;
        if (isSingle) {
            const size_t encodedStart = marker + singleMarker.size();
            const size_t encodedLength = specializedName.size() - encodedStart;
            if ((encodedLength & 1u) != 0) return false;
            std::string argument;
            if (!decodeGenericTypeNameHex(
                    specializedName.substr(encodedStart),
                    encodedLength / 2,
                    argument)) {
                return false;
            }
            parsedArguments.push_back(std::move(argument));
        } else {
            size_t cursor = marker + multipleMarker.size();
            if (cursor >= specializedName.size()) return false;
            while (cursor < specializedName.size()) {
                const size_t digitsBegin = cursor;
                while (cursor < specializedName.size() &&
                       specializedName[cursor] >= '0' &&
                       specializedName[cursor] <= '9') {
                    ++cursor;
                }
                if (digitsBegin == cursor || cursor >= specializedName.size() ||
                    specializedName[cursor] != 'x') {
                    return false;
                }
                size_t byteLength = 0;
                try {
                    byteLength = static_cast<size_t>(std::stoull(
                        specializedName.substr(digitsBegin, cursor - digitsBegin)));
                } catch (...) {
                    return false;
                }
                ++cursor;
                if (byteLength > (specializedName.size() - cursor) / 2) {
                    return false;
                }
                const size_t encodedLength = byteLength * 2;
                std::string argument;
                if (!decodeGenericTypeNameHex(
                        specializedName.substr(cursor, encodedLength),
                        byteLength, argument)) {
                    return false;
                }
                parsedArguments.push_back(std::move(argument));
                cursor += encodedLength;
                if (cursor == specializedName.size()) break;
                if (specializedName[cursor] != '_') return false;
                ++cursor;
                if (cursor == specializedName.size()) return false;
            }
            if (parsedArguments.size() < 2) return false;
        }

        baseName = std::move(parsedBase);
        typeArguments = std::move(parsedArguments);
        return true;
    }

}
