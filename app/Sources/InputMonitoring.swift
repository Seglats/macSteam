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
}
