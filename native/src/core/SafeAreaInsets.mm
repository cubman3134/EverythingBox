// iOS implementation of mmvSafeAreaInsets(): the key window's safeAreaInsets in points.
#include "SafeAreaInsets.h"
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

void mmvConfigureAudioSession()
{
    // Playback category: app audio (UI sounds, video/music playback) is media, not a ringtone — it must
    // play with the silent switch on, like every media app.
    AVAudioSession* session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback error:nil];
    [session setActive:YES error:nil];
}

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
