#pragma once
#include <functional>
#include <utility>

// "Keep the screen awake while reading" (issue #147): a REFCOUNT with a platform applier behind it.
//
// A refcount rather than a bare on/off because a wake lock is a shared resource and the reader is not the
// only thing that will ever want one — two holders must not be able to switch each other off, and the
// platform call must happen exactly on the 0->1 and 1->0 transitions rather than once per open.
//
// The point of the split is testability and portability at once: Registry and Guard are plain C++ with no
// Qt and no platform headers, so probe_readergestures drives every transition with an observer applier, and
// the ONE #if in the whole feature lives in KeepAwake.cpp.
//
// Deliberately not Qt-flavoured: no QObject, no signals. A wake lock has no lifetime story that a parent
// widget could tell better than RAII already does — see Guard.
namespace KeepAwake
{

class Registry
{
public:
    static Registry& instance();

    // Replace the applier. The new one is handed the CURRENT state immediately, so it can never start out of
    // step with the count. The default applier is the platform one (installed in the constructor), which is
    // what makes "nobody has to remember to install it at startup" true.
    void setApplier(std::function<void(bool)> fn);

    void acquire();
    void release();   // a release with no matching acquire is ignored; the count can never go negative

    int  holders() const { return holders_; }
    bool active()  const { return holders_ > 0; }

private:
    Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    std::function<void(bool)> apply_;
    int holders_ = 0;
};

// The RAII token. Construct one while reading, destroy it to let the screen sleep again.
//
// It is RAII rather than an explicit acquire/release pair for one specific reason issue #147 asks about: the
// lock has to be released when the reader goes away by a path nobody wrote — a teardown, a stack unwind, the
// window being destroyed under it. A destructor covers all of those; a close() handler covers only the ones
// somebody thought of. Hold it in a std::unique_ptr member of the reader and the guarantee is structural.
//
// Constructing with enabled=false (the setting is off) holds NOTHING — no acquire, no release, no transition.
// So the toggle is expressed by whether a Guard holds anything, not by a branch at every site that makes one.
class Guard
{
public:
    explicit Guard(bool enabled);
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    bool held() const { return held_; }
    void release();   // early release; the destructor then does nothing. Idempotent.

private:
    bool held_ = false;
};

} // namespace KeepAwake
