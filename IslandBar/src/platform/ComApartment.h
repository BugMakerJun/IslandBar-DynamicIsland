// platform/ComApartment.h -- paired COM apartment initialization.
//
// CoInitializeEx can return RPC_E_CHANGED_MODE when the thread already belongs
// to another apartment. COM is still usable in that case, but this call did not
// acquire an initialization reference, so it must not be paired with
// CoUninitialize. Keeping the two decisions together prevents callers from
// treating every usable result as an initialization that needs balancing.
#pragma once

#include <windows.h>
#include <objbase.h>

namespace island {

class ComApartment {
public:
    explicit ComApartment(DWORD mode) noexcept : result_(CoInitializeEx(nullptr, mode)) {}

    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    // True when COM can be used on this thread, including an apartment that
    // another component initialized before this call.
    bool Usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_;
};

}  // namespace island
