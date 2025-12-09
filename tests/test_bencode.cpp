#include <gtest/gtest.h>
#include "bencode.h"
#include <vector>

using namespace torrent;

class BencodeParserTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== Integer Tests ====================

TEST_F(BencodeParserTest, ParsePositiveInteger) {
    auto result = BencodeParser::parse("i42e");
    ASSERT_TRUE(result.isInteger());
    EXPECT_EQ(result.getInteger(), 42);
}

TEST_F(BencodeParserTest, ParseNegativeInteger) {
    auto result = BencodeParser::parse("i-42e");
    ASSERT_TRUE(result.isInteger());
    EXPECT_EQ(result.getInteger(), -42);
}

TEST_F(BencodeParserTest, ParseZero) {
    auto result = BencodeParser::parse("i0e");
    ASSERT_TRUE(result.isInteger());
    EXPECT_EQ(result.getInteger(), 0);
}

TEST_F(BencodeParserTest, ParseLargeInteger) {
    auto result = BencodeParser::parse("i9223372036854775807e");
    ASSERT_TRUE(result.isInteger());
    EXPECT_EQ(result.getInteger(), 9223372036854775807LL);
}

TEST_F(BencodeParserTest, ParseIntegerWithLeadingZeroThrows) {
    EXPECT_THROW(BencodeParser::parse("i042e"), BencodeError);
}

TEST_F(BencodeParserTest, ParseNegativeZeroThrows) {
    EXPECT_THROW(BencodeParser::parse("i-0e"), BencodeError);
}

TEST_F(BencodeParserTest, ParseEmptyIntegerThrows) {
    EXPECT_THROW(BencodeParser::parse("ie"), BencodeError);
}

TEST_F(BencodeParserTest, ParseInvalidIntegerThrows) {
    EXPECT_THROW(BencodeParser::parse("i12a34e"), BencodeError);
}

// ==================== String Tests ====================

TEST_F(BencodeParserTest, ParseSimpleString) {
    auto result = BencodeParser::parse("4:spam");
    ASSERT_TRUE(result.isString());
    EXPECT_EQ(result.getString(), "spam");
}

TEST_F(BencodeParserTest, ParseEmptyString) {
    auto result = BencodeParser::parse("0:");
    ASSERT_TRUE(result.isString());
    EXPECT_EQ(result.getString(), "");
}

TEST_F(BencodeParserTest, ParseLongString) {
    std::string long_str(1000, 'x');
    std::string encoded = "1000:" + long_str;
    auto result = BencodeParser::parse(encoded);
    ASSERT_TRUE(result.isString());
    EXPECT_EQ(result.getString(), long_str);
}

TEST_F(BencodeParserTest, ParseBinaryString) {
    std::string binary_data = "5:\x00\x01\x02\x03\x04";
    auto result = BencodeParser::parse(binary_data);
    ASSERT_TRUE(result.isString());
    EXPECT_EQ(result.getString().size(), 5);
    EXPECT_EQ(result.getString()[0], '\x00');
    EXPECT_EQ(result.getString()[4], '\x04');
}

TEST_F(BencodeParserTest, ParseStringLengthExceedsDataThrows) {
    EXPECT_THROW(BencodeParser::parse("10:short"), BencodeError);
}

TEST_F(BencodeParserTest, ParseStringInvalidLengthThrows) {
    EXPECT_THROW(BencodeParser::parse("abc:test"), BencodeError);
}

// ==================== List Tests ====================

TEST_F(BencodeParserTest, ParseEmptyList) {
    auto result = BencodeParser::parse("le");
    ASSERT_TRUE(result.isList());
    EXPECT_EQ(result.getList().size(), 0);
}

TEST_F(BencodeParserTest, ParseSimpleList) {
    auto result = BencodeParser::parse("l4:spami42ee");
    ASSERT_TRUE(result.isList());
    const auto& list = result.getList();
    ASSERT_EQ(list.size(), 2);
    EXPECT_TRUE(list[0].isString());
    EXPECT_EQ(list[0].getString(), "spam");
    EXPECT_TRUE(list[1].isInteger());
    EXPECT_EQ(list[1].getInteger(), 42);
}

TEST_F(BencodeParserTest, ParseNestedList) {
    auto result = BencodeParser::parse("ll4:spam3:eggee");
    ASSERT_TRUE(result.isList());
    const auto& outer = result.getList();
    ASSERT_EQ(outer.size(), 1);
    ASSERT_TRUE(outer[0].isList());
    const auto& inner = outer[0].getList();
    ASSERT_EQ(inner.size(), 2);
    EXPECT_EQ(inner[0].getString(), "spam");
    EXPECT_EQ(inner[1].getString(), "egg");
}

TEST_F(BencodeParserTest, ParseListOfIntegers) {
    auto result = BencodeParser::parse("li1ei2ei3ee");
    ASSERT_TRUE(result.isList());
    const auto& list = result.getList();
    ASSERT_EQ(list.size(), 3);
    EXPECT_EQ(list[0].getInteger(), 1);
    EXPECT_EQ(list[1].getInteger(), 2);
    EXPECT_EQ(list[2].getInteger(), 3);
}

// ==================== Dictionary Tests ====================

TEST_F(BencodeParserTest, ParseEmptyDictionary) {
    auto result = BencodeParser::parse("de");
    ASSERT_TRUE(result.isDictionary());
    EXPECT_EQ(result.getDictionary().size(), 0);
}

TEST_F(BencodeParserTest, ParseSimpleDictionary) {
    auto result = BencodeParser::parse("d3:bar4:spam3:fooi42ee");
    ASSERT_TRUE(result.isDictionary());
    const auto& dict = result.getDictionary();
    ASSERT_EQ(dict.size(), 2);
    EXPECT_TRUE(dict.at("bar").isString());
    EXPECT_EQ(dict.at("bar").getString(), "spam");
    EXPECT_TRUE(dict.at("foo").isInteger());
    EXPECT_EQ(dict.at("foo").getInteger(), 42);
}

TEST_F(BencodeParserTest, ParseNestedDictionary) {
    auto result = BencodeParser::parse("d5:innerd3:keyi123eee");
    ASSERT_TRUE(result.isDictionary());
    const auto& outer = result.getDictionary();
    ASSERT_EQ(outer.size(), 1);
    ASSERT_TRUE(outer.at("inner").isDictionary());
    const auto& inner = outer.at("inner").getDictionary();
    ASSERT_EQ(inner.size(), 1);
    EXPECT_EQ(inner.at("key").getInteger(), 123);
}

TEST_F(BencodeParserTest, ParseDictionaryWithListValue) {
    auto result = BencodeParser::parse("d4:listli1ei2ei3eee");
    ASSERT_TRUE(result.isDictionary());
    const auto& dict = result.getDictionary();
    ASSERT_TRUE(dict.at("list").isList());
    const auto& list = dict.at("list").getList();
    ASSERT_EQ(list.size(), 3);
    EXPECT_EQ(list[0].getInteger(), 1);
}

// ==================== Encoding Tests ====================

TEST_F(BencodeParserTest, EncodeInteger) {
    BencodeValue value(42);
    std::string encoded = BencodeParser::encode(value);
    EXPECT_EQ(encoded, "i42e");
}

TEST_F(BencodeParserTest, EncodeString) {
    BencodeValue value(std::string("spam"));
    std::string encoded = BencodeParser::encode(value);
    EXPECT_EQ(encoded, "4:spam");
}

TEST_F(BencodeParserTest, EncodeEmptyString) {
    BencodeValue value(std::string(""));
    std::string encoded = BencodeParser::encode(value);
    EXPECT_EQ(encoded, "0:");
}

TEST_F(BencodeParserTest, EncodeList) {
    BencodeValue::List list;
    list.push_back(BencodeValue(std::string("spam")));
    list.push_back(BencodeValue(42));
    BencodeValue value(list);
    std::string encoded = BencodeParser::encode(value);
    EXPECT_EQ(encoded, "l4:spami42ee");
}

TEST_F(BencodeParserTest, EncodeDictionary) {
    BencodeValue::Dictionary dict;
    dict["foo"] = BencodeValue(42);
    dict["bar"] = BencodeValue(std::string("spam"));
    BencodeValue value(dict);
    std::string encoded = BencodeParser::encode(value);
    // Dictionary keys are ordered
    EXPECT_EQ(encoded, "d3:bar4:spam3:fooi42ee");
}

// ==================== Round-trip Tests ====================

TEST_F(BencodeParserTest, RoundTripInteger) {
    std::string original = "i-999e";
    auto parsed = BencodeParser::parse(original);
    std::string encoded = BencodeParser::encode(parsed);
    EXPECT_EQ(encoded, original);
}

TEST_F(BencodeParserTest, RoundTripString) {
    std::string original = "11:hello world";
    auto parsed = BencodeParser::parse(original);
    std::string encoded = BencodeParser::encode(parsed);
    EXPECT_EQ(encoded, original);
}

TEST_F(BencodeParserTest, RoundTripComplexStructure) {
    std::string original = "d4:listli1ei2ei3ee6:stringi4:teste";
    auto parsed = BencodeParser::parse(original);
    std::string encoded = BencodeParser::encode(parsed);
    // Parse again to verify correctness
    auto reparsed = BencodeParser::parse(encoded);
    ASSERT_TRUE(reparsed.isDictionary());
}

// ==================== Binary Data Tests ====================

TEST_F(BencodeParserTest, ParseFromBinaryVector) {
    std::vector<uint8_t> data = {'i', '4', '2', 'e'};
    auto result = BencodeParser::parse(data);
    ASSERT_TRUE(result.isInteger());
    EXPECT_EQ(result.getInteger(), 42);
}

TEST_F(BencodeParserTest, ParseBinaryDataInString) {
    std::vector<uint8_t> binary = {0x00, 0xFF, 0x7F, 0x80, 0x01};
    std::string encoded = "5:";
    encoded.append(reinterpret_cast<char*>(binary.data()), binary.size());
    auto result = BencodeParser::parse(encoded);
    ASSERT_TRUE(result.isString());
    EXPECT_EQ(result.getString().size(), 5);
    EXPECT_EQ(static_cast<uint8_t>(result.getString()[0]), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(result.getString()[1]), 0xFF);
}

// ==================== Error Handling Tests ====================

TEST_F(BencodeParserTest, ParseUnexpectedEndOfData) {
    EXPECT_THROW(BencodeParser::parse("i42"), BencodeError);
}

TEST_F(BencodeParserTest, ParseInvalidCharacter) {
    EXPECT_THROW(BencodeParser::parse("x"), BencodeError);
}

TEST_F(BencodeParserTest, ParseUnterminatedList) {
    EXPECT_THROW(BencodeParser::parse("li1ei2e"), BencodeError);
}

TEST_F(BencodeParserTest, ParseUnterminatedDictionary) {
    EXPECT_THROW(BencodeParser::parse("d3:fooi42e"), BencodeError);
}

// ==================== Type Checker Tests ====================

TEST_F(BencodeParserTest, TypeCheckersWork) {
    BencodeValue int_val(42);
    EXPECT_TRUE(int_val.isInteger());
    EXPECT_FALSE(int_val.isString());
    EXPECT_FALSE(int_val.isList());
    EXPECT_FALSE(int_val.isDictionary());

    BencodeValue str_val(std::string("test"));
    EXPECT_FALSE(str_val.isInteger());
    EXPECT_TRUE(str_val.isString());
    EXPECT_FALSE(str_val.isList());
    EXPECT_FALSE(str_val.isDictionary());
}

// ==================== Main ====================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
