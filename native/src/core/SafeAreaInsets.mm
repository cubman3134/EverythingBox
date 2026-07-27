// iOS implementation of mmvSafeAreaInsets(): the key window's safeAreaInsets in points.
#include "SafeAreaInsets.h"
#import <UIKit/UIKit.h>

QMarginsF mmvSafeAreaInsets()
{
    UIWindow* win = nil;
    for (UIWindow* w in UIApplication.sharedApplication.windows)
        if (w.isKeyWindow) { win = w; break; }
    if (!win) win = UIApplication.sharedApplication.windows.firstObject;
    if (!win) return {};
    const UIEdgeInsets in = win.safeAreaInsets;
    return QMarginsF(in.left, in.top, in.right, in.bottom);
}
