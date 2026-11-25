#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreWLAN/CoreWLAN.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void printJSON(NSDictionary *entry)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
    fwrite(data.bytes, 1, data.length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void recordEventAt(NSString *event, NSError *error, uint64_t wall, uint64_t uptime)
{
    NSMutableDictionary *entry = [@{
        @"event": event,
        @"wall_ns": @(wall),
        @"uptime_ns": @(uptime),
    } mutableCopy];
    if (error) {
        entry[@"domain"] = error.domain;
        entry[@"code"] = @(error.code);
        entry[@"message"] = error.localizedDescription;
    }
    printJSON(entry);
}

static void recordEvent(NSString *event, NSError *error)
{
    recordEventAt(event, error, clock_gettime_nsec_np(CLOCK_REALTIME),
                  clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

static id axAttribute(AXUIElementRef element, CFStringRef name)
{
    CFTypeRef value = NULL;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess) return nil;
    return CFBridgingRelease(value);
}

static BOOL exactLabel(id candidate, NSString *recipient)
{
    return [candidate isKindOfClass:NSString.class] &&
        [[candidate precomposedStringWithCanonicalMapping] isEqualToString:recipient];
}

static BOOL isAirDropDialog(AXUIElementRef window)
{
    return exactLabel(axAttribute(window, kAXTitleAttribute), @"AirDrop") &&
        exactLabel(axAttribute(window, kAXSubroleAttribute), @"AXSystemDialog");
}

static void findRecipients(AXUIElementRef element, NSString *recipient,
                           BOOL inDeviceList, NSUInteger depth, NSMutableArray *matches)
{
    if (depth > 12) return;
    NSString *role = axAttribute(element, kAXRoleAttribute);
    if ([role isEqualToString:(__bridge NSString *)kAXScrollAreaRole]) inDeviceList = YES;
    if (inDeviceList && [role isEqualToString:(__bridge NSString *)kAXButtonRole] &&
        [axAttribute(element, kAXEnabledAttribute) boolValue] &&
        (exactLabel(axAttribute(element, kAXTitleAttribute), recipient) ||
         exactLabel(axAttribute(element, kAXDescriptionAttribute), recipient) ||
         exactLabel(axAttribute(element, kAXValueAttribute), recipient))) {
        [matches addObject:(__bridge id)element];
    }
    NSArray *children = axAttribute(element, kAXChildrenAttribute);
    for (id child in children) findRecipients((__bridge AXUIElementRef)child, recipient,
                                             inDeviceList, depth + 1, matches);
}

static int selectRecipient(pid_t pid, NSString *name)
{
    if (!AXIsProcessTrusted()) {
        recordEvent(@"accessibility_unavailable", nil);
        return 1;
    }
    NSString *recipient = name.precomposedStringWithCanonicalMapping;
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    AXUIElementSetMessagingTimeout(app, 0.4);
    for (;;) {
        @autoreleasepool {
            NSMutableArray *matches = [NSMutableArray array];
            for (id object in axAttribute(app, kAXWindowsAttribute)) {
                AXUIElementRef window = (__bridge AXUIElementRef)object;
                if (!isAirDropDialog(window)) continue;
                findRecipients(window, recipient, NO, 0, matches);
            }
            if (matches.count > 1) {
                recordEvent(@"recipient_ambiguous", nil);
                CFRelease(app);
                return 1;
            }
            if (matches.count == 1) {
                uint64_t wall = clock_gettime_nsec_np(CLOCK_REALTIME);
                uint64_t uptime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                AXError result = AXUIElementPerformAction((__bridge AXUIElementRef)matches[0], kAXPressAction);
                recordEventAt(result == kAXErrorSuccess ? @"recipient_selected" : @"recipient_press_failed",
                              nil, wall, uptime);
                CFRelease(app);
                return result == kAXErrorSuccess ? 0 : 1;
            }
        }
        [NSThread sleepForTimeInterval:0.2];
    }
}

static void findCancelButtons(AXUIElementRef element, NSUInteger depth, NSMutableArray *matches)
{
    if (depth > 12) return;
    NSString *role = axAttribute(element, kAXRoleAttribute);
    // Devices are buttons in the scroll area. Never press one while cancelling.
    if ([role isEqualToString:(__bridge NSString *)kAXScrollAreaRole]) return;
    if ([role isEqualToString:(__bridge NSString *)kAXButtonRole] &&
        [axAttribute(element, kAXEnabledAttribute) boolValue]) {
        id title = axAttribute(element, kAXTitleAttribute);
        id description = axAttribute(element, kAXDescriptionAttribute);
        for (NSString *label in @[@"Cancel", @"Hủy", @"Huỷ"]) {
            NSString *normalized = label.precomposedStringWithCanonicalMapping;
            if (exactLabel(title, normalized) || exactLabel(description, normalized)) {
                [matches addObject:(__bridge id)element];
                return;
            }
        }
    }
    for (id child in axAttribute(element, kAXChildrenAttribute)) {
        findCancelButtons((__bridge AXUIElementRef)child, depth + 1, matches);
    }
}

static BOOL tileHasProgress(AXUIElementRef element, unsigned depth)
{
    if (depth > 6) return NO;
    if (exactLabel(axAttribute(element, kAXRoleAttribute), @"AXProgressIndicator")) {
        id value = axAttribute(element, kAXValueAttribute);
        id maximum = axAttribute(element, kAXMaxValueAttribute);
        return ![value isKindOfClass:NSNumber.class] || ![maximum isKindOfClass:NSNumber.class] ||
            [value doubleValue] < [maximum doubleValue];
    }
    for (id child in axAttribute(element, kAXChildrenAttribute)) {
        if (tileHasProgress((__bridge AXUIElementRef)child, depth + 1)) return YES;
    }
    return NO;
}

static BOOL isSendingStatus(NSString *text)
{
    NSString *status = [text.lowercaseString stringByTrimmingCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@" .…\t"]];
    return [@[@"sending", @"waiting", @"cancel transfer", @"cancel sending", @"đang gửi", @"đang chờ",
              @"hủy gửi", @"huỷ gửi", @"hủy truyền", @"huỷ truyền"] containsObject:status];
}

static void findActiveTiles(AXUIElementRef element, NSString *name, BOOL inDeviceList,
                            unsigned depth, NSMutableArray *matches)
{
    if (depth > 12) return;
    NSString *role = axAttribute(element, kAXRoleAttribute);
    if ([role isEqualToString:@"AXScrollArea"]) inDeviceList = YES;
    if (inDeviceList && [role isEqualToString:@"AXButton"] && [axAttribute(element, kAXEnabledAttribute) boolValue]) {
        BOOL named = !name.length;
        BOOL active = tileHasProgress(element, 0);
        for (id label in @[axAttribute(element, kAXTitleAttribute) ?: NSNull.null,
                           axAttribute(element, kAXDescriptionAttribute) ?: NSNull.null]) {
            if (![label isKindOfClass:NSString.class]) continue;
            NSArray<NSString *> *parts = [label componentsSeparatedByCharactersInSet:
                [NSCharacterSet characterSetWithCharactersInString:@",\n"]];
            if (exactLabel(label, name) || exactLabel(parts.firstObject, name)) named = YES;
            for (NSUInteger i = 1; i < parts.count; i++) active |= isSendingStatus(parts[i]);
        }
        for (id hint in @[axAttribute(element, kAXValueAttribute) ?: NSNull.null,
                          axAttribute(element, kAXHelpAttribute) ?: NSNull.null]) {
            if ([hint isKindOfClass:NSString.class]) active |= isSendingStatus(hint);
        }
        if (named && active) [matches addObject:(__bridge id)element];
        return;
    }
    for (id child in axAttribute(element, kAXChildrenAttribute)) {
        findActiveTiles((__bridge AXUIElementRef)child, name, inDeviceList, depth + 1, matches);
    }
}

static int cancelActiveShare(AXUIElementRef dialog, NSString *name)
{
    NSMutableArray *matches = [NSMutableArray array];
    findActiveTiles(dialog, name, NO, 0, matches);
    if (matches.count != 1) {
        recordEvent(matches.count ? @"cancel_tile_ambiguous" : @"cancel_tile_missing", nil);
        return 1;
    }
    AXError result = AXUIElementPerformAction((__bridge AXUIElementRef)matches[0], kAXPressAction);
    if (result != kAXErrorSuccess) {
        recordEvent(@"cancel_action_failed", [NSError errorWithDomain:@"AXError" code:result userInfo:nil]);
        return 1;
    }
    recordEvent(@"cancel_tile_pressed", nil);
    unsigned idleSamples = 0;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        [NSThread sleepForTimeInterval:0.15];
        if (!isAirDropDialog(dialog)) break; // AppKit reports a dismissed dialog separately.
        [matches removeAllObjects];
        findActiveTiles(dialog, name, NO, 0, matches);
        idleSamples = matches.count ? 0 : idleSamples + 1;
        if (idleSamples >= 2) {
            recordEvent(@"cancel_tile_cleared", nil);
            break;
        }
    }
    return 0;
}

static int cancelShare(pid_t pid, NSString *activeRecipient)
{
    if (!AXIsProcessTrusted()) {
        recordEvent(@"accessibility_unavailable", nil);
        return 1;
    }
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    AXUIElementSetMessagingTimeout(app, 0.4);
    NSMutableArray *dialogs = [NSMutableArray array];
    for (NSUInteger attempt = 0; attempt < 4; attempt++) {
        for (id window in axAttribute(app, kAXWindowsAttribute)) {
            if (isAirDropDialog((__bridge AXUIElementRef)window)) [dialogs addObject:window];
        }
        if (dialogs.count || attempt == 3) break;
        [NSThread sleepForTimeInterval:0.2];
    }
    CFRelease(app);
    if (dialogs.count != 1) {
        recordEvent(dialogs.count ? @"cancel_dialog_ambiguous" : @"cancel_dialog_missing", nil);
        return 1;
    }
    AXUIElementRef dialog = (__bridge AXUIElementRef)dialogs[0];
    if (activeRecipient) return cancelActiveShare(dialog, activeRecipient.precomposedStringWithCanonicalMapping);
    AXError result = AXUIElementPerformAction(dialog, kAXCancelAction);
    if (result == kAXErrorActionUnsupported || result == kAXErrorNotImplemented) {
        id button = axAttribute(dialog, kAXCancelButtonAttribute);
        if (!button) {
            NSMutableArray *buttons = [NSMutableArray array];
            findCancelButtons(dialog, 0, buttons);
            if (!buttons.count) return cancelActiveShare(dialog, @"");
            if (buttons.count != 1) {
                recordEvent(@"cancel_control_ambiguous", nil);
                return 1;
            }
            button = buttons[0];
        }
        result = AXUIElementPerformAction((__bridge AXUIElementRef)button, kAXPressAction);
    }
    NSError *error = result == kAXErrorSuccess ? nil : [NSError errorWithDomain:@"AXError" code:result userInfo:nil];
    recordEvent(error ? @"cancel_action_failed" : @"cancel_pressed", error);
    return error ? 1 : 0;
}

static NSArray<NSString *> *activeEthernet(NSArray<NSString *> *wifiNames)
{
    NSMutableSet *up = [NSMutableSet set];
    struct ifaddrs *interfaces;
    if (getifaddrs(&interfaces) != 0) return nil;
    for (struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (entry->ifa_flags & IFF_UP) [up addObject:@(entry->ifa_name)];
    }
    freeifaddrs(interfaces);

    NSMutableSet *ethernet = [NSMutableSet set];
    NSArray *hardware = CFBridgingRelease(SCNetworkInterfaceCopyAll());
    if (!hardware) return nil;
    for (id object in hardware) {
        SCNetworkInterfaceRef interface = (__bridge SCNetworkInterfaceRef)object;
        NSString *name = (__bridge NSString *)SCNetworkInterfaceGetBSDName(interface);
        CFStringRef type = SCNetworkInterfaceGetInterfaceType(interface);
        if (name && type && CFEqual(type, kSCNetworkInterfaceTypeEthernet) &&
            ![wifiNames containsObject:name] && [up containsObject:name]) {
            [ethernet addObject:name];
        }
    }
    return [[ethernet allObjects] sortedArrayUsingSelector:@selector(compare:)];
}

static NSDictionary *networkState(CWInterface *wifi)
{
    NSArray *ethernet = activeEthernet(CWWiFiClient.sharedWiFiClient.interfaceNames);
    NSMutableDictionary *state = [@{@"ethernet_up": ethernet ?: NSNull.null} mutableCopy];
    NSString *name = wifi.interfaceName;
    if (!name) return state;
    state[@"wifi_interface"] = name;
    BOOL powered = wifi.powerOn;
    state[@"wifi_powered"] = @(powered);

    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("AirDropBenchmark"), NULL, NULL);
    NSString *key = [NSString stringWithFormat:@"State:/Network/Interface/%@/Link", name];
    NSDictionary *link = store ? CFBridgingRelease(SCDynamicStoreCopyValue(store, (__bridge CFStringRef)key)) : nil;
    if (store) CFRelease(store);
    NSNumber *linkActive = [link isKindOfClass:NSDictionary.class] ? link[(__bridge NSString *)kSCPropNetLinkActive] : nil;
    NSString *ssid = powered ? wifi.ssid : nil;
    BOOL associated = powered && (ssid != nil || (linkActive ? linkActive.boolValue : wifi.interfaceMode == kCWInterfaceModeStation));
    state[@"wifi_associated"] = @(associated);
    if (!associated) return state;
    if (ssid) state[@"ssid"] = ssid;
    CWChannel *channel = wifi.wlanChannel;
    if (channel.channelNumber > 0) state[@"channel"] = @(channel.channelNumber);
    switch (channel.channelBand) {
        case kCWChannelBand2GHz: state[@"band_ghz"] = @2.4; break;
        case kCWChannelBand5GHz: state[@"band_ghz"] = @5; break;
        case kCWChannelBand6GHz: state[@"band_ghz"] = @6; break;
        default: break;
    }
    switch (channel.channelWidth) {
        case kCWChannelWidth20MHz: state[@"width_mhz"] = @20; break;
        case kCWChannelWidth40MHz: state[@"width_mhz"] = @40; break;
        case kCWChannelWidth80MHz: state[@"width_mhz"] = @80; break;
        case kCWChannelWidth160MHz: state[@"width_mhz"] = @160; break;
        default: break;
    }
    NSInteger rssi = wifi.rssiValue;
    NSInteger noise = wifi.noiseMeasurement;
    double rate = wifi.transmitRate;
    if (rssi < 0) state[@"rssi_dbm"] = @(rssi);
    if (noise < 0) state[@"noise_dbm"] = @(noise);
    if (isfinite(rate) && rate > 0) state[@"phy_tx_mbps"] = @(rate);
    return state;
}

@interface BenchmarkShare : NSObject <NSApplicationDelegate, NSSharingServiceDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSURL *file;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSSharingService *service;
@property(nonatomic) BOOL finished;
@property(nonatomic) int status;
@end

@implementation BenchmarkShare
- (void)finish:(NSString *)event error:(NSError *)error status:(int)status
{
    if (self.finished) return;
    self.finished = YES;
    self.status = status;
    recordEvent(event, error);
    [NSApp stop:nil];
    // Wake the AppKit event loop even if the callback arrived over XPC.
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil subtype:0 data1:0 data2:0] atStart:NO];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    self.service = [NSSharingService sharingServiceNamed:NSSharingServiceNameSendViaAirDrop];
    if (![self.service canPerformWithItems:@[self.file]]) {
        [self finish:@"unavailable" error:nil status:1];
        return;
    }
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 390, 76)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"AirDrop Benchmark";
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    NSTextField *label = [NSTextField labelWithString:@"Choose the receiving device in the AirDrop window."];
    label.frame = NSMakeRect(20, 28, 350, 20);
    [self.window.contentView addSubview:label];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activate];
    self.service.delegate = self;
    recordEvent(@"picker_requested", nil);
    [self.service performWithItems:@[self.file]];
}

- (NSWindow *)sharingService:(NSSharingService *)service sourceWindowForShareItems:(NSArray *)items
         sharingContentScope:(NSSharingContentScope *)scope
{
    return self.window;
}

- (void)sharingService:(NSSharingService *)service willShareItems:(NSArray *)items
{
    recordEvent(@"will_share", nil);
}

- (void)sharingService:(NSSharingService *)service didShareItems:(NSArray *)items
{
    [self finish:@"shared" error:nil status:0];
}

- (void)sharingService:(NSSharingService *)service didFailToShareItems:(NSArray *)items error:(NSError *)error
{
    BOOL cancelled = [error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSUserCancelledError;
    [self finish:cancelled ? @"cancelled" : @"failed" error:error status:1];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [self finish:@"cancelled" error:nil status:1];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    [self finish:@"cancelled" error:nil status:1];
    return NSTerminateCancel;
}
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--accessibility-status")) return AXIsProcessTrusted() ? 0 : 1;
        if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--cancel-share")) {
            pid_t pid = (pid_t)strtol(argv[2], NULL, 10);
            return pid >= 2 ? cancelShare(pid, argc == 4 ? @(argv[3]) : nil) : 2;
        }
        if (argc == 4 && !strcmp(argv[1], "--select-recipient")) {
            pid_t pid = (pid_t)strtol(argv[2], NULL, 10);
            if (pid < 2 || !argv[3][0]) return 2;
            return selectRecipient(pid, @(argv[3]));
        }
        if (argc == 2) {
            CWInterface *wifi = CWWiFiClient.sharedWiFiClient.interface;
            if (!strcmp(argv[1], "--network-state")) {
                printJSON(networkState(wifi));
                return 0;
            }
            if (!wifi.interfaceName) return 1;
            if (!strcmp(argv[1], "--wifi-scan")) {
                NSError *error = nil;
                NSSet<CWNetwork *> *networks = [wifi scanForNetworksWithSSID:nil error:&error];
                if (networks) {
                    printJSON(@{@"network_count": @(networks.count)});
                    return 0;
                }
                fprintf(stderr, "%s\n", error.localizedDescription.UTF8String ?: "CoreWLAN scan failed");
                return 1;
            }
            if (!strcmp(argv[1], "--wifi-disassociate")) {
                [wifi disassociate];
                return 0;
            }
            BOOL powerOn = !strcmp(argv[1], "--wifi-on");
            if (powerOn || !strcmp(argv[1], "--wifi-off")) {
                NSError *error = nil;
                if ([wifi setPower:powerOn error:&error]) return 0;
                fprintf(stderr, "%s\n", error.localizedDescription.UTF8String ?: "Wi-Fi power control failed");
                return 1;
            }
            return 2;
        }
        if (argc != 3 || strcmp(argv[1], "--share")) return 2;
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        BenchmarkShare *delegate = [BenchmarkShare new];
        delegate.file = [NSURL fileURLWithFileSystemRepresentation:argv[2] isDirectory:NO relativeToURL:nil];
        app.delegate = delegate;
        [app run];
        return delegate.finished ? delegate.status : 1;
    }
}
