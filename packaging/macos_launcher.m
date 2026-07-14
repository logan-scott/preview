/*
 * Launcher for the macOS Preview.app bundle.
 *
 * `preview` is a CLI that takes a file path as an argument, but when the
 * Finder opens a document it delivers the path through an Apple Event, not
 * argv. This tiny Cocoa app receives that event and re-execs the real
 * `preview` binary (bundled alongside it) with the file, so double-clicking
 * a document or using "Open With" works.
 */
#import <Cocoa/Cocoa.h>
#include <unistd.h>

@interface Launcher : NSObject <NSApplicationDelegate>
@end

@implementation Launcher

- (void)openFile:(NSString *)file {
    NSString *bin =
        [[NSBundle mainBundle] pathForAuxiliaryExecutable:@"preview"];
    if (!bin) {
        [NSApp terminate:nil];
        return;
    }
    const char *b = [bin fileSystemRepresentation];
    const char *f = [file fileSystemRepresentation];
    /* Replace this launcher process with preview; its own window loop
     * takes over. */
    execl(b, b, f, (char *)NULL);
    _exit(127); /* exec failed */
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename {
    (void)app;
    [self openFile:filename];
    return YES;
}

- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    (void)app;
    for (NSURL *u in urls) {
        if (u.isFileURL) {
            [self openFile:u.path];
            return;
        }
    }
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    /* Opening a document delivers openFile/openURLs first, and that call
     * execs away. If we reach here still running a short moment later, the
     * app was launched with no document — nothing to show, so quit. */
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
          [NSApp terminate:nil];
        });
}

@end

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        Launcher *launcher = [[Launcher alloc] init];
        [NSApp setDelegate:launcher];
        [NSApp run];
    }
    return 0;
}
