// macOS host: menu bar item, IOHIDManager input, IOHIDUserDevice virtual HOTAS, CoreText font.
// Virtual HID devices need the com.apple.developer.hid.virtual.device entitlement or root.
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "app.h"
#include "core/keys.h"
#include "ui/icon.h"

static App g_app;

static double now() { return CFAbsoluteTimeGetCurrent(); }

// Virtual HOTAS ------------------------------------------------------------------------------

// 8 x int16 axes, 32 buttons, 1 hat nibble plus padding: a 21-byte report without report ID.
static const uint8_t kReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x33, 0x09, 0x34,
    0x09, 0x35, 0x09, 0x36, 0x09, 0x37, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x08,
    0x81, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x20,
    0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65,
    0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xC0};

// IOHIDUserDevice (IOKitUser hidsystem/IOHIDUserDevice.h, macOS 10.15+) is exported by IOKit but its header
// is not in the public SDK, so the functions are resolved at runtime. Creating a device needs the
// Apple-granted com.apple.developer.hid.virtual.device entitlement; without it creation returns NULL.
typedef struct __IOHIDUserDevice* IOHIDUserDeviceRef;

struct UserDeviceApi {
    IOHIDUserDeviceRef (*createWithProperties)(CFAllocatorRef, CFDictionaryRef, IOOptionBits) = nullptr;
    void (*setDispatchQueue)(IOHIDUserDeviceRef, dispatch_queue_t) = nullptr;
    void (*activate)(IOHIDUserDeviceRef) = nullptr;
    void (*cancel)(IOHIDUserDeviceRef) = nullptr;
    IOReturn (*handleReportWithTimeStamp)(IOHIDUserDeviceRef, uint64_t, const uint8_t*, CFIndex) = nullptr;

    bool load() {
        if (createWithProperties) return true;
#define RESOLVE(field, name) field = (decltype(field))dlsym(RTLD_DEFAULT, name)
        RESOLVE(createWithProperties, "IOHIDUserDeviceCreateWithProperties");
        RESOLVE(setDispatchQueue, "IOHIDUserDeviceSetDispatchQueue");
        RESOLVE(activate, "IOHIDUserDeviceActivate");
        RESOLVE(cancel, "IOHIDUserDeviceCancel");
        RESOLVE(handleReportWithTimeStamp, "IOHIDUserDeviceHandleReportWithTimeStamp");
#undef RESOLVE
        bool complete = setDispatchQueue && activate && cancel && handleReportWithTimeStamp;
        if (!complete) createWithProperties = nullptr;
        return createWithProperties != nullptr;
    }
};
static UserDeviceApi g_userDevice;

class HidUserPad : public VirtualPad {
public:
    ~HidUserPad() override { close(); }

    bool open(int) override {
        close();
        if (!g_userDevice.load()) {
            snprintf(status_, sizeof status_, "Virtual HID not available on this macOS");
            return false;
        }
        NSDictionary* properties = @{
            @kIOHIDReportDescriptorKey : [NSData dataWithBytes:kReportDescriptor length:sizeof kReportDescriptor],
            @kIOHIDVendorIDKey : @0x1209,
            @kIOHIDProductIDKey : @0x5348,
            @kIOHIDProductKey : @"SpacemouseHOTAS Virtual HOTAS",
            @kIOHIDManufacturerKey : @"SpacemouseHOTAS",
            @kIOHIDTransportKey : @"Virtual",
        };
        device_ = g_userDevice.createWithProperties(kCFAllocatorDefault, (__bridge CFDictionaryRef)properties, 0);
        if (!device_) {
            snprintf(status_, sizeof status_, "Virtual HID denied (needs Apple entitlement)");
            return false;
        }
        // Required sequence for devices created with properties: queue, then activate; cancel before release.
        g_userDevice.setDispatchQueue(device_, dispatch_get_main_queue());
        g_userDevice.activate(device_);
        snprintf(status_, sizeof status_, "Virtual HID ready");
        return true;
    }

    void close() override {
        if (!device_) return;
        g_userDevice.cancel(device_);
        CFRelease(device_);
        device_ = nullptr;
    }

    bool isOpen() const override { return device_ != nullptr; }

    bool send(const float* axes, uint32_t buttons, int hat) override {
        if (!device_) return false;
        uint8_t report[21];
        for (int i = 0; i < kPadAxes; i++) {
            int16_t v = (int16_t)lroundf(fmaxf(-1.0f, fminf(1.0f, axes[i])) * 32767.0f);
            report[i * 2] = (uint8_t)(v & 255);
            report[i * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
        }
        memcpy(report + 16, &buttons, 4);           // little-endian
        report[20] = (uint8_t)(hat < 0 ? 8 : hat);  // 8 is outside 0..7: the null state
        return g_userDevice.handleReportWithTimeStamp(device_, mach_absolute_time(), report, sizeof report) ==
               kIOReturnSuccess;
    }

    const char* status() const override { return status_; }
    const char* help() const override {
        return "Uses IOHIDUserDevice. Needs the com.apple.developer.hid.virtual.device entitlement, which Apple grants "
               "on request.\nGames using the GameController framework ignore virtual devices.";
    }

private:
    IOHIDUserDeviceRef device_ = nullptr;
    char status_[96] = "Virtual HID not opened";
};

static HidUserPad g_pad;

// System services ----------------------------------------------------------------------------

// Key and mouse injection needs the Accessibility permission.
static void injectKey(int vk, bool down) {
    const KeyDef* key = keyByVk(vk);
    if (!key || key->macKey == 0xFF) return;
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, key->macKey, down);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void injectMouseButton(int button, bool down) {
    CGEventRef current = CGEventCreate(nullptr);
    CGPoint position = CGEventGetLocation(current);
    CFRelease(current);
    CGEventType type;
    CGMouseButton cgButton;
    switch (button) {
    case MOUSE_LEFT:
        type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        cgButton = kCGMouseButtonLeft;
        break;
    case MOUSE_RIGHT:
        type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        cgButton = kCGMouseButtonRight;
        break;
    default:  // middle, X1, X2 are "other" buttons 2, 3, 4
        type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        cgButton = (CGMouseButton)button;
        break;
    }
    CGEventRef event = CGEventCreateMouseEvent(nullptr, type, position, cgButton);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static std::string homeDirectory() {
    const char* home = getenv("HOME");
    return home ? home : ".";
}

// A per-user LaunchAgent starts the app at login.
static void setStartWithMacOS(bool enabled) {
    std::string directory = homeDirectory() + "/Library/LaunchAgents";
    std::string path = directory + "/io.github.spacemousehotas.plist";
    if (!enabled) {
        remove(path.c_str());
        return;
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:directory.c_str()]
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSDictionary* agent = @{
        @"Label" : @"io.github.spacemousehotas",
        @"ProgramArguments" : @[ [[NSBundle mainBundle] executablePath] ],
        @"RunAtLoad" : @YES,
    };
    [agent writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES];
}

// Font ---------------------------------------------------------------------------------------

bool loadSystemFont(FontAtlas& font, int pixelSize, bool bold) {
    CTFontRef ctFont =
        CTFontCreateUIFontForLanguage(bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem, pixelSize, nullptr);
    if (!ctFont) return false;
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    font.alpha.clear();
    for (int i = 0; i < 95; i++) {
        UniChar ch = (UniChar)(0x20 + i);
        CGGlyph cgGlyph;
        FontGlyph& glyph = font.glyphs[i];
        if (!CTFontGetGlyphsForCharacters(ctFont, &ch, &cgGlyph, 1)) continue;
        CGSize advance;
        CTFontGetAdvancesForGlyphs(ctFont, kCTFontOrientationHorizontal, &cgGlyph, &advance, 1);
        glyph.advance = (int)lround(advance.width);
        CGRect bounds = CTFontGetBoundingRectsForGlyphs(ctFont, kCTFontOrientationHorizontal, &cgGlyph, nullptr, 1);
        if (bounds.size.width <= 0 || bounds.size.height <= 0) continue;
        int left = (int)floor(bounds.origin.x) - 1, bottom = (int)floor(bounds.origin.y) - 1;
        int right = (int)ceil(bounds.origin.x + bounds.size.width) + 1;
        int top = (int)ceil(bounds.origin.y + bounds.size.height) + 1;
        int width = right - left, height = top - bottom;
        std::vector<uint8_t> coverage(width * height, 0);
        CGContextRef context = CGBitmapContextCreate(coverage.data(), width, height, 8, width, gray, kCGImageAlphaNone);
        CGContextSetGrayFillColor(context, 1.0, 1.0);
        CGContextSetShouldAntialias(context, true);
        CGPoint origin = CGPointMake(-left, -bottom);
        CTFontDrawGlyphs(ctFont, &cgGlyph, &origin, 1, context);
        CGContextRelease(context);
        glyph.width = width;
        glyph.height = height;
        glyph.left = left;
        glyph.top = -top;
        glyph.offset = (int)font.alpha.size();
        font.alpha.insert(font.alpha.end(), coverage.begin(), coverage.end());  // bitmap row 0 is the top
    }
    font.capHeight = (int)lround(CTFontGetCapHeight(ctFont));
    CGColorSpaceRelease(gray);
    CFRelease(ctFont);
    return font.capHeight > 0;
}

static NSImage* makeStatusImage(bool outputEnabled) {
    const int size = 36;  // 18 pt at 2x
    std::vector<uint32_t> argb(size * size);
    makeIcon(argb.data(), size, outputEnabled);
    for (uint32_t& p : argb) {  // CoreGraphics wants premultiplied alpha
        uint32_t a = p >> 24;
        uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(argb.data(), size, size, 8, size * 4, rgb,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(context);
    NSImage* result = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(18, 18)];
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(rgb);
    return result;
}

// 3D mouse input -----------------------------------------------------------------------------

static void onInputReport(void*, IOReturn, void*, IOHIDReportType, uint32_t reportId, uint8_t* report, CFIndex length) {
    if (length > 0 && reportId != 0 && report[0] != (uint8_t)reportId) {
        uint8_t withId[64];  // parser expects the report ID as the first byte
        CFIndex copied = length < 63 ? length : 63;
        withId[0] = (uint8_t)reportId;
        memcpy(withId + 1, report, copied);
        g_app.onDeviceReport(withId, (int)copied + 1);
    } else {
        g_app.onDeviceReport(report, (int)length);
    }
    g_app.update(now());
}

static void onDeviceMatched(void*, IOReturn, void*, IOHIDDeviceRef device) {
    auto intProperty = [&](CFStringRef key) {
        CFTypeRef value = IOHIDDeviceGetProperty(device, key);
        int number = 0;
        if (value && CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &number);
        return number;
    };
    uint16_t vendorId = (uint16_t)intProperty(CFSTR(kIOHIDVendorIDKey));
    uint16_t productId = (uint16_t)intProperty(CFSTR(kIOHIDProductIDKey));
    if (!isSpaceMouse(vendorId, productId)) return;
    char name[128] = {};
    CFTypeRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (product && CFGetTypeID(product) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)product, name, sizeof name, kCFStringEncodingUTF8);
    uint8_t* reportBuffer = (uint8_t*)calloc(1, 64);  // lives as long as the device
    IOHIDDeviceRegisterInputReportCallback(device, reportBuffer, 64, onInputReport, nullptr);
    g_app.onDeviceArrived(vendorId, productId, strncmp(name, "3Dconnexion ", 12) ? name : name + 12);
}

static void onDeviceRemoved(void*, IOReturn, void*, IOHIDDeviceRef) {
    g_app.onDeviceRemoved();
    g_app.update(now());
}

// UI -----------------------------------------------------------------------------------------

@interface SettingsView : NSView
@property(nonatomic) UiInput input;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSStatusItem* statusItem;
@property(strong) NSMenuItem* outputItem;
@property(strong) NSWindow* window;
@property(strong) SettingsView* view;
@property(strong) NSTimer* updateTimer;
@property(strong) NSTimer* redrawTimer;
@property bool statusShowsOutput;
- (void)syncWithApp;
@end

static AppDelegate* g_delegate;

@implementation SettingsView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}
- (int)scale {
    double factor = self.window.backingScaleFactor;
    return factor < 1 ? 1 : (int)lround(factor);
}
- (void)updateTrackingAreas {
    for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
    NSTrackingAreaOptions options = NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways;
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:options owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)drawRect:(NSRect)dirty {
    UiInput in = self.input;
    in.shiftDown = ([NSEvent modifierFlags] & NSEventModifierFlagShift) != 0;
    bool again = g_app.drawFrame(in, [self scale]);
    in.clearEdges();
    self.input = in;
    const Canvas& canvas = g_app.canvas();
    if (canvas.pixels) {
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, canvas.pixels, (size_t)canvas.width * canvas.height * 4, nullptr);
        CGImageRef image = CGImageCreate(canvas.width, canvas.height, 8, 32, canvas.width * 4, rgb,
                                         kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, nullptr,
                                         false, kCGRenderingIntentDefault);
        CGContextRef context = [NSGraphicsContext currentContext].CGContext;
        CGContextSaveGState(context);
        CGContextTranslateCTM(context, 0, App::kHeight);  // flipped view; images draw bottom-up
        CGContextScaleCTM(context, 1, -1);
        CGContextSetInterpolationQuality(context, kCGInterpolationNone);
        CGContextDrawImage(context, CGRectMake(0, 0, App::kWidth, App::kHeight), image);
        CGContextRestoreGState(context);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(rgb);
    }
    if (again) [self setNeedsDisplay:YES];
    [g_delegate syncWithApp];
}
- (void)updateInput:(void (^)(UiInput&))change event:(NSEvent*)event {
    UiInput in = self.input;
    if (event) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        in.mouseX = (int)p.x;
        in.mouseY = (int)p.y;
    }
    change(in);
    self.input = in;
    [self setNeedsDisplay:YES];
    [self displayIfNeeded];
}
- (void)mouseMoved:(NSEvent*)event {
    [self
        updateInput:^(UiInput&) {
        }
              event:event];
}
- (void)mouseDragged:(NSEvent*)event {
    [self
        updateInput:^(UiInput&) {
        }
              event:event];
}
- (void)mouseExited:(NSEvent*)event {
    [self
        updateInput:^(UiInput& in) {
          if (!in.leftDown) in.mouseX = in.mouseY = -1;
        }
              event:nil];
}
- (void)mouseDown:(NSEvent*)event {
    [self
        updateInput:^(UiInput& in) {
          in.leftDown = in.leftPressed = true;
        }
              event:event];
}
- (void)mouseUp:(NSEvent*)event {
    [self
        updateInput:^(UiInput& in) {
          in.leftDown = false;
          in.leftReleased = true;
        }
              event:event];
}
- (void)rightMouseDown:(NSEvent*)event {
    [self
        updateInput:^(UiInput& in) {
          in.rightPressed = true;
        }
              event:event];
}
- (void)otherMouseDown:(NSEvent*)event {
    NSInteger button = event.buttonNumber;
    [self
        updateInput:^(UiInput& in) {
          if (button == 2)
              in.middlePressed = true;
          else if (button == 3)
              in.x1Pressed = true;
          else if (button == 4)
              in.x2Pressed = true;
        }
              event:event];
}
- (void)scrollWheel:(NSEvent*)event {
    CGFloat delta = event.scrollingDeltaY;
    [self
        updateInput:^(UiInput& in) {
          if (delta > 0.5)
              in.wheel++;
          else if (delta < -0.5)
              in.wheel--;
        }
              event:event];
}
- (void)keyDown:(NSEvent*)event {
    const KeyDef* key = keyByMacKeyCode(event.keyCode);
    NSString* characters = event.characters;
    [self
        updateInput:^(UiInput& in) {
          if (key) in.key = key->vk;
          for (NSUInteger i = 0; i < characters.length; i++) {
              unichar c = [characters characterAtIndex:i];
              if (c >= 0x20 && c < 0x7F) in.text += (char)c;
          }
        }
              event:nil];
}
- (void)flagsChanged:(NSEvent*)event {  // modifier keys; report presses only
    NSEventModifierFlags flags = event.modifierFlags;
    bool pressed = false;
    switch (event.keyCode) {
    case 0x38:
    case 0x3C: pressed = flags & NSEventModifierFlagShift; break;
    case 0x3B:
    case 0x3E: pressed = flags & NSEventModifierFlagControl; break;
    case 0x3A:
    case 0x3D: pressed = flags & NSEventModifierFlagOption; break;
    case 0x39: pressed = true; break;  // caps lock
    }
    const KeyDef* key = keyByMacKeyCode(event.keyCode);
    if (pressed && key)
        [self
            updateInput:^(UiInput& in) {
              in.key = key->vk;
            }
                  event:nil];
}
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    std::string directory = homeDirectory() + "/Library/Application Support/SpacemouseHOTAS";
    [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:directory.c_str()]
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    PlatformHooks hooks;
    hooks.injectKey = injectKey;
    hooks.injectMouseButton = injectMouseButton;
    hooks.setStartWithSystem = setStartWithMacOS;
    hooks.startWithSystemLabel = "Start with macOS";
    g_app.init(&g_pad, hooks, directory + "/config.ini");

    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    NSMenu* menu = [[NSMenu alloc] init];
    self.outputItem = [menu addItemWithTitle:@"Output enabled" action:@selector(toggleOutput:) keyEquivalent:@""];
    NSMenuItem* openItem = [menu addItemWithTitle:@"Open window..." action:@selector(openWindow:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quitItem = [menu addItemWithTitle:@"Quit" action:@selector(quit:) keyEquivalent:@"q"];
    for (NSMenuItem* item in @[ self.outputItem, openItem, quitItem ]) item.target = self;
    self.statusItem.menu = menu;
    [self refreshStatusItem];

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSDictionary* matching = @{@kIOHIDDeviceUsagePageKey : @1, @kIOHIDDeviceUsageKey : @8};  // multi-axis controller
    IOHIDManagerSetDeviceMatching(manager, (__bridge CFDictionaryRef)matching);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, onDeviceMatched, nullptr);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, onDeviceRemoved, nullptr);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);

    if (g_app.firstRun || g_app.profile.openWindowAtStart) [self openWindow:nil];
}
- (void)refreshStatusItem {
    bool enabled = g_app.profile.outputEnabled;
    self.statusItem.button.image = makeStatusImage(enabled);
    self.statusItem.button.toolTip = enabled ? @"SpacemouseHOTAS: output on" : @"SpacemouseHOTAS: output off";
    self.outputItem.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
    self.statusShowsOutput = enabled;
}
- (void)syncWithApp {
    if (g_app.timedUpdatesNeeded && !self.updateTimer) {
        self.updateTimer = [NSTimer scheduledTimerWithTimeInterval:0.01
                                                           repeats:YES
                                                             block:^(NSTimer*) {
                                                               g_app.update(now());
                                                               [g_delegate syncWithApp];
                                                             }];
    } else if (!g_app.timedUpdatesNeeded && self.updateTimer) {
        [self.updateTimer invalidate];
        self.updateTimer = nil;
    }
    if (self.statusShowsOutput != g_app.profile.outputEnabled) [self refreshStatusItem];
    if (g_app.hasDeferredActions())
        dispatch_async(dispatch_get_main_queue(), ^{
          g_app.runDeferredActions();
        });
    if (g_app.quitRequested) [self quit:nil];
}
- (void)toggleOutput:(id)sender {
    g_app.setOutputEnabled(!g_app.profile.outputEnabled);
    [self syncWithApp];
    [self.view setNeedsDisplay:YES];
}
- (void)openWindow:(id)sender {
    if (!self.window) {
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, App::kWidth, App::kHeight)
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        self.window.title = @"SpacemouseHOTAS";
        self.window.releasedWhenClosed = NO;
        self.window.delegate = self;
        self.view = [[SettingsView alloc] initWithFrame:NSMakeRect(0, 0, App::kWidth, App::kHeight)];
        self.view.input = UiInput();
        self.window.contentView = self.view;
        [self.window center];
        self.redrawTimer = [NSTimer scheduledTimerWithTimeInterval:0.033
                                                           repeats:YES
                                                             block:^(NSTimer*) {
                                                               if (g_app.redrawNeeded)
                                                                   [g_delegate.view setNeedsDisplay:YES];
                                                             }];
    }
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
}
- (void)windowWillClose:(NSNotification*)notification {
    [self.redrawTimer invalidate];
    self.redrawTimer = nil;
    self.window = nil;
    self.view = nil;
    g_app.releaseCanvas();
    if (g_app.configDirty) g_app.save();
}
- (void)quit:(id)sender {
    g_app.quitRequested = false;
    if (self.window) [self.window close];
    g_app.shutdown();
    [NSApp terminate:nil];
}
@end

int main(int argc, const char** argv) {
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyAccessory];  // menu bar only, no Dock icon
        g_delegate = [[AppDelegate alloc] init];
        application.delegate = g_delegate;
        [application run];
    }
    return 0;
}
