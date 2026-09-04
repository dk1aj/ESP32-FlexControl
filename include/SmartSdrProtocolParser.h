#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace SmartSdrProtocolParser
{
namespace Detail
{
inline bool isDecimalDigit(const char character)
{
    return character >= '0' && character <= '9';
}

inline bool isHexDigit(const char character)
{
    return isDecimalDigit(character) ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

inline uint8_t hexValue(const char character)
{
    if (isDecimalDigit(character))
    {
        return static_cast<uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f')
    {
        return static_cast<uint8_t>(character - 'a' + 10);
    }
    return static_cast<uint8_t>(character - 'A' + 10);
}

inline bool isTokenDelimiter(const char character)
{
    return character == '\0' ||
           character == ' ' ||
           character == '\t' ||
           character == '\r' ||
           character == '\n' ||
           character == '|';
}

inline const char *findFieldValue(const char *line, const char *field)
{
    if (line == nullptr || field == nullptr || field[0] == '\0')
    {
        return nullptr;
    }

    const size_t fieldLength = strlen(field);
    const char *candidate = line;
    while ((candidate = strstr(candidate, field)) != nullptr)
    {
        if (candidate == line || isTokenDelimiter(candidate[-1]))
        {
            return candidate + fieldLength;
        }
        candidate += fieldLength;
    }
    return nullptr;
}

inline bool parseDecimalUint32(const char *&cursor, uint32_t &value)
{
    if (cursor == nullptr || !isDecimalDigit(*cursor))
    {
        return false;
    }

    uint32_t parsed = 0;
    while (isDecimalDigit(*cursor))
    {
        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10U)
        {
            return false;
        }
        parsed = parsed * 10U + digit;
        ++cursor;
    }
    value = parsed;
    return true;
}

inline bool parseHexUint32(const char *&cursor, uint32_t &value)
{
    if (cursor == nullptr || !isHexDigit(*cursor))
    {
        return false;
    }

    uint32_t parsed = 0;
    while (isHexDigit(*cursor))
    {
        const uint32_t digit = hexValue(*cursor);
        if (parsed > (UINT32_MAX - digit) / 16U)
        {
            return false;
        }
        parsed = parsed * 16U + digit;
        ++cursor;
    }
    value = parsed;
    return true;
}
} // namespace Detail

inline bool parseUnsignedField(const char *line,
                               const char *field,
                               uint16_t &value)
{
    const char *cursor = Detail::findFieldValue(line, field);
    uint32_t parsed = 0;
    if (!Detail::parseDecimalUint32(cursor, parsed) ||
        !Detail::isTokenDelimiter(*cursor) ||
        parsed > UINT16_MAX)
    {
        return false;
    }

    value = static_cast<uint16_t>(parsed);
    return true;
}

inline bool parseSignedField(const char *line,
                             const char *field,
                             int32_t &value)
{
    const char *cursor = Detail::findFieldValue(line, field);
    if (cursor == nullptr)
    {
        return false;
    }

    const bool negative = *cursor == '-';
    if (negative)
    {
        ++cursor;
    }

    uint32_t parsed = 0;
    if (!Detail::parseDecimalUint32(cursor, parsed) ||
        !Detail::isTokenDelimiter(*cursor) ||
        (!negative && parsed > static_cast<uint32_t>(INT32_MAX)) ||
        (negative && parsed > static_cast<uint32_t>(INT32_MAX) + 1U))
    {
        return false;
    }

    if (!negative)
    {
        value = static_cast<int32_t>(parsed);
    }
    else if (parsed == static_cast<uint32_t>(INT32_MAX) + 1U)
    {
        value = INT32_MIN;
    }
    else
    {
        value = -static_cast<int32_t>(parsed);
    }
    return true;
}

inline bool parseHexField(const char *line,
                          const char *field,
                          uint32_t &value)
{
    const char *cursor = Detail::findFieldValue(line, field);
    if (cursor == nullptr || cursor[0] != '0' ||
        (cursor[1] != 'x' && cursor[1] != 'X'))
    {
        return false;
    }

    cursor += 2;
    uint32_t parsed = 0;
    if (!Detail::parseHexUint32(cursor, parsed) ||
        !Detail::isTokenDelimiter(*cursor))
    {
        return false;
    }

    value = parsed;
    return true;
}

inline bool parseTextField(const char *line,
                           const char *field,
                           char *value,
                           const size_t valueSize)
{
    const char *cursor = Detail::findFieldValue(line, field);
    if (cursor == nullptr || value == nullptr || valueSize == 0 ||
        Detail::isTokenDelimiter(*cursor))
    {
        return false;
    }

    const char *end = cursor;
    while (!Detail::isTokenDelimiter(*end))
    {
        ++end;
    }
    const size_t length = static_cast<size_t>(end - cursor);
    if (length + 1 > valueSize)
    {
        return false;
    }

    memcpy(value, cursor, length);
    value[length] = '\0';
    return true;
}

inline bool parseSliceNumber(const char *payload,
                             const uint8_t maximumSliceCount,
                             uint8_t &sliceNumber)
{
    constexpr char PREFIX[] = "slice ";
    if (payload == nullptr ||
        maximumSliceCount == 0 ||
        strncmp(payload, PREFIX, sizeof(PREFIX) - 1U) != 0)
    {
        return false;
    }

    const char *cursor = payload + sizeof(PREFIX) - 1U;
    uint32_t parsed = 0;
    if (!Detail::parseDecimalUint32(cursor, parsed) ||
        !Detail::isTokenDelimiter(*cursor) ||
        parsed >= maximumSliceCount)
    {
        return false;
    }

    sliceNumber = static_cast<uint8_t>(parsed);
    return true;
}

inline bool parseFrequencyHz(const char *line, uint64_t &frequencyHz)
{
    constexpr char FIELD[] = "RF_frequency=";
    const char *cursor = Detail::findFieldValue(line, FIELD);
    if (cursor == nullptr || !Detail::isDecimalDigit(*cursor))
    {
        return false;
    }

    char *end = nullptr;
    const double frequencyMhz = strtod(cursor, &end);
    const long double roundedHz =
        static_cast<long double>(frequencyMhz) * 1000000.0L + 0.5L;
    if (end == cursor ||
        !Detail::isTokenDelimiter(*end) ||
        !isfinite(frequencyMhz) ||
        frequencyMhz <= 0.0 ||
        roundedHz > static_cast<long double>(UINT64_MAX))
    {
        return false;
    }

    frequencyHz = static_cast<uint64_t>(roundedHz);
    return frequencyHz > 0;
}

inline bool parseResponse(const char *line,
                          uint32_t &sequence,
                          uint32_t &responseCode)
{
    if (line == nullptr || line[0] != 'R')
    {
        return false;
    }

    const char *cursor = line + 1;
    uint32_t parsedSequence = 0;
    if (!Detail::parseDecimalUint32(cursor, parsedSequence) || *cursor != '|')
    {
        return false;
    }

    ++cursor;
    uint32_t parsedResponse = 0;
    if (!Detail::parseHexUint32(cursor, parsedResponse) ||
        (*cursor != '|' && *cursor != '\0'))
    {
        return false;
    }

    sequence = parsedSequence;
    responseCode = parsedResponse;
    return true;
}
} // namespace SmartSdrProtocolParser
