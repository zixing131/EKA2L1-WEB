/*
 * Copyright (c) 2020 EKA2L1 Team.
 * 
 * This file is part of EKA2L1 project.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <services/etel/subsess.h>
#include <services/context.h>
#include <common/log.h>
#include <utils/err.h>

namespace eka2l1 {
    etel_ussd_subsession::~etel_ussd_subsession() {
        receive_nof_.complete(epoc::error_cancel);
        release_nof_.complete(epoc::error_cancel);
    }

    void etel_ussd_subsession::dispatch(service::ipc_context *ctx) {
        switch (ctx->msg->function) {
        case 20110: { // RMobileUssdMessaging::GetCaps
            struct ussd_caps { std::uint32_t extension, format, types; };
            auto caps = ctx->get_argument_data_from_descriptor<ussd_caps>(0);
            if (!caps) { ctx->complete(epoc::error_argument); return; }
            // These are API capabilities, independent of radio registration.
            // PhoneServer constructs its send handler even in offline mode.
            caps->format = 1; // KCapsPackedString
            caps->types = 3; // KCapsMOUssd | KCapsMTUssd
            ctx->write_data_to_descriptor_argument(0, *caps);
            ctx->complete(epoc::error_none);
            break;
        }
        case 29003: // ReceiveMessage
            receive_nof_ = epoc::notify_info(ctx->msg->request_sts, ctx->msg->own_thr);
            break;
        case 20111: // NotifyNetworkRelease
            release_nof_ = epoc::notify_info(ctx->msg->request_sts, ctx->msg->own_thr);
            break;
        case 29503:
            receive_nof_.complete(epoc::error_cancel);
            ctx->complete(epoc::error_none);
            break;
        case 20611:
            release_nof_.complete(epoc::error_cancel);
            ctx->complete(epoc::error_none);
            break;
        case 27006: // SendRelease
        case 28001: // SendMessage
        case 28002: // SendMessageNoFdnCheck
            ctx->complete(epoc::error_not_ready); // No registered modem.
            break;
        case 27506:
        case 28501:
        case 28502:
            ctx->complete(epoc::error_none);
            break;
        default:
            ctx->complete(epoc::error_not_supported);
            break;
        }
    }

    etel_call_subsession::~etel_call_subsession() {
        for (auto &[opcode, notification] : notifications_) {
            notification.complete(epoc::error_cancel);
        }
    }

    void etel_call_subsession::dispatch(service::ipc_context *ctx) {
        const int op = ctx->msg->function;
        if (op == 19 || op == 20005 || op == 13 || op == 15 || op == 18 || op == 20007) {
            // Empty call slots are idle and have no active call capabilities.
            const std::uint32_t value = (op == 19 || op == 20005) ? 1 : 0;
            ctx->write_data_to_descriptor_argument<std::uint32_t>(0, value);
            ctx->complete(epoc::error_none);
        } else if (op == 20004) {
            struct mobile_call_caps { std::uint32_t extension, control, events; };
            auto caps = ctx->get_argument_data_from_descriptor<mobile_call_caps>(0);
            if (!caps) {
                ctx->complete(epoc::error_argument);
                return;
            }
            caps->control = caps->events = 0;
            ctx->write_data_to_descriptor_argument(0, *caps);
            ctx->complete(epoc::error_none);
        } else if (op == 10 || op == 20 || op == 22 || op == 24 || (op >= 20008 && op <= 20016) || op == 24007) {
            notifications_[op] = epoc::notify_info(ctx->msg->request_sts, ctx->msg->own_thr);
        } else if (op == 11 || op == 21 || op == 23 || op == 25 || (op >= 20508 && op <= 20516) || op == 24507) {
            const int original = op < 100 ? op - 1 : op - 500;
            auto entry = notifications_.find(original);
            if (entry != notifications_.end()) {
                entry->second.complete(epoc::error_cancel);
                notifications_.erase(entry);
            }
            ctx->complete(epoc::error_none);
        } else {
            LOG_WARN(SERVICE_ETEL, "Unsupported call telephony opcode {}", op);
            ctx->complete(epoc::error_not_supported);
        }
    }

    etel_conference_subsession::~etel_conference_subsession() {
        for (auto &notification : notifications_) {
            notification.complete(epoc::error_cancel);
        }
    }

    void etel_conference_subsession::dispatch(service::ipc_context *ctx) {
        const int opcode = ctx->msg->function;
        if (opcode >= 20017 && opcode <= 20019) {
            // No calls: zero members, no available conference operations,
            // and EConferenceIdle. All three getters return a 32-bit value.
            ctx->write_data_to_descriptor_argument<std::uint32_t>(0, 0);
            ctx->complete(epoc::error_none);
        } else if (opcode >= 20020 && opcode <= 20022) {
            notifications_[opcode - 20020] = epoc::notify_info(ctx->msg->request_sts, ctx->msg->own_thr);
        } else if (opcode >= 20520 && opcode <= 20522) {
            notifications_[opcode - 20520].complete(epoc::error_cancel);
            ctx->complete(epoc::error_none);
        } else {
            LOG_WARN(SERVICE_ETEL, "Unsupported conference telephony opcode {}", opcode);
            ctx->complete(epoc::error_not_supported);
        }
    }

    void etel_custom_subsession::dispatch(service::ipc_context *ctx) {
        LOG_WARN(SERVICE_ETEL, "Unsupported custom telephony opcode {}", ctx->msg->function);
        ctx->complete(epoc::error_not_supported);
    }

    etel_subsession::etel_subsession(etel_session *session, const etel_legacy_level lvl)
        : session_(session)
        , legacy_level_(lvl) {
    }
}
