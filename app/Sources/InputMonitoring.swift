import Foundation
import IOKit
import IOKit.hid

/// Settings › General › Mouse › High-precision (AppModel.MouseInput.highPrecision, ported from
/// Recall 1.6): the game reads the mouse's own reports through IOHID instead of Apple's mouse input
/// for games (GCMouse), which drops some of them in game. Reading a mouse that way needs macOS's
/// Input Monitoring. To macOS the game runs as part of this app, so the permission is the app's:
/// it is asked for when the player chooses High-precision, never from inside the game, and only
/// the player can change it afterwards, in System Settings. macOS's prompt speaks of keystrokes;
/// the window driver opens only mice and reads only their movement and buttons (cocoa_app.m
/// hid_start). Without the permission the mouse works as Standard does.
enum InputMonitoring {
    enum Access:String { case allowed, denied, undecided }
    static var access:Access {
        switch IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) {
        case kIOHIDAccessTypeGranted: return .allowed
        case kIOHIDAccessTypeDenied: return .denied
        default: return .undecided
        }
    }
    /// Shows macOS's prompt the first time; the player then turns the app on in System Settings.
    static func ask() { _ = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent) }
    static let privacySettings=URL(string:"x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent")!
    /// Whether a mouse High-precision would read is connected, by the window driver's rule
    /// (cocoa_app.m hid_is_trackpad): a HID device with a mouse collection that is neither built
    /// in nor named a trackpad. It reads the I/O Registry, which needs no permission. From Recall.
    static var mouseConnected:Bool {
        let matching=IOServiceMatching(kIOHIDDeviceKey) as NSMutableDictionary
        matching[kIOHIDDeviceUsagePageKey]=kHIDPage_GenericDesktop
        matching[kIOHIDDeviceUsageKey]=kHIDUsage_GD_Mouse
        var services:io_iterator_t=0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,matching,&services) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(services) }
        var found=false
        while case let service=IOIteratorNext(services), service != 0 {
            defer { IOObjectRelease(service) }
            if !found && !isTrackpad(service) { found=true }
        }
        return found
    }
    private static func isTrackpad(_ service:io_service_t)->Bool {
        if IORegistryEntryCreateCFProperty(service,kIOHIDBuiltInKey as CFString,kCFAllocatorDefault,0)?.takeRetainedValue() as? Bool == true { return true }
        let product=IORegistryEntryCreateCFProperty(service,kIOHIDProductKey as CFString,kCFAllocatorDefault,0)?.takeRetainedValue() as? String
        return product?.range(of:"Trackpad",options:.caseInsensitive) != nil
    }
}
