#include "SafeAreaInsets.h"

// Non-Apple-mobile platforms have no system cutouts: zero insets. iOS provides the real values from
// UIKit in SafeAreaInsets.mm (compiled instead of this definition — see the if(IOS) CMake block).
#ifndef Q_OS_IOS
QMarginsF mmvSafeAreaInsets() { return {}; }
void mmvConfigureAudioSession() {}
#endif
