import SwiftUI
import AppKit
import IOKit.hid

/// "Problems with your controller?": a guided check whose report says what is needed to support a
/// controller better (ControllerCheck.swift). Pages: before you start, controllers found, a few
/// seconds untouched, one prompt per control, report. Nothing is sent; the report is a file.
@MainActor @Observable
final class ControllerCheckModel {
    enum Page { case intro, permission, found, resting, steps, report }
    var page=Page.intro
    let capture=ControllerCapture()
    var usbPads:[ControllerScan.USBPad]=[]
    var gameControllers:[String]=[]
    var hidNames:[String]=[]
    var dinput:[String]=[]
    var xPads:[Int]=[]
    var wineReady=false
    var wineFailed:String?
    var stepIndex=0
    var results:[ControllerStepResult]=[]
    var waitingRelease=false
    var stepStarted=Date()
    var restLeft=0
    var reportFile:URL?
    var notice=""
    private var poll:Task<Void,Never>?
    private var started=false
    var step:ControllerStep? { stepIndex < ControllerStep.all.count ? ControllerStep.all[stepIndex] : nil }

    /// Opens the controllers (the only point where macOS could ask for permission) and starts the Wine probe.
    func begin(resources:URL) {
        usbPads=ControllerScan.usbPads()
        gameControllers=ControllerScan.gameControllers()
        if !started {
            capture.startHID()
            if capture.openResult == kIOReturnNotPermitted { page = .permission;return }
            started=true
            capture.startWine(probe:resources.appendingPathComponent("ControllerCheck/controller_probe.exe"))
            startPolling()
        }
        page = .found
    }
    func retryPermission(resources:URL) {
        capture.stop();started=false
        begin(resources:resources)
    }
    private func startPolling() {
        poll=Task { [weak self] in
            while !Task.isCancelled {
                self?.tick()
                try? await Task.sleep(for:.milliseconds(50))
            }
        }
    }
    private func tick() {
        capture.lock.lock()
        hidNames=capture.devices.values.map { "\($0.name.isEmpty ? "Controller" : $0.name) (\($0.transport))" }.sorted()
        dinput=capture.dinput;xPads=capture.xState.keys.sorted();wineReady=capture.wineReady;wineFailed=capture.wineFailed
        let raw=capture.rawHit,x=capture.xHit
        capture.lock.unlock()
        guard page == .steps,let step else { return }
        if waitingRelease {
            if capture.released || Date().timeIntervalSince(stepStarted) > 3 { waitingRelease=false;startStep() }
            return
        }
        let wantX=wineReady && step.expected != nil && !xPads.isEmpty
        let gotRaw=raw != nil || hidNames.isEmpty
        // Done once both sides answered, or one side answered and the other stayed quiet for a second.
        if (gotRaw && (x != nil || !wantX)) || ((raw != nil || x != nil) && Date().timeIntervalSince(stepStarted) > 1.5) {
            finishStep(raw:raw,x:x)
        }
    }
    func startResting() {
        page = .resting;restLeft=3;capture.setResting(true)
        Task {
            for left in stride(from:3,to:0,by:-1) { restLeft=left;try? await Task.sleep(for:.seconds(1)) }
            capture.setResting(false)
            page = .steps;stepIndex=0;results=[];startStep()
        }
    }
    private func startStep() { capture.arm();stepStarted=Date() }
    private func finishStep(raw:ControllerCapture.RawHit?,x:ControllerCapture.XHit?,skipped:Bool=false) {
        capture.disarm()
        guard let step else { return }
        results.append(ControllerStepResult(step:step.id,expected:step.expected,raw:raw,xinput:x?.field,skipped:skipped))
        stepIndex+=1
        if stepIndex >= ControllerStep.all.count { finish();return }
        waitingRelease=true;stepStarted=Date()
    }
    func skip() { finishStep(raw:nil,x:nil,skipped:true) }
    func redo() {
        guard !results.isEmpty else { return }
        results.removeLast();stepIndex-=1;waitingRelease=false;startStep()
    }
    private func finish() {
        poll?.cancel();page = .report
        do { reportFile=try writeReport() } catch { notice="Couldn’t save the report: \(error.localizedDescription)" }
        capture.stop()
    }
    func close() { poll?.cancel();capture.stop() }

    // MARK: Report
    var summary:String {
        let ok=results.filter { $0.verdict == "ok" }.count
        let wrong=results.filter { $0.verdict == "wrong" }.map(\.step)
        let missing=results.filter { $0.verdict == "missing" }.map(\.step)
        var lines=["Controllers: "+(hidNames+usbPads.map { "\($0.name) (\($0.mode))" }).joined(separator:", ")]
        lines.append("\(ok) of \(results.count) controls reach games correctly.")
        if !wrong.isEmpty { lines.append("Wrong in games: "+wrong.joined(separator:", ")) }
        if !missing.isEmpty { lines.append("Not seen by games: "+missing.joined(separator:", ")) }
        return lines.joined(separator:"\n")
    }
    private func writeReport()throws->URL {
        func hex(_ bytes:[UInt8])->String { bytes.map { String(format:"%02x",$0) }.joined(separator:" ") }
        func stats(_ v:[Double])->[String:Double] {
            guard v.count > 10 else { return [:] }
            let s=v.sorted()
            return ["median_ms":s[s.count/2],"p95_ms":s[s.count*95/100],"max_ms":s[s.count-1],"count":Double(s.count)]
        }
        capture.lock.lock()
        let devices=capture.devices.values.map { d->[String:Any] in
            ["name":d.name,"manufacturer":d.manufacturer,"transport":d.transport,
             "vendor":String(format:"%04x",d.vendor),"product":String(format:"%04x",d.product),"version":d.version,
             "usage":String(format:"%02x:%02x",d.usagePage,d.usage),"reports":d.reports,
             "hid_descriptor":hex([UInt8](d.descriptor)),"interval_untouched":stats(d.idle),"interval_in_use":stats(d.busy),
             "moves_at_rest":d.restRange.keys.sorted().map { [$0,d.restRange[$0]!] },"samples":d.samples.prefix(20).map(hex)]
        }
        let dinput=capture.dinput
        capture.lock.unlock()
        let wineLines=((try? String(contentsOf:ControllerCapture.wineLog,encoding:.utf8)) ?? "")
            .split(separator:"\n").filter { $0.contains("plugplay") || $0.contains("xinput") || $0.contains("hid") || $0.contains("err:") }
            .suffix(400).map(String.init)
        let steps=results.map { r->[String:Any] in
            var e:[String:Any]=["step":r.step,"verdict":r.verdict]
            if let x=r.expected { e["expected"]=x }
            if let x=r.xinput { e["xinput"]=x }
            if let raw=r.raw { e["raw"]=["device":raw.device,"changed":raw.diffs] }
            return e
        }
        let report:[String:Any]=[
            "kind":"macSteam controller check","date":ISO8601DateFormatter().string(from:Date()),
            "macos":ProcessInfo.processInfo.operatingSystemVersionString,
            "app":Bundle.main.object(forInfoDictionaryKey:"CFBundleShortVersionString") as? String ?? "?",
            "steamdriver_runtime":GameTools.bundled(Bundle.main.resourceURL!).first.flatMap { GameTools.installedRuntime($0) } ?? "not installed",
            "more_controller_support":["on":ControllerSupport.enabled,"listed":ControllerSupport.saved().map(\.id),
                                       "steam_started_with":ControllerSupport.steamValue() ?? ""],
            "hid_controllers":devices,
            "usb_xbox_mode":usbPads.map { ["name":$0.name,"vendor":String(format:"%04x",$0.vendor),"product":String(format:"%04x",$0.product),
                                           "mode":$0.mode,"held_by":$0.holders] },
            "macos_game_controllers":gameControllers,
            "windows_directinput":dinput,"windows_xinput_pads":xPads,
            "steps":steps,"wine_log":wineLines,
        ]
        let folder=NotProton.ours.appendingPathComponent("controller-reports")
        try FileManager.default.createDirectory(at:folder,withIntermediateDirectories:true)
        let stamp=DateFormatter();stamp.dateFormat="yyyyMMdd-HHmmss"
        let file=folder.appendingPathComponent("controller-check-\(stamp.string(from:Date())).json")
        try JSONSerialization.data(withJSONObject:report,options:[.prettyPrinted,.sortedKeys]).write(to:file)
        return file
    }
    var issueURL:URL {
        var c=URLComponents(url:Brand.issues.appendingPathComponent("new"),resolvingAgainstBaseURL:false)!
        c.queryItems=[URLQueryItem(name:"title",value:"Controller support: "+(hidNames.first ?? usbPads.first?.name ?? "controller")),
                      URLQueryItem(name:"body",value:summary+"\n\nPlease attach the report file from macSteam’s controller check (drag it here).")]
        return c.url ?? Brand.issues
    }
}

struct ControllerCheckSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var check=ControllerCheckModel()

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:"Controller check",subtitle:"",compact:true)
            Group {
                switch check.page {
                case .intro: intro
                case .permission: permission
                case .found: found
                case .resting: resting
                case .steps: steps
                case .report: report
                }
            }.frame(maxWidth:.infinity,alignment:.leading)
            if !check.notice.isEmpty { LauncherNotice(text:check.notice) }
            HStack {
                Button(check.page == .report ? "Done" : "Cancel") { check.close();dismiss() }
                    .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).keyboardShortcut(.cancelAction)
                Spacer()
                primaryButton
            }
        }.font(LauncherStyle.font(14)).padding(.horizontal,32).padding(.vertical,28).frame(width:600)
            .tint(.primary).background { LauncherBackground() }
            .onDisappear { check.close() }
    }

    @ViewBuilder private var primaryButton:some View {
        switch check.page {
        case .intro: Button("Start") { check.begin(resources:model.resources) }.buttonStyle(.borderedProminent).hoverGrow().buttonBorderShape(.capsule)
        case .found: Button("Test the buttons") { check.startResting() }.buttonStyle(.borderedProminent).hoverGrow().buttonBorderShape(.capsule)
            .disabled(check.hidNames.isEmpty && check.xPads.isEmpty)
        case .permission: Button("Try Again") { check.retryPermission(resources:model.resources) }.buttonStyle(.borderedProminent).hoverGrow().buttonBorderShape(.capsule)
        case .steps:
            HStack {
                Button("Back") { check.redo() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).disabled(check.results.isEmpty)
                Button("Skip") { check.skip() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
            }
        default: EmptyView()
        }
    }

    private func para(_ text:String)->some View {
        Text(text).font(LauncherStyle.font(13)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
    }

    private var intro:some View {
        VStack(alignment:.leading,spacing:10) {
            para("This check finds out how your controller reaches games, so support for it can be added or fixed.")
            para("1. Connect the controller the way you play: dongle, cable or Bluetooth, in the mode you use.\n2. Close your games.\n3. Follow the prompts: press each button and move each stick when asked. It takes about two minutes.")
            para("It records what the controller sends and what Windows games see through SteamDriver, and saves a report file. Nothing is sent anywhere; you choose whether to attach the file to a GitHub issue.")
        }
    }
    private var permission:some View {
        VStack(alignment:.leading,spacing:10) {
            para("macOS didn’t let \(Brand.name) read the controller. Allow \(Brand.name) under Input Monitoring in System Settings, then come back and press Try Again.")
            Button("Open Input Monitoring Settings") { InputMonitoring.ask();NSWorkspace.shared.open(InputMonitoring.privacySettings) }
                .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
        }
    }
    private var found:some View {
        VStack(alignment:.leading,spacing:8) {
            row("Controllers macOS can read",check.hidNames.isEmpty ? "None" : check.hidNames.joined(separator:"\n"))
            if !check.usbPads.isEmpty {
                row("In Xbox mode (USB)",check.usbPads.map { pad in
                    "\(pad.name), \(pad.mode)"+(pad.holders.isEmpty ? "" : " — held by \(pad.holders.joined(separator:", "))")
                }.joined(separator:"\n"))
                if check.usbPads.contains(where:{ $0.holders.contains("steam_osx") }) {
                    para("Steam is holding this controller, so games can’t see it. Turn on More controller support to hand it to your games.")
                }
            }
            row("macOS game controller list",check.gameControllers.isEmpty ? "None" : check.gameControllers.joined(separator:"\n"))
            row("What Windows games see",windowsLine)
        }
    }
    private var windowsLine:String {
        if let failed=check.wineFailed { return failed }
        if !check.wineReady { return "Starting the SteamDriver Wine… (the first time takes up to a minute)" }
        var parts:[String]=[]
        parts.append(check.xPads.isEmpty ? "No XInput controller" : "XInput controller \(check.xPads.map { "\($0+1)" }.joined(separator:", "))")
        if !check.dinput.isEmpty { parts.append("DirectInput: "+check.dinput.joined(separator:", ")) }
        return parts.joined(separator:"\n")
    }
    private func row(_ title:String,_ value:String)->some View {
        VStack(alignment:.leading,spacing:2) {
            Text(title).font(LauncherStyle.font(12,.semibold))
            Text(value).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
        }
    }
    private var resting:some View {
        VStack(alignment:.leading,spacing:10) {
            Text("Don’t touch the controller… \(check.restLeft)").font(LauncherStyle.font(18,.semibold))
            para("This learns which values move on their own, so small drift isn’t taken for a press.")
        }
    }
    private var steps:some View {
        VStack(alignment:.leading,spacing:12) {
            if let step=check.step {
                Text("Step \(check.stepIndex+1) of \(ControllerStep.all.count)").font(LauncherStyle.font(12)).foregroundStyle(.secondary)
                Text(check.waitingRelease ? "Let go" : step.prompt).font(LauncherStyle.font(20,.semibold))
                ProgressView(value:Double(check.stepIndex),total:Double(ControllerStep.all.count))
            }
            if let last=check.results.last {
                Text("Last: \(last.step) — \(verdictText(last))").font(LauncherStyle.font(12)).foregroundStyle(.secondary)
            }
        }
    }
    private func verdictText(_ r:ControllerStepResult)->String {
        switch r.verdict {
        case "ok": return "games see it correctly"
        case "wrong": return "games see \(r.xinput ?? "something else")"
        case "missing": return r.raw != nil ? "the controller sent it, games didn’t see it" : "nothing detected"
        case "raw-only": return "the controller sent it (games can’t show this one)"
        default: return "skipped"
        }
    }
    private var report:some View {
        VStack(alignment:.leading,spacing:10) {
            Text(check.summary).font(LauncherStyle.font(13)).fixedSize(horizontal:false,vertical:true)
            if let file=check.reportFile {
                para("Report saved: \(file.lastPathComponent)")
                HStack(spacing:10) {
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([file]) }
                        .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                    Button("Copy Summary") {
                        NSPasteboard.general.clearContents();NSPasteboard.general.setString(check.summary,forType:.string)
                    }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                    Button("Report on GitHub…") { NSWorkspace.shared.open(check.issueURL) }
                        .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                }
                para("On GitHub, drag the report file into the issue so the controller can be supported.")
            }
        }
    }
}
