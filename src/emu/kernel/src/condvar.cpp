/*
 * Copyright (c) 2022 EKA2L1 Team.
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

#include <kernel/condvar.h>
#include <kernel/scheduler.h>
#include <kernel/kernel.h>
#include <kernel/mutex.h>
#include <kernel/timing.h>
#include <common/log.h>
#include <functional>

#include <utils/err.h>

namespace eka2l1::kernel {
    // Is it too simple???
    condvar::condvar(kernel_system *kern, ntimer *timing, kernel::process *owner, std::string name, kernel::access_type access)
        : kernel_obj(kern, name, owner, access)
        , timing_(timing)
        , holding_(nullptr)
        , timeout_event_(-1) {
        obj_type = object_type::condvar;

        timeout_event_ = timing_->register_event(fmt::format("CondVar{}TimeoutEvt", unique_id()),
            [kern, id = unique_id()](std::uint64_t token, int late) {
                // The timer may have dequeued the callback before destruction.
                // Resolve under the kernel lock instead of retaining a raw this.
                const std::lock_guard<kernel_system> guard(*kern);
                if (auto *cv = kern->get_by_id<condvar>(id)) {
                    cv->on_timeout(token, late);
                }
            });
    }

    condvar::~condvar() {
        while (!waits.empty()) {
            thread *thr = waits.top();
            waits.pop();

            thr->get_scheduler()->dewait(thr);
        }

        while (!suspended.empty()) {
            thread *thr = E_LOFF(suspended.first()->deque(), thread, suspend_link);
            thr->get_scheduler()->dewait(thr);
        }

        for (const auto &[thr, token] : timing_out_thrs_) {
            timing_->unschedule_event(timeout_event_, token);
            thr->decrease_access_count();
        }

        timing_->remove_event(timeout_event_);
    }

    bool condvar::wait(thread *thr, mutex *mut, const int timeout) {
        // The thread should be in ready state
        if (thr->current_state() != kernel::thread_state::run) {
            LOG_ERROR(KERNEL, "Calling condvar wait in an abnormal state (not running!");
            return false;
        }

        // POSIX / J9: a condvar is not permanently bound to one mutex.
        // Concurrent waiters may even use different mutexes (each reacquires
        // its own). Only treat holding_ as "the last mutex used".
        if (waits.empty() && suspended.empty()) {
            holding_ = mut;
        } else if (!holding_) {
            holding_ = mut;
        }

        thr->condvar_mutex_ = mut;
        thr->condvar_lock_count_ = 0;

        // Object.wait() / RCondVar::Wait must drop the full recursive lock
        // count, then restore it on wake. One signal() leaves lock_count > 0
        // and the waiter still "owns" the mutex.
        if (mut->holding == thr) {
            thr->condvar_lock_count_ = mut->lock_count;
            mut->lock_count = 1;
            mut->signal(thr);
        } else {
            LOG_TRACE(KERNEL, "Condvar wait without owning mutex (thread={}, mutex={})",
                thr->name(), mut->name());
        }

        thr->get_scheduler()->wait(thr);

        thr->state = thread_state::wait_condvar;
        thr->wait_obj = this;

        if (timeout > 0) {
            const auto token = ++next_timeout_;
            timing_out_thrs_.emplace(thr, token);
            thr->increase_access_count();

            timing_->schedule_event(timeout, timeout_event_, token);
        }

        waits.emplace(thr);
        return true;
    }

    void condvar::on_timeout(std::uint64_t userdata, int late) {
        const auto pending = std::find_if(timing_out_thrs_.begin(), timing_out_thrs_.end(),
            [userdata](const auto &entry) { return entry.second == userdata; });
        // Signal can cancel a callback that is already waiting for the kernel
        // lock. Never let that old callback wake a later semaphore/mutex wait,
        // or even a newer wait on this same condvar.
        if (pending == timing_out_thrs_.end()) {
            return;
        }
        thread *thr = pending->first;
        if (thr->wait_obj != this || (thr->state != thread_state::wait_condvar
                && thr->state != thread_state::wait_condvar_suspend)) {
            timing_out_thrs_.erase(pending);
            thr->decrease_access_count();
            return;
        }

        if (thr->state == thread_state::wait_condvar_suspend) {
            thr->suspend_link.deque();
        } else {
            waits.remove(thr);
        }

        // Set the error code (register 0)
        thr->ctx.cpu_registers[0] = epoc::error_timed_out;
        unblock_thread(thr);

    }

    void condvar::unblock_thread(thread *thr) {
        mutex *mut = thr->condvar_mutex_ ? thr->condvar_mutex_ : holding_;
        const int saved_locks = thr->condvar_lock_count_;
        // Keep the saved mutex/count until actual acquisition. Another thread
        // may still own the mutex when Signal/Broadcast wakes this waiter.
        if (!mut || saved_locks <= 0) {
            thr->condvar_mutex_ = nullptr;
            thr->condvar_lock_count_ = 0;
        }
        thr->wait_obj = nullptr;
        const auto pending = timing_out_thrs_.find(thr);
        const bool had_timeout = pending != timing_out_thrs_.end();
        if (had_timeout) {
            timing_->unschedule_event(timeout_event_, pending->second);
            timing_out_thrs_.erase(pending);
        }

        if (!mut) {
            thr->get_scheduler()->dewait(thr);
            if (had_timeout) thr->decrease_access_count();
            return;
        }

        if (thr->state == thread_state::wait_condvar_suspend) {
            // You will still be in waiting state
            // NOTE: Right now it's put into mutex's suspend queue waiting for resume
            // It's not reclaim right away. I think this is correct though.
            mut->transfer_suspend_from_condvar(thr);
        } else {
            thr->get_scheduler()->dewait(thr);
            if (saved_locks > 0) {
                mut->wait(thr);
            }
        }

        if (waits.empty() && suspended.empty()) {
            holding_ = nullptr;
        }
        if (had_timeout) thr->decrease_access_count();
    }

    void condvar::signal() {
        thread *thr = nullptr;
        if (!waits.empty()) {
            thr = waits.top();
            waits.pop();
        } else if (!suspended.empty()) {
            thr = E_LOFF(suspended.first()->deque(), thread, suspend_link);      
        }

        if (thr) {
            unblock_thread(thr);
        } else {
            holding_ = nullptr;
        }
    }

    void condvar::broadcast() {
        while (!waits.empty() || !suspended.empty()) {
            signal();
        }
    }

    bool condvar::suspend_thread(thread *thr) {
        waits.remove(thr);
        suspended.push(&thr->suspend_link);

        thr->state = thread_state::wait_condvar_suspend;
        return true;
    }

    bool condvar::unsuspend_thread(thread *thr) {
        thr->suspend_link.deque();
        waits.push(thr);

        thr->state = thread_state::wait_condvar;
        return true;
    }

    void condvar::priority_change() {
        waits.resort();
    }
}
