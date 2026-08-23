#include <Arduino.h>
#include <string.h>
#include <unity.h>

#include "SmartSdrLineParser.h"
#include "SmartSdrProtocolParser.h"

namespace
{
using SmallParser = SmartSdrLineParser<8>;
using ProductionParser = SmartSdrLineParser<2048>;

template <typename Parser>
void assertResult(const typename Parser::Result expected,
                  const typename Parser::Result actual)
{
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected),
                          static_cast<int>(actual));
}

template <typename Parser>
void pushTextWithoutLine(Parser &parser, const char *text)
{
    while (*text != '\0')
    {
        assertResult<Parser>(Parser::Result::None, parser.push(*text++));
    }
}

void testNormalPartialLine()
{
    SmallParser parser;
    pushTextWithoutLine(parser, "sli");
    pushTextWithoutLine(parser, "ce");
    assertResult<SmallParser>(SmallParser::Result::LineReady,
                              parser.push('\n'));
    TEST_ASSERT_EQUAL_STRING("slice", parser.line());
}

void testEmptyCrLfAndBackToBackLines()
{
    SmallParser parser;
    assertResult<SmallParser>(SmallParser::Result::None, parser.push('\r'));
    assertResult<SmallParser>(SmallParser::Result::None, parser.push('\n'));

    pushTextWithoutLine(parser, "A");
    assertResult<SmallParser>(SmallParser::Result::LineReady,
                              parser.push('\r'));
    TEST_ASSERT_EQUAL_STRING("A", parser.line());
    assertResult<SmallParser>(SmallParser::Result::None, parser.push('\n'));

    pushTextWithoutLine(parser, "B");
    assertResult<SmallParser>(SmallParser::Result::LineReady,
                              parser.push('\n'));
    TEST_ASSERT_EQUAL_STRING("B", parser.line());
}

void testOverflowDiscardsThroughDelimiterAndRecovers()
{
    SmallParser parser;
    pushTextWithoutLine(parser, "1234567");
    assertResult<SmallParser>(SmallParser::Result::Overflow, parser.push('8'));
    assertResult<SmallParser>(SmallParser::Result::None, parser.push('X'));
    assertResult<SmallParser>(SmallParser::Result::None, parser.push('\n'));

    pushTextWithoutLine(parser, "OK");
    assertResult<SmallParser>(SmallParser::Result::LineReady,
                              parser.push('\n'));
    TEST_ASSERT_EQUAL_STRING("OK", parser.line());
}

void testResetLeavesDiscardMode()
{
    SmallParser parser;
    pushTextWithoutLine(parser, "1234567");
    assertResult<SmallParser>(SmallParser::Result::Overflow, parser.push('8'));

    parser.reset();
    pushTextWithoutLine(parser, "A");
    assertResult<SmallParser>(SmallParser::Result::LineReady,
                              parser.push('\n'));
    TEST_ASSERT_EQUAL_STRING("A", parser.line());
}

void testProductionBufferBoundary()
{
    static ProductionParser parser;
    parser.reset();
    for (size_t index = 0;
         index < ProductionParser::maxLineLength();
         ++index)
    {
        assertResult<ProductionParser>(ProductionParser::Result::None,
                                       parser.push('A'));
    }
    assertResult<ProductionParser>(ProductionParser::Result::LineReady,
                                   parser.push('\n'));
    TEST_ASSERT_EQUAL_UINT32(ProductionParser::maxLineLength(),
                             strlen(parser.line()));

    for (size_t index = 0;
         index < ProductionParser::maxLineLength();
         ++index)
    {
        assertResult<ProductionParser>(ProductionParser::Result::None,
                                       parser.push('B'));
    }
    assertResult<ProductionParser>(ProductionParser::Result::Overflow,
                                   parser.push('B'));
}

void testUnsignedFieldValidation()
{
    uint16_t value = 999;
    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseUnsignedField(
        "S1|transmit 0 rfpower=90", "rfpower=", value));
    TEST_ASSERT_EQUAL_UINT16(90, value);

    value = 999;
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseUnsignedField(
        "S1|transmit 0 notrfpower=90", "rfpower=", value));
    TEST_ASSERT_EQUAL_UINT16(999, value);

    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseUnsignedField(
        "S1|transmit 0 rfpower=90bad", "rfpower=", value));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseUnsignedField(
        "S1|transmit 0 rfpower=65536", "rfpower=", value));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseUnsignedField(
        "S1|transmit 0 rfpower=", "rfpower=", value));

    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseUnsignedField(
        "slice 0 inactive=1 active=0", "active=", value));
    TEST_ASSERT_EQUAL_UINT16(0, value);
}

void testHexAndTextFieldValidation()
{
    uint32_t handle = 0;
    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseHexField(
        "slice 0 client_handle=0x7A231C50 active=1",
        "client_handle=", handle));
    TEST_ASSERT_EQUAL_HEX32(0x7A231C50UL, handle);
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseHexField(
        "slice 0 notclient_handle=0x1234", "client_handle=", handle));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseHexField(
        "slice 0 client_handle=7A231C50", "client_handle=", handle));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseHexField(
        "slice 0 client_handle=0x1234bad-value", "client_handle=", handle));

    char clientId[37] = {};
    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseTextField(
        "client 0x1 connected client_id=7661C601-4964-4F02-B36E-C72C054BA082 program=SmartSDR-Win",
        "client_id=", clientId, sizeof(clientId)));
    TEST_ASSERT_EQUAL_STRING("7661C601-4964-4F02-B36E-C72C054BA082", clientId);
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseTextField(
        "client 0x1 connected notclient_id=bad", "client_id=",
        clientId, sizeof(clientId)));
    char tooSmall[4] = {};
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseTextField(
        "client_id=1234", "client_id=", tooSmall, sizeof(tooSmall)));
}

void testSliceNumberValidation()
{
    uint8_t sliceNumber = 99;
    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseSliceNumber(
        "slice 7 RF_frequency=14.074", 8, sliceNumber));
    TEST_ASSERT_EQUAL_UINT8(7, sliceNumber);

    sliceNumber = 99;
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseSliceNumber(
        "slice 8 RF_frequency=14.074", 8, sliceNumber));
    TEST_ASSERT_EQUAL_UINT8(99, sliceNumber);
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseSliceNumber(
        "slice +1 active=1", 8, sliceNumber));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseSliceNumber(
        "slice  1 active=1", 8, sliceNumber));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseSliceNumber(
        "slice 1bad active=1", 8, sliceNumber));
}

void testFrequencyValidation()
{
    uint64_t frequencyHz = 123;
    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseFrequencyHz(
        "slice 0 RF_frequency=14.074000 active=1", frequencyHz));
    TEST_ASSERT_EQUAL_UINT32(14074000UL,
                             static_cast<uint32_t>(frequencyHz));

    frequencyHz = 123;
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseFrequencyHz(
        "slice 0 notRF_frequency=14.074", frequencyHz));
    TEST_ASSERT_EQUAL_UINT32(123UL, static_cast<uint32_t>(frequencyHz));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseFrequencyHz(
        "slice 0 RF_frequency=-14.074", frequencyHz));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseFrequencyHz(
        "slice 0 RF_frequency=nan", frequencyHz));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseFrequencyHz(
        "slice 0 RF_frequency=14.074bad", frequencyHz));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseFrequencyHz(
        "slice 0 RF_frequency=1e20", frequencyHz));
}

void testResponseValidation()
{
    uint32_t sequence = 99;
    uint32_t responseCode = 99;
    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseResponse(
        "R42|00000000|accepted", sequence, responseCode));
    TEST_ASSERT_EQUAL_UINT32(42, sequence);
    TEST_ASSERT_EQUAL_HEX32(0x00000000UL, responseCode);

    TEST_ASSERT_TRUE(SmartSdrProtocolParser::parseResponse(
        "R7|5abcdef0", sequence, responseCode));
    TEST_ASSERT_EQUAL_UINT32(7, sequence);
    TEST_ASSERT_EQUAL_HEX32(0x5ABCDEF0UL, responseCode);

    sequence = 99;
    responseCode = 99;
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseResponse(
        "R+1|0", sequence, responseCode));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseResponse(
        "R 1|0", sequence, responseCode));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseResponse(
        "R1|+0", sequence, responseCode));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseResponse(
        "R1|100000000", sequence, responseCode));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseResponse(
        "R1|0bad-data", sequence, responseCode));
    TEST_ASSERT_FALSE(SmartSdrProtocolParser::parseResponse(
        "R1|", sequence, responseCode));
    TEST_ASSERT_EQUAL_UINT32(99, sequence);
    TEST_ASSERT_EQUAL_UINT32(99, responseCode);
}
} // namespace

void setup()
{
    delay(60000);
    UNITY_BEGIN();
    RUN_TEST(testNormalPartialLine);
    RUN_TEST(testEmptyCrLfAndBackToBackLines);
    RUN_TEST(testOverflowDiscardsThroughDelimiterAndRecovers);
    RUN_TEST(testResetLeavesDiscardMode);
    RUN_TEST(testProductionBufferBoundary);
    RUN_TEST(testUnsignedFieldValidation);
    RUN_TEST(testHexAndTextFieldValidation);
    RUN_TEST(testSliceNumberValidation);
    RUN_TEST(testFrequencyValidation);
    RUN_TEST(testResponseValidation);
    UNITY_END();
}

void loop()
{
}
