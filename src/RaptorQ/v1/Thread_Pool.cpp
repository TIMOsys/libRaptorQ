/*
 * Copyright (c) 2015-2016, Luca Fulchir<luca@fulchir.it>, All rights reserved.
 *
 * This file is part of "libRaptorQ".
 *
 * libRaptorQ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * libRaptorQ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * and a copy of the GNU Lesser General Public License
 * along with libRaptorQ.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "RaptorQ/v1/Thread_Pool.hpp"

namespace RFC6330__v1 {
namespace Impl {

Thread_Pool& Thread_Pool::get()
{
    //#pragma GCC diagnostic push
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wexit-time-destructors"
    #pragma clang diagnostic ignored "-Wglobal-constructors"
    static Thread_Pool _pool;
    #pragma clang diagnostic pop
    //#pragma GCC diagnostic pop
    return _pool;
}

Thread_Pool::Thread_Pool()
{
	resize_pool (std::thread::hardware_concurrency(),
									RaptorQ__v1::Work_State::ABORT_COMPUTATION);
}

size_t Thread_Pool::size()
{
	return _pool.size();
}

Thread_Pool::~Thread_Pool()
{
	std::unique_lock<std::mutex> _data_lock (_data_mtx);
	_queue.clear();
	_data_lock.unlock();

	resize_pool (0, RaptorQ__v1::Work_State::ABORT_COMPUTATION);

	_cond.notify_all();
	std::unique_lock<std::mutex> pool_lock (_pool_mtx);
	while (_pool.size() != 0 || _exiting.size() != 0)
		_cond.wait (pool_lock);
}

void Thread_Pool::resize_pool (const size_t size,
										const RaptorQ__v1::Work_State exit_t)
{
    if (size == _pool.size())
        return;
    std::unique_lock<std::mutex> _lock_pool (_pool_mtx);

    while (_pool.size() > size) {
		std::lock_guard<std::mutex> _guard_data (_data_mtx);
		RQ_UNUSED(_guard_data);

		auto _it = _pool.begin();
		while (_it != _pool.end() && _pool.size() > size) {
			// delete a thread that is still waiting
			std::weak_ptr<Work_State_Overlay> _sec = _it->second;
			std::shared_ptr<Work_State_Overlay> _state = _sec.lock();
			// auto state = it.base()->second.lock();
			if (_state == nullptr) {
				// the thread has surely stopped. delete it.
				// we should never get here anyway
				assert(false && "RQ: thread0: we should have not goten here!");
				_it = _pool.erase (_it);
			} else {
				if (Work_State_Overlay::WAITING == *_state) {
					th_state pair = {std::thread(),
								std::shared_ptr<Work_State_Overlay> (nullptr)};
					pair.swap (*_it);
					_it = _pool.erase (_it);
					*_state = static_cast<Work_State_Overlay> (exit_t);
					_exiting.emplace_back (std::move(pair));
				} else {
                    ++_it;
                }
			}
		}
		if (_pool.size() > size && _it == _pool.end()) {
			// all threads are busy, but we must terminate one :(
			auto _end = _pool.begin();
			//auto state = end->second.lock();
			std::weak_ptr<Work_State_Overlay> _sec = _end->second;
			std::shared_ptr<Work_State_Overlay> _state = _sec.lock();
			if (_state == nullptr) {
				// the thread has surely stopped. delete it.
				// we should never get here anyway
				assert(false && "RQ: thread1: we should have not goten here!");
				_pool.erase (_end);
			} else {
				th_state pair = {std::thread(),
								std::shared_ptr<Work_State_Overlay> (nullptr)};
				pair.swap (*_end);
				_pool.erase (_end);
				*_state = static_cast<Work_State_Overlay> (exit_t);
				_exiting.emplace_back (std::move(pair));
			}
		}
    }
    while (_pool.size() < size) {
		auto state = std::make_shared<Work_State_Overlay> (
											Work_State_Overlay::KEEP_WORKING);
		_pool.emplace_back (std::thread (working_thread, this, state),
									std::weak_ptr<Work_State_Overlay> (state));
    }
	_lock_pool.unlock();
	_cond.notify_all();
}

bool Thread_Pool::add_work (std::unique_ptr<Pool_Work> work)
{
    std::unique_lock<std::mutex> _lock_data (_data_mtx);
	if (_pool.size() == 0)
        return false;

    _queue.emplace_back (std::move(work));
    _lock_data.unlock();
    _cond.notify_all();

    return true;
}

void Thread_Pool::working_thread (Thread_Pool *obj,
									std::shared_ptr<Work_State_Overlay> state)
{
	while (Work_State_Overlay::KEEP_WORKING == *state) {
		std::unique_lock<std::mutex> lock_data (obj->_data_mtx);
        if (Work_State_Overlay::KEEP_WORKING != *state) {
            lock_data.unlock();
			break;
        }
		if (obj->_queue.size() == 0) {
			*state = Work_State_Overlay::WAITING;
			obj->_cond.wait (lock_data);
            if (Work_State_Overlay::WAITING != *state) {    // => abort
                lock_data.unlock();
				break;
            }
			*state = Work_State_Overlay::KEEP_WORKING;
            lock_data.unlock();
            continue;
		}

		std::unique_ptr<Pool_Work> my_work;
        my_work.swap (obj->_queue.front());
		obj->_queue.pop_front();
		lock_data.unlock();

		auto exit_stat = my_work->do_work (
					reinterpret_cast<RaptorQ__v1::Work_State *> (state.get()));

		switch (exit_stat) {
		case Work_Exit_Status::DONE:
			break;
		case Work_Exit_Status::STOPPED:
			lock_data.lock();
			obj->_queue.push_front (std::move(my_work));
            lock_data.unlock();
			obj->_cond.notify_all();
			break;
		case Work_Exit_Status::REQUEUE:
            Thread_Pool::get().add_work (std::move(my_work));
			break;
		}
	}
	// delete ourselves from the thread queue.
	std::unique_lock<std::mutex> _lock_pool (obj->_pool_mtx);

	for (auto _th = obj->_pool.begin(); _th != obj->_pool.end(); ++_th) {
		if (_th->first.get_id() == std::this_thread::get_id()) {
			_th->first.detach();
			obj->_pool.erase (_th);
            _lock_pool.unlock();
			obj->_cond.notify_all();
			return;
		}
	}
	// were we moved to the exiting queue?
	for (auto _th = obj->_exiting.begin(); _th != obj->_exiting.end(); ++_th) {
		if (_th->first.get_id() == std::this_thread::get_id()) {
			_th->first.detach();
			obj->_exiting.erase (_th);
            _lock_pool.unlock();
			obj->_cond.notify_all();
			return;
		}
	}
}

} // namespace Impl
} // namespave RaptorQ__v1
