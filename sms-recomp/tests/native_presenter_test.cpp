#include "native_presenter.h"

#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

} // namespace

int main() {
    const auto exact = sbr_native_present_viewport(1280, 960, 4, 3);
    require(exact.x == 0 && exact.y == 0 && exact.width == 1280 && exact.height == 960);

    const auto pillarbox = sbr_native_present_viewport(1920, 1080, 4, 3);
    require(pillarbox.x == 240 && pillarbox.y == 0 && pillarbox.width == 1440 &&
            pillarbox.height == 1080);

    const auto letterbox = sbr_native_present_viewport(1024, 1024, 16, 9);
    require(letterbox.x == 0 && letterbox.y == 224 && letterbox.width == 1024 &&
            letterbox.height == 576);

    const auto invalid = sbr_native_present_viewport(0, 1080, 4, 3);
    require(invalid.width == 0 && invalid.height == 0);

    require(sbr_native_presenter_initialize_action(NativePresenterLifecycle::Uninitialized, true,
                                                   false) ==
            NativePresenterInitializeAction::Claim);
    require(sbr_native_presenter_initialize_action(NativePresenterLifecycle::Ready, true, true) ==
            NativePresenterInitializeAction::Reuse);
    require(sbr_native_presenter_initialize_action(NativePresenterLifecycle::Ready, true, false) ==
            NativePresenterInitializeAction::Reject);
    require(sbr_native_presenter_initialize_action(NativePresenterLifecycle::Claimed, true, true) ==
            NativePresenterInitializeAction::Reject);
    require(sbr_native_presenter_initialize_action(NativePresenterLifecycle::Uninitialized, false,
                                                   false) ==
            NativePresenterInitializeAction::Reject);
    require(!sbr_native_presenter_shutdown_releases(NativePresenterLifecycle::Uninitialized));
    require(sbr_native_presenter_shutdown_releases(NativePresenterLifecycle::Claimed));
    require(sbr_native_presenter_shutdown_releases(NativePresenterLifecycle::Ready));

    require(sbr_native_presenter_window_availability(true, false, 1280, 720) ==
            NativePresenterAvailability::Ready);
    require(sbr_native_presenter_window_availability(true, true, 1280, 720) ==
            NativePresenterAvailability::Unavailable);
    require(sbr_native_presenter_window_availability(true, false, 0, 720) ==
            NativePresenterAvailability::Unavailable);
    require(sbr_native_presenter_window_availability(false, false, 1280, 720) ==
            NativePresenterAvailability::Failed);

    require(sbr_native_presenter_acquire_availability(true, true, 1280, 720) ==
            NativePresenterAvailability::Ready);
    require(sbr_native_presenter_acquire_availability(true, false, 0, 0) ==
            NativePresenterAvailability::Unavailable);
    require(sbr_native_presenter_acquire_availability(true, true, 0, 720) ==
            NativePresenterAvailability::Unavailable);
    require(sbr_native_presenter_acquire_availability(false, false, 0, 0) ==
            NativePresenterAvailability::Failed);
    return 0;
}
