import Foundation
import IOKit

/// Experimental "More controller support": controllers in Xbox (XInput) mode for SteamDriver games.
///
/// Many third-party controllers (8BitDo pads in their normal mode, and others) speak Microsoft's
/// Xbox 360 USB protocol (USB interface class 255, subclass 93, protocol 1). That isn't HID, so
/// macOS has no driver for them and macOS Steam reads them itself, through its SDL3 and libusb,
/// holding them exclusively the whole time, even with Steam Input for Xbox controllers off and
/// while a game runs. The SteamDriver Wine (winebus.sys's Xbox 360 bus,
/// wine-winebus-xinput-any-pad.patch, runtime macfinals-153) reads them directly at their full rate
/// (1000 Hz for the 8BitDo Ultimate 2), but only if Steam lets go.
///
/// SDL's SDL_HIDAPI_IGNORE_DEVICES in Steam's environment makes Steam leave the listed controllers
/// alone. Steam is started by launchd, so this sets it with `launchctl setenv` now and, through a
/// LaunchAgent, at every login; Steam reads it when it starts. Microsoft's own controllers (vendor
/// 045e) are never listed: Steam keeps them, and on macOS 15+ the Mac drives them itself.
/// Tests 2026-11-25: environments/finals/tests/controller_latency/README.md.
enum ControllerSupport {
    struct Pad:Codable,Hashable,Identifiable {
        var vendor:Int
        var product:Int
        var name:String
        var id:String { Self.key(vendor,product) }
        static func key(_ vendor:Int,_ product:Int)->String { String(format:"0x%04x/0x%04x",vendor,product) }
    }

    static let variable="SDL_HIDAPI_IGNORE_DEVICES"
    static let agentLabel="app.steamdriver.controllers"
    /// Under NotProton.home, which the uninstaller tests point at a temporary folder.
    static var agent:URL { NotProton.home.appendingPathComponent("Library/LaunchAgents/\(agentLabel).plist") }
    /// The controllers handed to games, kept so ones not connected right now stay listed.
    static var listFile:URL { NotProton.ours.appendingPathComponent("xinput-controllers.json") }
    static let microsoft=0x045e

    /// On: the login item exists.
    static var enabled:Bool { FileManager.default.fileExists(atPath:agent.path) }

    static func saved()->[Pad] {
        guard let data=try? Data(contentsOf:listFile),let pads=try? JSONDecoder().decode([Pad].self,from:data) else { return [] }
        return pads
    }

    /// Connected USB devices with an Xbox 360 wired interface, Microsoft's excluded.
    static func connected()->[Pad] {
        var pads:[Pad]=[]
        var iter:io_iterator_t=0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostInterface"),&iter) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iter) }
        while case let entry=IOIteratorNext(iter),entry != 0 {
            defer { IOObjectRelease(entry) }
            guard number(entry,"bInterfaceClass") == 255,number(entry,"bInterfaceSubClass") == 93,
                  number(entry,"bInterfaceProtocol") == 1 else { continue }
            var parent:io_registry_entry_t=0
            guard IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent) == KERN_SUCCESS else { continue }
            defer { IOObjectRelease(parent) }
            guard let vendor=number(parent,"idVendor"),let product=number(parent,"idProduct"),vendor != microsoft else { continue }
            let name=(IORegistryEntryCreateCFProperty(parent,"USB Product Name" as CFString,kCFAllocatorDefault,0)?
                .takeRetainedValue() as? String) ?? "Controller"
            let pad=Pad(vendor:vendor,product:product,name:name)
            if !pads.contains(pad) { pads.append(pad) }
        }
        return pads
    }
    private static func number(_ entry:io_registry_entry_t,_ key:String)->Int? {
        IORegistryEntryCreateCFProperty(entry,key as CFString,kCFAllocatorDefault,0)?.takeRetainedValue() as? Int
    }

    /// The value Steam needs for these controllers.
    static func value(_ pads:[Pad])->String { pads.map(\.id).sorted().joined(separator:",") }

    /// The value the running Steam started with, read from its environment (nil: not running or unset).
    static func steamValue()->String? {
        let pgrep=Process();pgrep.executableURL=URL(fileURLWithPath:"/usr/bin/pgrep");pgrep.arguments=["-x","steam_osx"]
        let out=Pipe();pgrep.standardOutput=out;pgrep.standardError=FileHandle.nullDevice
        guard (try? pgrep.run()) != nil else { return nil }
        pgrep.waitUntilExit()
        guard let pid=String(data:out.fileHandleForReading.readDataToEndOfFile(),encoding:.utf8)?
            .split(separator:"\n").first.map(String.init) else { return nil }
        let ps=Process();ps.executableURL=URL(fileURLWithPath:"/bin/ps");ps.arguments=["eww","-o","command=","-p",pid]
        let psOut=Pipe();ps.standardOutput=psOut;ps.standardError=FileHandle.nullDevice
        guard (try? ps.run()) != nil else { return nil }
        let text=String(data:psOut.fileHandleForReading.readDataToEndOfFile(),encoding:.utf8) ?? ""
        ps.waitUntilExit()
        guard let range=text.range(of:variable+"=") else { return nil }
        return String(text[range.upperBound...].prefix { !$0.isWhitespace })
    }

    enum Failure:LocalizedError {
        case noControllers
        var errorDescription:String? {
            "Connect the controller in its Xbox mode (8BitDo: normal mode) by its dongle or cable, then try again."
        }
    }

    /// Turns it on, or adds newly connected controllers: saves the list, sets Steam's environment
    /// now and at every login. Steam applies it from its next start.
    @discardableResult
    static func enable()throws->[Pad] {
        var pads=saved()
        for pad in connected() where !pads.contains(where:{ $0.id == pad.id }) { pads.append(pad) }
        guard !pads.isEmpty else { throw Failure.noControllers }
        try FileManager.default.createDirectory(at:NotProton.ours,withIntermediateDirectories:true)
        try JSONEncoder().encode(pads).write(to:listFile,options:.atomic)
        let setting=value(pads)
        let plist:[String:Any]=[
            "Label":agentLabel,
            "ProgramArguments":["/bin/launchctl","setenv",variable,setting],
            "RunAtLoad":true,
        ]
        try FileManager.default.createDirectory(at:agent.deletingLastPathComponent(),withIntermediateDirectories:true)
        try PropertyListSerialization.data(fromPropertyList:plist,format:.xml,options:0).write(to:agent,options:.atomic)
        _ = NotProton.tool("/bin/launchctl",["setenv",variable,setting])
        return pads
    }

    /// Turns it off: Steam takes these controllers again from its next start.
    static func disable() {
        try? FileManager.default.removeItem(at:agent)
        _ = NotProton.tool("/bin/launchctl",["unsetenv",variable])
    }
}
