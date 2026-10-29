#include "Events/EventBus.hpp"
#include "Events/EngineEvents.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
struct FirstEvent { int value = 0; };
struct SecondEvent { int value = 0; };
struct ModuleEvent { int value = 0; };

bool report(const char* name, bool ok)
{
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    return ok;
}

template<class Listener>
struct ListenerGuard {
    EventBus& bus;
    Listener& listener;
    ~ListenerGuard() { bus.disconnect(&listener); bus.clear(); }
};

struct Recorder {
    std::vector<int> first;
    std::vector<int> second;
    void onFirst(const FirstEvent& event) { first.push_back(event.value); }
    void onSecond(const SecondEvent& event) { second.push_back(event.value); }
};

bool immediateDeferredAndTypedFlush()
{
    auto& bus = EventBus::get();
    bus.clear();
    Recorder receiver;
    ListenerGuard guard{bus, receiver};
    bus.sink<FirstEvent>().connect<&Recorder::onFirst>(receiver);
    bus.sink<SecondEvent>().connect<&Recorder::onSecond>(receiver);

    bus.publish(FirstEvent{1});
    bus.queue(FirstEvent{2});
    bus.emplace<FirstEvent>(3);
    bus.queue(SecondEvent{9});
    bool ok = receiver.first == std::vector<int>{1} && receiver.second.empty();
    bus.flush<FirstEvent>();
    ok &= receiver.first == std::vector<int>({1, 2, 3}) && receiver.second.empty();
    bus.flush<SecondEvent>();
    ok &= receiver.second == std::vector<int>{9};
    bus.queue(FirstEvent{4});
    bus.queue(SecondEvent{10});
    bus.flush();
    ok &= receiver.first == std::vector<int>({1, 2, 3, 4}) &&
          receiver.second == std::vector<int>({9, 10});
    // Cross-type listener/queue order is deliberately not asserted.
    return report("EventBus immediate, FIFO and typed flush", ok);
}

struct NestedReceiver {
    EventBus& bus;
    std::vector<int> calls;
    void onFirst(const FirstEvent& event) {
        if (event.value == 1) {
            calls.push_back(10);
            bus.publish(FirstEvent{2});
            calls.push_back(30);
        } else {
            calls.push_back(20);
        }
    }
};

bool nestedImmediatePublication()
{
    auto& bus = EventBus::get();
    bus.clear();
    NestedReceiver receiver{bus};
    ListenerGuard guard{bus, receiver};
    bus.sink<FirstEvent>().connect<&NestedReceiver::onFirst>(receiver);
    bus.publish(FirstEvent{1});
    return report("EventBus nested immediate publication", receiver.calls == std::vector<int>({10, 20, 30}));
}

struct CreatedReceiver {
    EventBus& bus;
    std::vector<std::string> tags;
    bool rejected_nested_flush = false;
    void onCreated(const EntityCreatedEvent& event) {
        tags.push_back(event.tag);
        if (event.tag == "first") {
            bus.queue(EntityCreatedEvent{EntityRef{}, "later", {}});
            try { bus.flush<EntityCreatedEvent>(); }
            catch (const std::logic_error&) { rejected_nested_flush = true; }
        } else if (event.tag == "clear") {
            bus.clear();
            bus.queue(EntityCreatedEvent{EntityRef{}, "after-clear", {}});
        }
    }
};

bool createdQueueMutationAndFlushGuard()
{
    auto& bus = EventBus::get();
    bus.clear();
    CreatedReceiver receiver{bus};
    ListenerGuard guard{bus, receiver};
    bus.sink<EntityCreatedEvent>().connect<&CreatedReceiver::onCreated>(receiver);
    bus.queue(EntityCreatedEvent{EntityRef{}, "first", {}});
    bus.queue(EntityCreatedEvent{EntityRef{}, "second", {}});
    bus.flush<EntityCreatedEvent>();
    bool ok = receiver.rejected_nested_flush && receiver.tags == std::vector<std::string>({"first", "second"});
    bus.flush<EntityCreatedEvent>();
    ok &= receiver.tags == std::vector<std::string>({"first", "second", "later"});

    bus.queue(EntityCreatedEvent{EntityRef{}, "clear", {}});
    bus.queue(EntityCreatedEvent{EntityRef{}, "cancelled", {}});
    bus.flush<EntityCreatedEvent>();
    ok &= receiver.tags == std::vector<std::string>({"first", "second", "later", "clear"});
    bus.flush<EntityCreatedEvent>();
    ok &= receiver.tags == std::vector<std::string>({"first", "second", "later", "clear", "after-clear"});
    return report("EventBus created queue snapshot, clear and nested flush guard", ok);
}

struct OtherQueueReceiver {
    EventBus& bus;
    std::vector<int> first;
    std::vector<int> second;
    void onFirst(const FirstEvent& event) {
        first.push_back(event.value);
        bus.queue(SecondEvent{event.value + 10});
    }
    void onSecond(const SecondEvent& event) { second.push_back(event.value); }
};

bool queueOtherTypeDuringTypedFlush()
{
    auto& bus = EventBus::get();
    bus.clear();
    OtherQueueReceiver receiver{bus};
    ListenerGuard guard{bus, receiver};
    bus.sink<FirstEvent>().connect<&OtherQueueReceiver::onFirst>(receiver);
    // The second pool exists before dispatch; this does not test mutation of
    // the active ordinary-event vector or all-type flush ordering.
    bus.sink<SecondEvent>().connect<&OtherQueueReceiver::onSecond>(receiver);
    bus.queue(FirstEvent{2});
    bus.flush<FirstEvent>();
    bool ok = receiver.first == std::vector<int>{2} && receiver.second.empty();
    bus.flush<SecondEvent>();
    ok &= receiver.second == std::vector<int>{12};
    return report("EventBus queue other type during typed flush", ok);
}

struct SelfDisconnectingReceiver {
    EventBus& bus;
    int first = 0;
    int second = 0;
    void onFirst(const FirstEvent&) { ++first; bus.disconnect(this); }
    void onSecond(const SecondEvent&) { ++second; }
};

bool singleListenerDisconnect()
{
    auto& bus = EventBus::get();
    bus.clear();
    SelfDisconnectingReceiver receiver{bus};
    ListenerGuard guard{bus, receiver};
    bus.sink<FirstEvent>().connect<&SelfDisconnectingReceiver::onFirst>(receiver);
    bus.sink<SecondEvent>().connect<&SelfDisconnectingReceiver::onSecond>(receiver);
    bus.publish(FirstEvent{});
    bus.publish(FirstEvent{});
    bus.publish(SecondEvent{});
    // This is the single-listener observation, not a multi-listener ordering guarantee.
    return report("EventBus single listener self-disconnect", receiver.first == 1 && receiver.second == 0);
}

bool clearQueuedEvents()
{
    auto& bus = EventBus::get();
    bus.clear();
    Recorder receiver;
    ListenerGuard guard{bus, receiver};
    bus.sink<FirstEvent>().connect<&Recorder::onFirst>(receiver);
    bus.sink<SecondEvent>().connect<&Recorder::onSecond>(receiver);
    bus.queue(FirstEvent{1});
    bus.queue(SecondEvent{2});
    bus.clear();
    bus.flush();
    bool ok = receiver.first.empty() && receiver.second.empty();
    bus.queue(FirstEvent{3});
    bus.flush<FirstEvent>();
    ok &= receiver.first == std::vector<int>{3};
    return report("EventBus clear discards pending values and keeps listeners", ok);
}

struct ThrowingReceiver {
    std::vector<int> calls;
    bool should_throw = true;
    void onFirst(const FirstEvent& event) {
        calls.push_back(event.value);
        if (should_throw && event.value == 2) { should_throw = false; throw std::runtime_error("observer"); }
    }
};

bool ordinaryQueueExceptionObservation()
{
    auto& bus = EventBus::get();
    bus.clear();
    ThrowingReceiver receiver;
    ListenerGuard guard{bus, receiver};
    bus.sink<FirstEvent>().connect<&ThrowingReceiver::onFirst>(receiver);
    bus.queue(FirstEvent{1});
    bus.queue(FirstEvent{2});
    bool threw = false;
    try { bus.flush<FirstEvent>(); }
    catch (const std::runtime_error&) { threw = true; }
    bool ok = threw && receiver.calls == std::vector<int>({1, 2});
    bus.flush<FirstEvent>();
    ok &= receiver.calls == std::vector<int>({1, 2, 1, 2});
    bus.flush<FirstEvent>();
    ok &= receiver.calls == std::vector<int>({1, 2, 1, 2});
    // This is the vendored EnTT observation. It is not the created-event
    // at-most-once contract or a general supported observer-failure policy.
    return report("EventBus ordinary queue exception retains attempted values", ok);
}

garden::ecs::ComponentOwnerId owner(uint8_t id)
{
    std::array<uint8_t, 32> digest{};
    digest[0] = id;
    return garden::ecs::ComponentOwnerId::module(digest, 7);
}

template<class Operation>
bool recordsModuleUse(EventBus& bus, uint8_t id, Operation operation)
{
    const auto source = owner(id);
    if (!EventBus::moduleSourceCanRetire(source)) return false;
    {
        EventBus::ModuleSourceScope scope(source);
        operation();
    }
    const bool refused = !EventBus::moduleSourceCanRetire(source);
    bus.clear();
    return refused && !EventBus::moduleSourceCanRetire(source);
}

bool moduleScopeTracking()
{
    auto& bus = EventBus::get();
    bus.clear();
    const auto unused = owner(1);
    bool ok = EventBus::moduleSourceCanRetire(unused);
    {
        EventBus::ModuleSourceScope scope(unused);
        bus.flush(); // The non-templated flush does not instantiate a pool.
    }
    ok &= EventBus::moduleSourceCanRetire(unused);
    ok &= recordsModuleUse(bus, 2, [&] { (void)bus.sink<ModuleEvent>(); });
    ok &= recordsModuleUse(bus, 3, [&] { bus.publish(ModuleEvent{1}); });
    ok &= recordsModuleUse(bus, 4, [&] { bus.queue(ModuleEvent{1}); });
    ok &= recordsModuleUse(bus, 5, [&] { bus.emplace<ModuleEvent>(1); });
    ok &= recordsModuleUse(bus, 6, [&] { bus.flush<ModuleEvent>(); });
    bool batch_accepted = false;
    ok &= recordsModuleUse(bus, 7, [&] {
        std::vector<EntityCreatedEvent> batch{{EntityRef{}, "module", {}}};
        batch_accepted = bus.queueEntityCreatedBatch(std::move(batch));
    });
    ok &= batch_accepted;

    const auto outer = owner(8);
    const auto inner = owner(9);
    {
        EventBus::ModuleSourceScope outer_scope(outer);
        {
            EventBus::ModuleSourceScope inner_scope(inner);
            bus.queue(ModuleEvent{2});
        }
        ok &= !EventBus::moduleSourceCanRetire(inner) && EventBus::moduleSourceCanRetire(outer);
        bus.queue(ModuleEvent{3});
    }
    ok &= !EventBus::moduleSourceCanRetire(outer) && !EventBus::moduleSourceCanRetire(inner);
    bus.clear();
    ok &= EventBus::moduleSourceCanRetire(unused) &&
          !EventBus::moduleSourceCanRetire(garden::ecs::ComponentOwnerId::engineCore());
    // Fake owner identities exercise source accounting only. Actual DLL/image
    // retention and callback lifetime require the module lifecycle fixtures.
    return report("EventBus module source tracking is fail-closed", ok);
}
}

int main()
{
    bool ok = true;
    ok = immediateDeferredAndTypedFlush() && ok;
    ok = nestedImmediatePublication() && ok;
    ok = createdQueueMutationAndFlushGuard() && ok;
    ok = queueOtherTypeDuringTypedFlush() && ok;
    ok = singleListenerDisconnect() && ok;
    ok = clearQueuedEvents() && ok;
    ok = ordinaryQueueExceptionObservation() && ok;
    ok = moduleScopeTracking() && ok;
    return ok ? 0 : 1;
}
