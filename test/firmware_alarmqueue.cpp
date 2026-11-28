// Alarm notification queue (src/alarm_queue.h): order, drop of the oldest,
// write-back only to the message that was being sent.
#include "alarm_queue.h"
#include <cassert>

struct Msg { int text = 0; uint8_t mask = 0; uint8_t attempts = 0; };

static void add(AlarmQueue<Msg, 3>& q, int text, bool expectDrop)
{
    bool dropped = true;
    Msg& m = q.push(dropped);
    assert(dropped == expectDrop && m.text == 0 && m.mask == 0);
    m.text = text; m.mask = 0x3;
}

int main()
{
    AlarmQueue<Msg, 3> q;
    Msg m;
    uint32_t id = 0;
    assert(!q.front(m, id) && q.size() == 0 && !q.frontIfStill(1));
    add(q, 1, false); add(q, 2, false); add(q, 3, false);
    assert(q.size() == 3 && q.front(m, id) && m.text == 1);

    // the sender works on its copy of message 1; meanwhile a new event drops message 1
    const uint32_t sending = id;
    add(q, 4, true);
    assert(q.size() == 3);
    // its result must not land on message 2 (it keeps both channels and is not popped)
    assert(!q.frontIfStill(sending));
    assert(q.front(m, id) && m.text == 2 && m.mask == 0x3 && id != sending);

    // normal progress: one channel done, then the last one -> popped
    Msg* head = q.frontIfStill(id);
    assert(head && head->text == 2);
    head->mask = 0x2; head->attempts = 1;
    assert(q.front(m, id) && m.mask == 0x2 && m.attempts == 1);
    head = q.frontIfStill(id);
    head->mask = 0;
    q.popFront();
    assert(q.size() == 2 && q.front(m, id) && m.text == 3);
    q.popFront(); q.popFront(); q.popFront();   // popping an empty queue is harmless
    assert(q.size() == 0 && !q.front(m, id));

    // a push always queues: full -> the oldest goes, the pushed one is the newest
    add(q, 10, false); add(q, 11, false); add(q, 12, false);
    add(q, 13, true);
    assert(q.size() == 3 && q.front(m, id) && m.text == 11);
    q.popFront(); q.popFront();
    assert(q.front(m, id) && m.text == 13 && q.size() == 1);
    return 0;
}
