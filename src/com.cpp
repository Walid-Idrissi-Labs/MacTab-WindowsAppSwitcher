#include "pch.h"
#include "com.h"
#include "diag.h"

namespace mactab {

ComApartment::ComApartment(DWORD model) {
    const HRESULT hr = ::CoInitializeEx(nullptr, model);

    // S_FALSE means the thread was already initialised in this same mode. We
    // still owe a matching CoUninitialize, so it counts as success.
    if (SUCCEEDED(hr)) {
        m_initialized = true;
        return;
    }

    if (hr == RPC_E_CHANGED_MODE) {
        // Already in a different apartment. Do NOT uninitialise on destruction;
        // whoever set it owns it.
        MACTAB_WARN("com: thread already in a different apartment (RPC_E_CHANGED_MODE)");
    } else {
        MACTAB_FAIL("com: CoInitializeEx failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
    }
}

ComApartment::~ComApartment() {
    if (m_initialized)
        ::CoUninitialize();
}

} // namespace mactab
