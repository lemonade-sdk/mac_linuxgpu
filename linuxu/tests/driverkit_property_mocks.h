/* DriverKit container substitutes (OSObject, OSString, OSNumber, OSBoolean,
 * OSArray, OSDictionary) with the API subset the dext's property builders
 * use, reference counting that matches DriverKit's (a with*() result holds
 * one reference, a collection retains what it holds), a live-object count
 * for leak checks and an allocation-failure injector. No device access. */
#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

static long mock_live_objects;
static long mock_allocations;
static long mock_fail_at;	/* fail the Nth allocation from now (1-based), 0 never */

static bool mock_allocation_fails()
{
    ++mock_allocations;
    return mock_fail_at && mock_allocations == mock_fail_at;
}

class OSMetaClassBase {
public:
    virtual ~OSMetaClassBase() {}
};

class OSObject : public OSMetaClassBase {
    int refs = 1;
    bool immortal = false;
protected:
    OSObject() { ++mock_live_objects; }
    explicit OSObject(bool forever) : immortal(forever) {}
public:
    ~OSObject() override { if (!immortal) --mock_live_objects; }
    void retain() const { if (!immortal) ++const_cast<OSObject *>(this)->refs; }
    void release() const
    {
        if (immortal) return;
        OSObject *self = const_cast<OSObject *>(this);
        assert(self->refs > 0);
        if (--self->refs == 0) delete self;
    }
    int retainCount() const { return refs; }
};

class OSString : public OSObject {
public:
    std::string value;
    static OSString *withCString(const char *s)
    {
        if (mock_allocation_fails()) return nullptr;
        OSString *o = new OSString;
        o->value = s;
        return o;
    }
    const char *getCStringNoCopy() const { return value.c_str(); }
};

class OSNumber : public OSObject {
public:
    uint64_t value = 0;
    size_t bits = 0;
    static OSNumber *withNumber(uint64_t v, size_t numberOfBits)
    {
        assert(numberOfBits == 8 || numberOfBits == 16 || numberOfBits == 32 || numberOfBits == 64);
        if (mock_allocation_fails()) return nullptr;
        OSNumber *o = new OSNumber;
        o->bits = numberOfBits;
        o->value = numberOfBits == 64 ? v : (v & ((1ull << numberOfBits) - 1));
        return o;
    }
};

class OSBoolean : public OSObject {
public:
    bool value;
    explicit OSBoolean(bool v) : OSObject(true), value(v) {}
};
static OSBoolean mock_true(true), mock_false(false);
static OSBoolean *const kOSBooleanTrue = &mock_true;
static OSBoolean *const kOSBooleanFalse = &mock_false;

class OSArray : public OSObject {
public:
    std::vector<const OSObject *> items;
    static OSArray *withCapacity(uint32_t)
    {
        if (mock_allocation_fails()) return nullptr;
        return new OSArray;
    }
    ~OSArray() override { for (auto *o : items) o->release(); }
    bool setObject(const OSMetaClassBase *object)
    {
        auto *o = dynamic_cast<const OSObject *>(object);
        if (!o) return false;
        o->retain();
        items.push_back(o);
        return true;
    }
    uint32_t getCount() const { return (uint32_t)items.size(); }
    const OSObject *getObject(uint32_t i) const { return i < items.size() ? items[i] : nullptr; }
};

class OSDictionary : public OSObject {
public:
    std::map<std::string, const OSObject *> items;
    static OSDictionary *withCapacity(uint32_t)
    {
        if (mock_allocation_fails()) return nullptr;
        return new OSDictionary;
    }
    ~OSDictionary() override { for (auto &kv : items) kv.second->release(); }
    bool setObject(const char *key, const OSMetaClassBase *object)
    {
        auto *o = dynamic_cast<const OSObject *>(object);
        if (!key || !o) return false;
        o->retain();
        auto it = items.find(key);
        if (it != items.end()) {
            it->second->release();
            it->second = o;
        } else {
            items.emplace(key, o);
        }
        return true;
    }
    const OSObject *getObject(const char *key) const
    {
        auto it = items.find(key);
        return it == items.end() ? nullptr : it->second;
    }
    uint32_t getCount() const { return (uint32_t)items.size(); }
};
