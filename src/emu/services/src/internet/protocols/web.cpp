/* Copyright (c) 2026 EKA2L1 Team. GPL-3.0-or-later.
 * Browser TCP sockets shared by native Symbian and Java ME through ESock.
 */
#include <services/internet/protocols/common.h>
#include <services/internet/protocols/overall.h>
#include <services/socket/protocol.h>
#include <services/socket/server.h>
#include <kernel/kernel.h>
#include <common/cvt.h>
#include <common/log.h>
#include <utils/err.h>
#include <emscripten.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <unordered_map>

EM_JS(void, eka_web_connect, (unsigned id, const char *host, unsigned port), {
    Module.ekaNetwork.connect(id, UTF8ToString(host), port);
});
EM_JS(int, eka_web_send, (unsigned id, const unsigned char *data, unsigned size), {
    return Module.ekaNetwork.send(id, HEAPU8.subarray(data, data + size));
});
EM_JS(void, eka_web_close, (unsigned id), { Module.ekaNetwork.close(id); });

namespace eka2l1::epoc::internet {
    namespace {
        struct network_context {
            kernel_system *kern;
            // Proxy DNS: retain the original name so the relay resolves it on
            // its network. These benchmark-range addresses never leave here.
            std::unordered_map<std::string, std::uint32_t> names;
            std::unordered_map<std::uint32_t, std::string> addresses;

            std::uint32_t resolve(const std::string &name) {
                in_addr numeric{};
                if (inet_pton(AF_INET, name.c_str(), &numeric) == 1) return ntohl(numeric.s_addr);
                auto found = names.find(name);
                if (found != names.end()) return found->second;
                if (names.size() >= 65534) return 0;
                const std::uint32_t address = 0xC6120001 + names.size(); // 198.18.0.1/16
                names.emplace(name, address);
                addresses.emplace(address, name);
                return address;
            }

            std::string host(const epoc::socket::saddress &addr) const {
                char buffer[INET6_ADDRSTRLEN];
                if (addr.family_ == INET_ADDRESS_FAMILY) {
                    std::uint32_t value;
                    std::memcpy(&value, addr.user_data_, 4);
                    const auto found = addresses.find(value);
                    if (found != addresses.end()) return found->second;
                    in_addr numeric{ htonl(value) };
                    if (inet_ntop(AF_INET, &numeric, buffer, sizeof(buffer))) return buffer;
                } else if (addr.family_ == INET6_ADDRESS_FAMILY) {
                    // Symbian may promote an IPv4 result to IPv4-mapped IPv6.
                    const auto *b = addr.user_data_;
                    static const std::uint8_t prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
                    if (std::memcmp(b, prefix, 12) == 0) {
                        epoc::socket::saddress v4{};
                        v4.family_ = INET_ADDRESS_FAMILY;
                        const std::uint32_t value = (std::uint32_t(b[12]) << 24) | (std::uint32_t(b[13]) << 16)
                            | (std::uint32_t(b[14]) << 8) | b[15];
                        std::memcpy(v4.user_data_, &value, 4);
                        return host(v4);
                    }
                    if (inet_ntop(AF_INET6, addr.user_data_, buffer, sizeof(buffer))) return buffer;
                }
                return {};
            }
        };

        struct pending_request {
            kernel::uid thread = 0;
            eka2l1::ptr<epoc::request_status> status = 0;
            void set(const epoc::notify_info &info) { thread = info.requester->unique_id(); status = info.sts; }
            kernel::thread *owner(kernel_system *kern) const {
                if (!status) return nullptr;
                auto *thr = kern->get_by_id<kernel::thread>(thread);
                if (!thr || thr->get_exit_type() != kernel::entity_exit_type::pending) return nullptr;
                auto *request = status.get(thr->owning_process());
                return request && request->status == epoc::request_status::pending_status ? thr : nullptr;
            }
            void complete(kernel_system *kern, int error) {
                auto *thr = owner(kern);
                auto saved = status;
                status = 0;
                if (thr) { epoc::notify_info info(saved, thr); info.complete(error); }
            }
        };

        class web_resolver : public epoc::socket::host_resolver {
            std::shared_ptr<network_context> context_;
        public:
            explicit web_resolver(std::shared_ptr<network_context> context) : context_(std::move(context)) {}
            std::u16string host_name() const override { return u"eka2l1-web"; }
            bool host_name(const std::u16string &) override { return false; }
            void get_by_name(epoc::socket::name_entry *entry, epoc::notify_info &info) override {
                const auto name = common::ucs2_to_utf8(entry->name_.to_std_string(nullptr));
                if (name.empty() || name.size() > 253 || name.find_first_of(" /\\\r\n") != std::string::npos) {
                    info.complete(epoc::error_argument); return;
                }
                auto address = context_->resolve(name);
                if (!address) { info.complete(epoc::error_no_memory); return; }
                entry->addr_ = {};
                entry->addr_.family_ = INET_ADDRESS_FAMILY;
                std::memcpy(entry->addr_.user_data_, &address, 4);
                entry->length_ = (std::uint32_t(epoc::des_type::buf) << 28) | 12;
                entry->max_length_ = sizeof(epoc::socket::saddress);
                entry->flags_ = 0;
                info.complete(epoc::error_none);
            }
            void get_by_address(epoc::socket::saddress &addr, epoc::socket::name_entry *entry, epoc::notify_info &info) override {
                const auto name = context_->host(addr);
                if (name.empty()) { info.complete(epoc::error_not_found); return; }
                entry->name_.assign(nullptr, common::utf8_to_ucs2(name));
                entry->addr_ = addr;
                info.complete(epoc::error_none);
            }
        };

        class web_socket;
        std::unordered_map<std::uint32_t, web_socket *> active_sockets;
        std::uint32_t next_socket = 0;

        class web_socket : public epoc::socket::socket {
            std::shared_ptr<network_context> context_;
            std::uint32_t id_ = 0;
            bool connected_ = false;
            bool closed_ = false;
            int error_ = epoc::error_eof;
            epoc::socket::saddress remote_{};
            epoc::socket::saddress local_{};
            pending_request connect_, receive_;
            std::vector<std::uint8_t> bytes_;
            std::size_t offset_ = 0;
            std::uint8_t *destination_ = nullptr;
            std::uint32_t capacity_ = 0;
            std::uint32_t *received_ = nullptr;
            epoc::socket::saddress *receive_address_ = nullptr;
            bool partial_ = false;
            epoc::socket::receive_done_callback receive_callback_;

            void discard_transport() {
                if (id_) { active_sockets.erase(id_); eka_web_close(id_); id_ = 0; }
            }
            void drain() {
                if (!receive_.status) return;
                if (!receive_.owner(context_->kern)) {
                    receive_.status = 0; receive_callback_ = nullptr; return;
                }
                const auto available = bytes_.size() - offset_;
                if (available < capacity_ && !closed_ && !(partial_ && available)) return;
                const auto count = std::min<std::size_t>(available, capacity_);
                if (count) std::memcpy(destination_, bytes_.data() + offset_, count);
                offset_ += count;
                if (offset_ == bytes_.size()) { bytes_.clear(); offset_ = 0; }
                if (received_) *received_ = static_cast<std::uint32_t>(count);
                if (receive_address_) *receive_address_ = remote_;
                auto callback = std::move(receive_callback_);
                receive_callback_ = nullptr;
                if (callback) callback(count);
                receive_.complete(context_->kern, count || !capacity_ ? epoc::error_none : error_);
            }
        public:
            explicit web_socket(std::shared_ptr<network_context> context) : context_(std::move(context)) {
                local_.family_ = INET_ADDRESS_FAMILY;
            }
            ~web_socket() override {
                // Socket destruction also happens during kernel teardown: drop
                // callbacks without dereferencing any guest memory then.
                discard_transport();
            }
            kernel_system *kernel() const { return context_->kern; }
            void event(int type, const std::uint8_t *data, std::size_t size, int error) {
                if (type == 1) {
                    connected_ = true;
                    connect_.complete(context_->kern, epoc::error_none);
                } else if (type == 2) {
                    // Relay v1 cannot pause its TCP reader. Bound accumulated
                    // data and report overflow rather than silently losing bytes.
                    if (bytes_.size() - offset_ + size > 32 * 1024 * 1024) {
                        closed_ = true; error_ = epoc::error_overflow; discard_transport();
                    } else {
                        if (offset_ && offset_ >= bytes_.size() / 2) {
                            bytes_.erase(bytes_.begin(), bytes_.begin() + offset_); offset_ = 0;
                        }
                        if (size) bytes_.insert(bytes_.end(), data, data + size);
                    }
                } else if (type == 3 || type == 4) {
                    closed_ = true;
                    error_ = type == 3 ? epoc::error_eof : error;
                    connect_.complete(context_->kern, type == 3 ? epoc::error_could_not_connect : error);
                }
                drain();
            }
            void connect(const epoc::socket::saddress &addr, epoc::notify_info &info) override {
                if (connect_.status || connected_) { info.complete(epoc::error_in_use); return; }
                const auto host = context_->host(addr);
                if (host.empty() || !addr.port_ || addr.port_ > 65535) { info.complete(epoc::error_argument); return; }
                discard_transport();
                bytes_.clear(); offset_ = 0; closed_ = false; remote_ = addr;
                id_ = ++next_socket;
                local_.port_ = 49152 + id_ % 16384;
                active_sockets.emplace(id_, this);
                connect_.set(info);
                LOG_INFO(SERVICE_INTERNET, "Web TCP connect {}:{}", host, addr.port_);
                eka_web_connect(id_, host.c_str(), addr.port_);
            }
            void send(const std::uint8_t *data, std::uint32_t size, std::uint32_t *sent,
                const epoc::socket::saddress *, std::uint32_t flags, epoc::notify_info &info) override {
                int result = !connected_ || closed_ ? epoc::error_disconnected
                    : flags ? epoc::error_not_supported : eka_web_send(id_, data, size);
                if (sent) *sent = result == epoc::error_none ? size : 0;
                info.complete(result);
            }
            void receive(std::uint8_t *data, std::uint32_t size, std::uint32_t *received, epoc::socket::saddress *addr,
                std::uint32_t flags, epoc::notify_info &info, epoc::socket::receive_done_callback callback) override {
                if (receive_.status) { info.complete(epoc::error_in_use); return; }
                if (!connected_) { info.complete(epoc::error_not_ready); return; }
                if (flags & ~epoc::socket::SOCKET_FLAG_DONT_WAIT_FULL) { info.complete(epoc::error_not_supported); return; }
                receive_.set(info); destination_ = data; capacity_ = size; received_ = received;
                receive_address_ = addr; partial_ = flags & epoc::socket::SOCKET_FLAG_DONT_WAIT_FULL;
                receive_callback_ = std::move(callback);
                drain();
            }
            void bind(const epoc::socket::saddress &addr, epoc::notify_info &info) override {
                // Outbound relay sockets have no client-side listener/bind API.
                if (addr.port_) { info.complete(epoc::error_not_supported); return; }
                local_ = addr;
                info.complete(epoc::error_none);
            }
            std::int32_t local_name(epoc::socket::saddress &result, std::uint32_t &length) override {
                result = local_; length = local_.family_ == INET_ADDRESS_FAMILY ? 12 : 32; return epoc::error_none;
            }
            std::int32_t remote_name(epoc::socket::saddress &result, std::uint32_t &length) override {
                if (!connected_) return epoc::error_not_ready;
                result = remote_; length = remote_.family_ == INET_ADDRESS_FAMILY ? 12 : 32;
                return epoc::error_none;
            }
            void cancel_receive() override {
                receive_callback_ = nullptr;
                receive_.complete(context_->kern, epoc::error_cancel);
            }
            void cancel_connect() override {
                if (!connect_.status) return;
                discard_transport(); closed_ = true;
                connect_.complete(context_->kern, epoc::error_cancel);
            }
            void cancel_send() override {} // synchronous transport acceptance
            void cancel_accept() override {}
            void shutdown(epoc::notify_info &info, int) override {
                cancel_connect(); cancel_receive(); discard_transport(); closed_ = true; connected_ = false;
                info.complete(epoc::error_none);
            }
            void ioctl(std::uint32_t, epoc::notify_info &info, std::uint8_t *, std::size_t, std::size_t, std::uint32_t) override {
                info.complete(epoc::error_not_supported);
            }
            bool set_option(std::uint32_t id, std::uint32_t family, std::uint8_t *data, std::size_t size) override {
                if ((family == INET_TCP_SOCK_OPT_LEVEL && id == INET_TCP_NO_DELAY_OPT)
                    || (family == INET_IP_SOCK_OPT_LEVEL && id == INET_REUSE_ADDR)) return size >= 4;
                return epoc::socket::socket::set_option(id, family, data, size);
            }
        };

        class web_protocol : public epoc::socket::protocol {
            std::shared_ptr<network_context> context_;
        public:
            web_protocol(kernel_system *kern, bool oldarch) : epoc::socket::protocol(oldarch), context_(std::make_shared<network_context>()) {
                context_->kern = kern;
            }
            std::u16string name() const override { return u"tcp"; }
            std::vector<std::uint32_t> family_ids() const override { return { INET_ADDRESS_FAMILY, INET6_ADDRESS_FAMILY }; }
            std::vector<std::uint32_t> supported_ids() const override { return { INET_TCP_PROTOCOL_ID, INET_UDP_PROTOCOL_ID }; }
            epoc::version ver() const override { return { 1, 0, 0 }; }
            epoc::socket::byte_order get_byte_order() const override { return epoc::socket::byte_order_little_endian; }
            std::int32_t message_size() const override { return epoc::socket::SOCKET_MESSAGE_SIZE_IS_STREAM; }
            std::unique_ptr<epoc::socket::host_resolver> make_host_resolver(std::uint32_t, std::uint32_t) override {
                return std::make_unique<web_resolver>(context_);
            }
            std::unique_ptr<epoc::socket::socket> make_socket(std::uint32_t, std::uint32_t protocol_id, epoc::socket::socket_type type) override {
                if (protocol_id != INET_TCP_PROTOCOL_ID || type != epoc::socket::socket_type_stream) return nullptr;
                return std::make_unique<web_socket>(context_);
            }
            std::unique_ptr<epoc::socket::net_database> make_net_database(std::uint32_t, std::uint32_t) override { return nullptr; }
        };
    }

    void add_internet_stack_protocols(socket_server *server, bool oldarch) {
        std::unique_ptr<epoc::socket::protocol> protocol = std::make_unique<web_protocol>(server->get_kernel_object_owner(), oldarch);
        server->add_protocol(protocol);
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void eka_web_network_event(unsigned id, int event,
        const std::uint8_t *data, unsigned size, int error) {
        auto found = active_sockets.find(id);
        if (found == active_sockets.end()) return;
        auto *socket = found->second;
        const std::lock_guard<kernel_system> guard(*socket->kernel());
        socket->event(event, data, size, error);
    }
}
