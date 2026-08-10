#pragma once

#include "pch.h"

namespace mactab {

// Minimal intrusive COM pointer.
//
// Hand-rolled rather than using WRL's ComPtr or winrt::com_ptr: WRL headers are
// not reliably present outside MSVC, and pulling in winrt/base.h would make
// every file that touches COM un-checkable by the off-Windows syntax pass.
// Forty lines is a cheap price for keeping the whole shell layer verifiable.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr& other) : m_ptr(other.m_ptr) {
        if (m_ptr) m_ptr->AddRef();
    }
    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            Reset();
            m_ptr = other.m_ptr;
            if (m_ptr) m_ptr->AddRef();
        }
        return *this;
    }

    ComPtr(ComPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    void Reset() {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

    // For the ...(IID, void**) creation pattern. Releases any current value
    // first so a reused ComPtr cannot leak.
    T** Put() {
        Reset();
        return &m_ptr;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }

    T*  Get() const { return m_ptr; }
    T*  operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T* m_ptr = nullptr;
};

// Puts the calling thread in an apartment for its lifetime.
//
// Tolerates the thread already being initialised in the same mode, which is the
// documented S_FALSE case, and records the RPC_E_CHANGED_MODE case rather than
// silently proceeding with the wrong apartment.
class ComApartment {
public:
    explicit ComApartment(DWORD model = COINIT_APARTMENTTHREADED);
    ~ComApartment();

    ComApartment(const ComApartment&)            = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    bool Ok() const { return m_initialized; }

private:
    bool m_initialized = false;
};

} // namespace mactab
