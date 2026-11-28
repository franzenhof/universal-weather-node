#pragma once
// The alarm notification queue (alarms_run.cpp): a ring of N messages, the
// oldest dropped when it is full. Every message gets a sequence number: the
// sender (upload task) works on a copy without the lock, and writes its
// progress back only if the head is still that message - a message dropped
// (queue full) while it was being sent can never hand its channels / attempts
// to the next one, nor pop it unsent. The caller holds the alarms lock for
// every call. Host-tested in test/firmware_alarmqueue.cpp.
#include <stdint.h>

template <typename Msg, int N>
class AlarmQueue {
public:
    // A new, cleared message at the tail (the caller fills it); with the queue
    // full the oldest message is dropped first (dropped = true). The caller
    // pushes only a message it will send: a push always queues.
    Msg& push(bool& dropped)
    {
        dropped = count_ == N;
        if (dropped) popFront();
        const int i = (head_ + count_++) % N;
        items_[i] = Msg();
        seq_[i] = ++nextSeq_;
        return items_[i];
    }

    int size() const { return count_; }
    // a copy of the oldest message and its identity; false = empty
    bool front(Msg& out, uint32_t& id) const
    {
        if (!count_) return false;
        out = items_[head_];
        id = seq_[head_];
        return true;
    }
    // the oldest message, if it is still the one with this identity
    Msg* frontIfStill(uint32_t id) { return count_ && seq_[head_] == id ? &items_[head_] : nullptr; }
    void popFront() { if (count_) { head_ = (head_ + 1) % N; count_--; } }

private:
    Msg      items_[N];
    uint32_t seq_[N] = {};
    int      head_ = 0, count_ = 0;
    uint32_t nextSeq_ = 0;
};
