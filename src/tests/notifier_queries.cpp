#include <catch2/catch.hpp>
#include <services/notifier/queries.h>

using namespace eka2l1::epoc::notifier;

static std::vector<std::uint8_t> unhex(const std::string &hex) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

TEST_CASE("5320 Java network permission query preserves text and softkeys", "[notifier]") {
    // Captured CAknGlobalMsgQuery packet, through the complete header descriptor.
    auto bytes = unhex("BA6B1100D101CC0880448144000043001902416C6C6F77206170706C69636174696F6E204D75736963426F7820746F20757365206E6574776F726B20616E642073656E64206F72207265636569766520646174613F1100445365637572697479207761726E696E673A");
    global_message_query query;
    REQUIRE(parse_global_message_query(bytes.data(), bytes.size(), query));
    CHECK(query.softkeys == 0x08CC01D1);
    CHECK(query.text == u"Allow application MusicBox to use network and send or receive data?");
    CHECK(query.header == u"Security warning:");
    for (std::size_t n = 0; n < bytes.size(); n++)
        CHECK_FALSE(parse_global_message_query(bytes.data(), n, query));
    bytes[14] = 68; // Outer and streamed descriptor lengths must agree.
    CHECK_FALSE(parse_global_message_query(bytes.data(), bytes.size(), query));
    bytes[14] = 67;
    bytes[0] = 0;
    CHECK_FALSE(parse_global_message_query(bytes.data(), bytes.size(), query));
}

TEST_CASE("Global queries decode SCSU rather than treating it as UTF-8", "[notifier]") {
    auto bytes = unhex("BA6B11002400CC080000000000000100040E4E2D000000");
    global_message_query query;
    REQUIRE(parse_global_message_query(bytes.data(), bytes.size(), query));
    CHECK(query.text == u"中");
    CHECK(query.header.empty());
}

TEST_CASE("Avkon CBA preserves command IDs, signed cancel, and UTF-16 alignment", "[notifier]") {
    // LONG flags, WORD breadth, LLINK related resource, WORD count, CBA_BUTTONs.
    // Includes a third (middle) softkey and KAknCbaVersion long-press command.
    auto bytes = unhex("00000000000078563412030000BD0B0359006500730000FFFFffff"
        "0AFFff000002004E006F0000FFFFffff00BD0B0000FFFFffff");
    std::vector<query_button> buttons;
    REQUIRE(parse_query_buttons(bytes, buttons));
    REQUIRE(buttons.size() == 2);
    CHECK(buttons[0].command == 3005);
    CHECK(buttons[0].text == u"Yes");
    CHECK(buttons[1].command == -1);
    CHECK(buttons[1].text == u"No");
    bytes.pop_back();
    CHECK_FALSE(parse_query_buttons(bytes, buttons));
    bytes[12] = 99;
    CHECK_FALSE(parse_query_buttons(bytes, buttons));
}
