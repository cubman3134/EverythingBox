#include "KeepAwake.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace KeepAwake
{

// The ONE platform call in the feature (issue #147).
//
// Windows: SetThreadExecutionState with ES_CONTINUOUS, which makes the request STICKY for this thread until
// it is withdrawn — the alternative (a bare ES_DISPLAY_REQUIRED poke) only resets the idle timer once and
// would need a timer of its own to mean anything. ES_SYSTEM_REQUIRED comes along so a long page does not put
// the machine to sleep out from under a reader who has not touched it. Withdrawing is the same call with
// ES_CONTINUOUS alone, which is why release() cannot leave a lock behind.
//
// Everywhere else this is honestly a no-op, and says so rather than pretending: Android's FLAG_KEEP_SCREEN_ON
// needs the activity, and X11/Wayland/macOS each want a different inhibitor. The refcount above is still
// exact on those platforms, so wiring one in later is this function and nothing else.
static void platformApply(bool on)
{
#if defined(_WIN32)
    ::SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                : ES_CONTINUOUS);
#else
    (void)on;
#endif
}

Registry::Registry() : apply_(&platformApply) {}

Registry& Registry::instance()
{
    static Registry r;
    return r;
}

void Registry::setApplier(std::function<void(bool)> fn)
{
    apply_ = std::move(fn);
    if (apply_) apply_(active());   // the incoming applier inherits the state; it can never start out of step
}

void Registry::acquire()
{
    ++holders_;
    if (holders_ == 1 && apply_) apply_(true);
}

void Registry::release()
{
    if (holders_ <= 0) return;
    --holders_;
    if (holders_ == 0 && apply_) apply_(false);
}

Guard::Guard(bool enabled) : held_(enabled)
{
    if (held_) Registry::instance().acquire();
}

Guard::~Guard() { release(); }

void Guard::release()
{
    if (!held_) return;
    held_ = false;
    Registry::instance().release();
}

} // namespace KeepAwake
