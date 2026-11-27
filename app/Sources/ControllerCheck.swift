import Foundation
import IOKit
import IOKit.hid
import GameController

/// The controller check behind "Problems with your controller?" (ControllerCheckSheet): collects
/// what is needed to support a controller better, and nothing is sent anywhere; the player gets a
/// report file to attach to an issue.
///
/// - Controllers found: HID game controllers (IOHIDManager, with their HID report descriptors),
///   USB interfaces in Xbox 360 (255/93/1) and Xbox One (255/71/208) mode and which programs hold
///   them, and what macOS's GameController framework lists.
/// - Raw input: every HID input report with the kernel's timestamp (report rates, untouched and in
///   use), the bytes that move at rest, and per guided step which bytes changed.
/// - What Windows games see: controller_probe.exe (scripts/controller_probe.c), shipped in the app
///   (Resources/ControllerCheck), run with the installed SteamDriver tool's Wine in a throwaway prefix
///   of its own (SteamDriver/controller-check/prefix), never in a game's prefix or the runtime. It
///   lists DirectInput controllers and streams XInput state; Wine's +plugplay lines go in the report.
///
/// Nothing is opened before the check's test page; macOS normally asks no permission to read
/// game controllers, so permission is only requested if it refuses (IOHIDManagerOpen).
final class ControllerCapture:@unchecked Sendable {
    struct Device {
        var key:String, name:String, manufacturer:String, transport:String
        var vendor:Int, product:Int, version:Int, usagePage:Int, usage:Int
        var descriptor:Data
        var last:[UInt8]=[], lastTime=0.0, lastChange=0.0, reports=0
        var idle:[Double]=[], busy:[Double]=[]
        var restBase:[UInt8]?, restRange:[Int:Int]=[:]
        var samples:[[UInt8]]=[]
    }
    struct RawHit { var device:String; var diffs:[[Int]] }   // [byte index, from, to]
    struct XHit { var pad:Int; var field:String }

    let lock=NSLock()
    var devices:[String:Device]=[:]
    var openResult:IOReturn=kIOReturnSuccess
    var opened=false
    var resting=false
    var armed=false
    var baseline:[String:[UInt8]]=[:]
    var rawHit:RawHit?
    // What Windows games see (controller_probe.exe)
    var xState:[Int:[Int]]=[:]          // pad -> buttons, lt, rt, lx, ly, rx, ry
    var xBase:[Int:[Int]]=[:]
    var xHit:XHit?
    var dinput:[String]=[]
    var wineReady=false
    var wineFailed:String?

    private var runLoop:CFRunLoop?
    private var manager:IOHIDManager?
    private var contexts:[DeviceContext]=[]
    private var wine:Process?
    private var wineInput:Pipe?

    static let clock:Double={ var t=mach_timebase_info_data_t();mach_timebase_info(&t);return Double(t.numer)/Double(t.denom)/1e6 }()

    final class DeviceContext {
        let key:String;unowned let capture:ControllerCapture;let buffer:UnsafeMutablePointer<UInt8>;let size:Int
        init(key:String,capture:ControllerCapture,size:Int) {
            self.key=key;self.capture=capture;self.size=size;buffer = .allocate(capacity:size)
        }
        deinit { buffer.deallocate() }
    }

    // MARK: HID capture
    /// Opens the HID game controllers on a thread of its own; returns once the open was tried.
    func startHID() {
        let started=DispatchSemaphore(value:0)
        let thread=Thread { [self] in
            let manager=IOHIDManagerCreate(kCFAllocatorDefault,IOOptionBits(kIOHIDOptionsTypeNone))
            let matches=[(1,4),(1,5),(1,8)].map { [kIOHIDDeviceUsagePageKey:$0.0,kIOHIDDeviceUsageKey:$0.1] as CFDictionary }
            IOHIDManagerSetDeviceMatchingMultiple(manager,matches as CFArray)
            IOHIDManagerRegisterDeviceMatchingCallback(manager,{ context,_,_,device in
                Unmanaged<ControllerCapture>.fromOpaque(context!).takeUnretainedValue().added(device)
            },Unmanaged.passUnretained(self).toOpaque())
            IOHIDManagerScheduleWithRunLoop(manager,CFRunLoopGetCurrent(),CFRunLoopMode.defaultMode.rawValue)
            let result=IOHIDManagerOpen(manager,IOOptionBits(kIOHIDOptionsTypeNone))
            lock.lock();openResult=result;opened=result == kIOReturnSuccess;self.manager=manager;runLoop=CFRunLoopGetCurrent();lock.unlock()
            started.signal()
            if result == kIOReturnSuccess { CFRunLoopRun() }
        }
        thread.name="controller check"
        thread.start()
        started.wait()
    }
    private func added(_ device:IOHIDDevice) {
        func string(_ key:String)->String { IOHIDDeviceGetProperty(device,key as CFString) as? String ?? "" }
        func number(_ key:String)->Int { IOHIDDeviceGetProperty(device,key as CFString) as? Int ?? 0 }
        let vendor=number(kIOHIDVendorIDKey),product=number(kIOHIDProductIDKey)
        let key=String(format:"%04x:%04x %@ %d",vendor,product,string(kIOHIDTransportKey),number(kIOHIDLocationIDKey))
        let size=max(number(kIOHIDMaxInputReportSizeKey),64)
        let entry=Device(key:key,name:string(kIOHIDProductKey),manufacturer:string(kIOHIDManufacturerKey),
                         transport:string(kIOHIDTransportKey),vendor:vendor,product:product,version:number(kIOHIDVersionNumberKey),
                         usagePage:number(kIOHIDPrimaryUsagePageKey),usage:number(kIOHIDPrimaryUsageKey),
                         descriptor:IOHIDDeviceGetProperty(device,kIOHIDReportDescriptorKey as CFString) as? Data ?? Data())
        let context=DeviceContext(key:key,capture:self,size:size)
        lock.lock();devices[key]=entry;contexts.append(context);lock.unlock()
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(device,context.buffer,size,{ ctx,_,_,_,_,report,length,timestamp in
            let context=Unmanaged<DeviceContext>.fromOpaque(ctx!).takeUnretainedValue()
            context.capture.report(context.key,Array(UnsafeBufferPointer(start:report,count:length)),Double(timestamp)*ControllerCapture.clock)
        },Unmanaged.passUnretained(context).toOpaque())
    }
    private func report(_ key:String,_ bytes:[UInt8],_ time:Double) {
        lock.lock();defer { lock.unlock() }
        guard var d=devices[key] else { return }
        if d.lastTime > 0 {
            let gap=time-d.lastTime
            if time-d.lastChange < 100 { if d.busy.count < 20000 { d.busy.append(gap) } }
            else if d.idle.count < 20000 { d.idle.append(gap) }
        }
        if bytes != d.last {
            d.lastChange=time
            if d.samples.count < 40 { d.samples.append(bytes) }
        }
        if resting {
            if let base=d.restBase {
                for i in 0..<min(base.count,bytes.count) where base[i] != bytes[i] {
                    d.restRange[i]=max(d.restRange[i] ?? 0,abs(Int(base[i])-Int(bytes[i])))
                }
            } else { d.restBase=bytes }
        }
        if armed,rawHit == nil,let base=baseline[key] {
            let diffs=(0..<min(base.count,bytes.count)).compactMap { i->[Int]? in
                let change=abs(Int(base[i])-Int(bytes[i]))
                guard change > 0 else { return nil }
                if let noise=d.restRange[i],change <= max(24,noise*2) { return nil }
                return [i,Int(base[i]),Int(bytes[i])]
            }
            if !diffs.isEmpty { rawHit=RawHit(device:key,diffs:diffs) }
        }
        d.last=bytes;d.lastTime=time;d.reports+=1
        devices[key]=d
    }

    /// Whether every device is back within its noise of the step's baseline (the control was let go).
    var released:Bool {
        lock.lock();defer { lock.unlock() }
        for (key,d) in devices {
            guard let base=baseline[key] else { continue }
            for i in 0..<min(base.count,d.last.count) {
                let change=abs(Int(base[i])-Int(d.last[i]))
                if change > 0,change > max(24,(d.restRange[i] ?? 0)*2) { return false }
            }
        }
        for (pad,base) in xBase {
            guard let now=xState[pad] else { continue }
            if now[0] != base[0] || abs(now[1]-base[1]) > 64 || abs(now[2]-base[2]) > 64 { return false }
            if (3..<7).contains(where:{ abs(now[$0]-base[$0]) > 12000 }) { return false }
        }
        return true
    }
    func arm() {
        lock.lock()
        baseline=devices.mapValues(\.last);xBase=xState;rawHit=nil;xHit=nil;armed=true
        lock.unlock()
    }
    func disarm() { lock.lock();armed=false;lock.unlock() }
    func setResting(_ on:Bool) { lock.lock();resting=on;lock.unlock() }

    // MARK: What Windows games see
    static var toolWine:URL { NotProton.steamTools.appendingPathComponent("notproton-steamdriver/runtime/bin/wine") }
    static var folder:URL { NotProton.ours.appendingPathComponent("controller-check") }
    static var wineLog:URL { folder.appendingPathComponent("wine.log") }

    func startWine(probe:URL) {
        guard FileManager.default.isExecutableFile(atPath:Self.toolWine.path) else {
            lock.lock();wineFailed="The SteamDriver tool isn’t installed, so what games see can’t be checked.";lock.unlock();return
        }
        try? FileManager.default.createDirectory(at:Self.folder.appendingPathComponent("prefix"),withIntermediateDirectories:true)
        FileManager.default.createFile(atPath:Self.wineLog.path,contents:nil)
        let task=Process()
        task.executableURL=Self.toolWine
        task.arguments=[probe.path,"900"]
        var env=ProcessInfo.processInfo.environment
        env["WINEPREFIX"]=Self.folder.appendingPathComponent("prefix").path
        env["WINEDEBUG"]="err+all,+plugplay"
        env["WINEDLLOVERRIDES"]="mscoree,mshtml="
        task.environment=env
        let out=Pipe(),input=Pipe()
        task.standardOutput=out;task.standardInput=input
        task.standardError=(try? FileHandle(forWritingTo:Self.wineLog)) ?? FileHandle.nullDevice
        var pending=""
        out.fileHandleForReading.readabilityHandler={ [weak self] handle in
            let data=handle.availableData
            guard !data.isEmpty,let self,let text=String(data:data,encoding:.utf8) else { return }
            pending+=text
            while let newline=pending.firstIndex(of:"\n") {
                let line=String(pending[..<newline]);pending=String(pending[pending.index(after:newline)...])
                self.wineLine(line)
            }
        }
        do { try task.run() } catch {
            lock.lock();wineFailed="Couldn’t start the SteamDriver Wine: \(error.localizedDescription)";lock.unlock();return
        }
        lock.lock();wine=task;wineInput=input;lock.unlock()
    }
    private func wineLine(_ line:String) {
        let parts=line.split(separator:" ").map(String.init)
        lock.lock();defer { lock.unlock() }
        if line == "ready" { wineReady=true;return }
        if parts.first == "dinput" { dinput.append(parts.dropFirst().joined(separator:" "));return }
        guard parts.count >= 3,parts[0] == "x",let pad=Int(parts[1]) else { return }
        if parts[2] == "off" { xState[pad]=nil;return }
        guard parts.count == 9,let buttons=Int(parts[2],radix:16) else { return }
        let values=[buttons]+parts[3...].compactMap { Int($0) }
        guard values.count == 7 else { return }
        if armed,xHit == nil {
            let base=xBase[pad] ?? [0,0,0,0,0,0,0]
            let pressed=values[0] & ~base[0]
            if pressed != 0 { xHit=XHit(pad:pad,field:String(format:"button 0x%04x",pressed)) }
            else if values[1] > 128,base[1] < 64 { xHit=XHit(pad:pad,field:"lt") }
            else if values[2] > 128,base[2] < 64 { xHit=XHit(pad:pad,field:"rt") }
            else {
                let axes=["lx","ly","rx","ry"]
                for (i,name) in axes.enumerated() where abs(values[3+i]-base[3+i]) > 16000 {
                    xHit=XHit(pad:pad,field:name+(values[3+i] > base[3+i] ? "+" : "-"));break
                }
            }
        }
        xState[pad]=values
    }

    func stop() {
        lock.lock()
        let loop=runLoop,manager=self.manager,task=wine,input=wineInput
        runLoop=nil;self.manager=nil;wine=nil;wineInput=nil
        lock.unlock()
        if let manager { IOHIDManagerClose(manager,IOOptionBits(kIOHIDOptionsTypeNone)) }
        if let loop { CFRunLoopStop(loop) }
        try? input?.fileHandleForWriting.close()
        if let task {
            DispatchQueue.global().async {
                for _ in 0..<20 where task.isRunning { Thread.sleep(forTimeInterval:0.1) }
                if task.isRunning { task.terminate() }
                let server=Self.toolWine.deletingLastPathComponent().appendingPathComponent("wineserver")
                let kill=Process();kill.executableURL=server;kill.arguments=["-k"]
                kill.environment=["WINEPREFIX":Self.folder.appendingPathComponent("prefix").path]
                try? kill.run();kill.waitUntilExit()
            }
        }
    }
}

/// Controllers seen outside HID: USB interfaces in Xbox mode and who holds them, and macOS's own list.
enum ControllerScan {
    struct USBPad:Codable { var name:String;var vendor:Int;var product:Int;var mode:String;var holders:[String] }
    static func usbPads()->[USBPad] {
        var pads:[USBPad]=[]
        var iter:io_iterator_t=0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostInterface"),&iter) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iter) }
        while true {
            let entry=IOIteratorNext(iter)
            if entry == 0 { break }
            defer { IOObjectRelease(entry) }
            let c=number(entry,"bInterfaceClass"),s=number(entry,"bInterfaceSubClass"),p=number(entry,"bInterfaceProtocol")
            let mode:String
            if c == 255,s == 93,p == 1 { mode="Xbox 360 (XInput)" } else if c == 255,s == 71,p == 208 { mode="Xbox One (GIP)" } else { continue }
            var parent:io_registry_entry_t=0
            guard IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent) == KERN_SUCCESS else { continue }
            defer { IOObjectRelease(parent) }
            let name=(IORegistryEntryCreateCFProperty(parent,"USB Product Name" as CFString,kCFAllocatorDefault,0)?.takeRetainedValue() as? String) ?? "Controller"
            var holders:[String]=[]
            var children:io_iterator_t=0
            if IORegistryEntryGetChildIterator(entry,kIOServicePlane,&children) == KERN_SUCCESS {
                while true {
                    let child=IOIteratorNext(children)
                    if child == 0 { break }
                    if IOObjectConformsTo(child,"AppleUSBHostInterfaceUserClient") != 0 {
                        var buffer=[CChar](repeating:0,count:128)
                        if IORegistryEntryGetName(child,&buffer) == KERN_SUCCESS { holders.append(String(cString:buffer)) }
                    }
                    IOObjectRelease(child)
                }
                IOObjectRelease(children)
            }
            pads.append(USBPad(name:name,vendor:number(parent,"idVendor") ?? 0,product:number(parent,"idProduct") ?? 0,mode:mode,holders:holders))
        }
        return pads
    }
    private static func number(_ entry:io_registry_entry_t,_ key:String)->Int? {
        IORegistryEntryCreateCFProperty(entry,key as CFString,kCFAllocatorDefault,0)?.takeRetainedValue() as? Int
    }
    @MainActor static func gameControllers()->[String] {
        GCController.controllers().map { c in
            [c.vendorName ?? "?",c.productCategory,c.extendedGamepad != nil ? "full gamepad" : ""].filter { !$0.isEmpty }.joined(separator:", ")
        }
    }
}

/// The guided steps and what a Windows game should see for each (XInput).
struct ControllerStep:Identifiable {
    let id:String
    let prompt:String
    /// The XInput change expected; nil when XInput can't show it (Guide).
    let expected:String?
    static let all:[ControllerStep]=[
        .init(id:"A",prompt:"Press A (bottom face button)",expected:"button 0x1000"),
        .init(id:"B",prompt:"Press B (right face button)",expected:"button 0x2000"),
        .init(id:"X",prompt:"Press X (left face button)",expected:"button 0x4000"),
        .init(id:"Y",prompt:"Press Y (top face button)",expected:"button 0x8000"),
        .init(id:"LB",prompt:"Press the left bumper (LB / L1)",expected:"button 0x0100"),
        .init(id:"RB",prompt:"Press the right bumper (RB / R1)",expected:"button 0x0200"),
        .init(id:"LT",prompt:"Pull the left trigger all the way (LT / L2)",expected:"lt"),
        .init(id:"RT",prompt:"Pull the right trigger all the way (RT / R2)",expected:"rt"),
        .init(id:"Back",prompt:"Press Back / View / Select / Minus",expected:"button 0x0020"),
        .init(id:"Start",prompt:"Press Start / Menu / Plus",expected:"button 0x0010"),
        .init(id:"Guide",prompt:"Press the Guide / Home button",expected:nil),
        .init(id:"LS",prompt:"Click the left stick in (L3)",expected:"button 0x0040"),
        .init(id:"RS",prompt:"Click the right stick in (R3)",expected:"button 0x0080"),
        .init(id:"DUp",prompt:"Press D-pad up",expected:"button 0x0001"),
        .init(id:"DDown",prompt:"Press D-pad down",expected:"button 0x0002"),
        .init(id:"DLeft",prompt:"Press D-pad left",expected:"button 0x0004"),
        .init(id:"DRight",prompt:"Press D-pad right",expected:"button 0x0008"),
        .init(id:"LUp",prompt:"Push the left stick fully up",expected:"ly+"),
        .init(id:"LDown",prompt:"Push the left stick fully down",expected:"ly-"),
        .init(id:"LLeft",prompt:"Push the left stick fully left",expected:"lx-"),
        .init(id:"LRight",prompt:"Push the left stick fully right",expected:"lx+"),
        .init(id:"RUp",prompt:"Push the right stick fully up",expected:"ry+"),
        .init(id:"RDown",prompt:"Push the right stick fully down",expected:"ry-"),
        .init(id:"RLeft",prompt:"Push the right stick fully left",expected:"rx-"),
        .init(id:"RRight",prompt:"Push the right stick fully right",expected:"rx+"),
    ]
}

/// One step's outcome.
struct ControllerStepResult:Codable {
    var step:String
    var expected:String?
    var raw:ControllerCapture.RawHit?
    var xinput:String?
    var skipped=false
    /// ok, wrong (games see something else), missing (games see nothing), raw-only, skipped
    var verdict:String {
        if skipped { return "skipped" }
        guard let expected else { return raw != nil ? "raw-only" : "missing" }
        guard let xinput else { return "missing" }
        return xinput == expected ? "ok" : "wrong"
    }
}
extension ControllerCapture.RawHit:Codable {}
