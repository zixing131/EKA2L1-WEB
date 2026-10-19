#include <catch2/catch.hpp>
#include <j2me/common.h>

TEST_CASE("JAD synthesis unfolds manifest attributes without changing their bytes", "[j2me][manifest]") {
    const auto jad = eka2l1::j2me::jad_from_manifest(
        "MIDlet-Name: Music\r\n Box\r\n"
        "MIDlet-Permissions: javax.microedition.io.Connector.http,javax.microedit\r\n"
        " ion.io.Connector.file.read,javax.microedition.io.Connector.file.write\r\n"
        "MIDlet-Description: UTF-8 \xC2\r\n \xB7\r\n\r\n", "m.jar", 323139);
    REQUIRE(jad == "MIDlet-Name: MusicBox\r\n"
        "MIDlet-Permissions: javax.microedition.io.Connector.http,javax.microedition.io.Connector.file.read,javax.microedition.io.Connector.file.write\r\n"
        "MIDlet-Description: UTF-8 \xC2\xB7\r\n"
        "MIDlet-Jar-URL: m.jar\r\nMIDlet-Jar-Size: 323139\r\n");
}

TEST_CASE("JAD synthesis uses only suite attributes and the actual archive location", "[j2me][manifest]") {
    for (const std::string newline : {"\n", "\r", "\r\n"}) {
        const auto jad = eka2l1::j2me::jad_from_manifest(
            "MIDlet-Name: App" + newline + "MIDlet-Jar-URL: stale.jar" + newline
                + "MIDlet-Jar-Size: 1" + newline + newline
                + "Name: resource.png" + newline + "SHA-256-Digest: hash" + newline,
            "m.jar", 42);
        REQUIRE(jad == "MIDlet-Name: App\r\nMIDlet-Jar-URL: m.jar\r\nMIDlet-Jar-Size: 42\r\n");
    }
    REQUIRE(eka2l1::j2me::jad_from_manifest("MIDlet-Name: App", "m.jar", 42)
        == "MIDlet-Name: App\r\nMIDlet-Jar-URL: m.jar\r\nMIDlet-Jar-Size: 42\r\n");
}
