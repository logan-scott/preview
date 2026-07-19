#import <Cocoa/Cocoa.h>

#include "macos_menu.h"

/* Build the menu bar the web view needs for standard editing shortcuts.
 * Cut/Copy/Paste/Select All are wired to the first responder (the
 * WKWebView), which implements them; macOS matches the key equivalents
 * against this menu, so without it Cmd+C never reaches the page. */
int preview_install_menu(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        if (!app)
            return 0;
        if ([app mainMenu])
            return 1; /* something already installed one */

        NSString *name = [[NSProcessInfo processInfo] processName];
        NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
        [app setMainMenu:bar];

        /* Application menu (the first menu is always the app menu). */
        NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@""
                                                         action:NULL
                                                  keyEquivalent:@""];
        [bar addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:name];
        [appItem setSubmenu:appMenu];
        [appMenu addItemWithTitle:[@"About " stringByAppendingString:name]
                           action:@selector(orderFrontStandardAboutPanel:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name]
                           action:@selector(hide:)
                    keyEquivalent:@"h"];
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];

        /* Edit menu — the reason this file exists. */
        NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:@"Edit"
                                                          action:NULL
                                                   keyEquivalent:@""];
        [bar addItem:editItem];
        NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editItem setSubmenu:edit];
        [edit addItemWithTitle:@"Cut"
                        action:@selector(cut:)
                 keyEquivalent:@"x"];
        [edit addItemWithTitle:@"Copy"
                        action:@selector(copy:)
                 keyEquivalent:@"c"];
        [edit addItemWithTitle:@"Paste"
                        action:@selector(paste:)
                 keyEquivalent:@"v"];
        [edit addItem:[NSMenuItem separatorItem]];
        [edit addItemWithTitle:@"Select All"
                        action:@selector(selectAll:)
                 keyEquivalent:@"a"];

        /* Window menu, so the window can be minimized/closed as usual. */
        NSMenuItem *winItem = [[NSMenuItem alloc] initWithTitle:@"Window"
                                                         action:NULL
                                                  keyEquivalent:@""];
        [bar addItem:winItem];
        NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
        [winItem setSubmenu:win];
        [win addItemWithTitle:@"Minimize"
                       action:@selector(performMiniaturize:)
                keyEquivalent:@"m"];
        [win addItemWithTitle:@"Close"
                       action:@selector(performClose:)
                keyEquivalent:@"w"];
        [app setWindowsMenu:win];

        return [app mainMenu] != nil;
    }
}
