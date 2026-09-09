#include <catch2/catch.hpp>
#include <services/window/window.h>

TEST_CASE("key captures select the highest matching signed priority in a heap", "[key-capture]") {
    using namespace eka2l1;
    using namespace eka2l1::epoc;
    cp_queue<event_capture_key_notifier> requests;
    CHECK(find_key_capture(requests, event_key_capture_type::normal, 0) == nullptr);
    auto add = [&](int priority, unsigned id, event_key_capture_type type, unsigned mask, unsigned modifiers) {
        event_capture_key_notifier request{};
        request.user = reinterpret_cast<epoc::window *>(std::uintptr_t(0x1000 + id));
        request.pri_ = priority;
        request.id = id;
        request.type_ = type;
        request.modifiers_mask_ = mask;
        request.modifiers_ = modifiers;
        requests.push(request);
    };
    add(-1, 1, event_key_capture_type::normal, 0, 0);
    add(2, 2, event_key_capture_type::normal, 0, 0);
    add(100, 3, event_key_capture_type::normal, 1, 1);
    add(20, 4, event_key_capture_type::up_and_downs, 0, 0);
    CHECK(find_key_capture(requests, event_key_capture_type::normal, 0)->id == 2);
    CHECK(find_key_capture(requests, event_key_capture_type::normal, 1)->id == 3);
    CHECK(find_key_capture(requests, event_key_capture_type::up_and_downs, 0)->id == 4);
    add(2, 5, event_key_capture_type::normal, 0, 0);
    CHECK(find_key_capture(requests, event_key_capture_type::normal, 0)->id == 5);
}
