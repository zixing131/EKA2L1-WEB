#include <catch2/catch.hpp>
#include <services/centralrepo/centralrepo.h>
#include <services/context.h>

TEST_CASE("CenRep Set creates a missing integer and updates it in place", "[centralrepo]") {
    using namespace eka2l1;
    central_repo repo{};
    repo.default_meta = 0x1234;
    central_repo_client_subsession session;
    session.attach_repo = &repo;

    ipc_msg message(nullptr);
    message.function = cen_rep_set_int;
    message.args.args[0] = 0x1000;
    message.args.args[1] = 42;
    service::ipc_context context;
    context.msg = &message;
    context.auto_deref = false;
    session.set_value(&context);

    REQUIRE(repo.entries.size() == 1);
    REQUIRE(repo.find_entry(0x1000));
    CHECK(repo.find_entry(0x1000)->data.etype == central_repo_entry_type::integer);
    CHECK(repo.find_entry(0x1000)->data.intd == 42);
    CHECK(repo.find_entry(0x1000)->metadata_val == 0x1234);

    message.args.args[1] = static_cast<std::uint32_t>(-7);
    session.set_value(&context);
    REQUIRE(repo.entries.size() == 1);
    CHECK(static_cast<std::int32_t>(repo.find_entry(0x1000)->data.intd) == -7);
}

TEST_CASE("CenRep Set preserves an existing setting of a different type", "[centralrepo]") {
    using namespace eka2l1;
    central_repo repo{};
    central_repo_entry_variant value;
    value.etype = central_repo_entry_type::string;
    value.strd = "unchanged";
    REQUIRE(repo.add_new_entry(0x1000, value));
    central_repo_client_subsession session;
    session.attach_repo = &repo;
    ipc_msg message(nullptr);
    message.function = cen_rep_set_int;
    message.args.args[0] = 0x1000;
    message.args.args[1] = 42;
    service::ipc_context context;
    context.msg = &message;
    context.auto_deref = false;
    session.set_value(&context);
    CHECK(repo.find_entry(0x1000)->data.etype == central_repo_entry_type::string);
    CHECK(repo.find_entry(0x1000)->data.strd == "unchanged");
}
