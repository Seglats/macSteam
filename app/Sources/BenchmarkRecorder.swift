import AppKit
import Carbon.HIToolbox

/// The benchmark hotkey (Benchmarks.hotkey(), Control-Option-B unless changed in the Benchmarks
/// sheet): the first press starts a run in the game being played (Benchmarks.liveSession), the
/// second stops it; the run is then read from the game's Metal HUD log and saved
/// (Benchmarks.analyse). A Carbon hot key works while the game is in front and needs no
/// Accessibility or Input Monitoring permission; the key goes to macSteam, not the game. It is
/// registered only while some game records, so macSteam doesn't take the combo from other apps
/// otherwise. Sounds say what happened, since the game covers the screen: Tink started, Pop
/// stopped (saved), Basso nothing to record.
@MainActor final class BenchmarkRecorder {
    static let shared=BenchmarkRecorder()
    struct Active { var session:Benchmarks.Session; var startUnix:Double; var startHost:Double }
    private(set) var active:Active?
    /// Called with a status line ("" when idle) and, when a run ends, the saved run or an error.
    var changed:((String,Result<Benchmarks.Run,Error>?)->Void)?
    private var hotKey:EventHotKeyRef?
    private var handler:EventHandlerRef?
    private var wanted=false
    /// While the Benchmarks sheet records a new combo, the current one is let go, so pressing it
    /// again reaches the sheet instead of starting a run.
    var suspended=false { didSet { register() } }

    func enable(_ on:Bool) {
        wanted=on
        if !on,active != nil { active=nil;changed?("",nil) }
        register()
    }
    /// Registers the saved combo (again, after it changed), or lets it go.
    func register() {
        if let hotKey { UnregisterEventHotKey(hotKey);self.hotKey=nil }
        guard wanted,!suspended,let (code,mods)=KeyOverrides.keyCode(Benchmarks.hotkey()) else { return }
        if handler==nil {
            var spec=EventTypeSpec(eventClass:OSType(kEventClassKeyboard),eventKind:UInt32(kEventHotKeyPressed))
            InstallEventHandler(GetApplicationEventTarget(),{ _,_,_ in
                Task { @MainActor in BenchmarkRecorder.shared.toggle() };return noErr
            },1,&spec,nil,&handler)
        }
        let flags=[("ctrl",controlKey),("alt",optionKey),("shift",shiftKey),("cmd",cmdKey)].filter { mods.contains($0.0) }.reduce(0) { $0|$1.1 }
        let id=EventHotKeyID(signature:OSType(0x53_44_42_4B),id:1)   // 'SDBK'
        RegisterEventHotKey(UInt32(code),UInt32(flags),id,GetApplicationEventTarget(),0,&hotKey)
    }

    func toggle() {
        if let run=active {
            active=nil
            let stopUnix=Date().timeIntervalSince1970,stopHost=Benchmarks.hostSeconds()
            let hz=NSScreen.main?.maximumFramesPerSecond ?? 0
            NSSound(named:"Pop")?.play()
            changed?("Reading the run in \(run.session.game)…",nil)
            // The HUD writes a line about every 0.7 s, after its frames: wait for the last one.
            Task.detached(priority:.userInitiated) {
                try? await Task.sleep(for:.seconds(1.5))
                let result=Result { () throws -> Benchmarks.Run in
                    let saved=try Benchmarks.analyse(run.session,startUnix:run.startUnix,stopUnix:stopUnix,startHost:run.startHost,stopHost:stopHost,refreshHz:hz)
                    try Benchmarks.save(saved);return saved
                }
                await MainActor.run { BenchmarkRecorder.shared.changed?("",result) }
            }
            return
        }
        guard var session=Benchmarks.liveSession() else {
            NSSound(named:"Basso")?.play()
            changed?("",.failure(Benchmarks.Failure.message("No game is recording. Start it after turning recording on for it (or with STEAMDRIVER_BENCH=1), then press \(Benchmarks.hotkeyTitle) again.")))
            return
        }
        // Read from here on: everything before this press is older than the run.
        session.logOffset=max(session.logOffset,Benchmarks.size(session.log))
        active=Active(session:session,startUnix:Date().timeIntervalSince1970,startHost:Benchmarks.hostSeconds())
        NSSound(named:"Tink")?.play()
        changed?("Recording \(session.game). Press \(Benchmarks.hotkeyTitle) again to stop.",nil)
    }
}

extension AppModel {
    func loadBenchmarks() {
        benchRecord=Benchmarks.record()
        benchHotkey=Benchmarks.hotkey()
        benchRuns=Benchmarks.runs()
        let recorder=BenchmarkRecorder.shared
        recorder.changed={ [weak self] status,result in
            guard let self else { return }
            benchStatus=status
            switch result {
            case .success(let run)?:
                benchRuns=Benchmarks.runs()
                notice="Benchmark saved: \(run.game), \(Int(run.summary.avgFPS.rounded())) fps average over \(Int(run.summary.seconds.rounded())) s."
            case .failure(let error)?: notice=error.localizedDescription
            case nil: break
            }
        }
        recorder.enable(benchRecord != .off)
    }
    func setBenchRecord(_ record:Benchmarks.Record) {
        do { try Benchmarks.setRecord(record) } catch { notice=error.localizedDescription }
        benchRecord=Benchmarks.record()
        BenchmarkRecorder.shared.enable(benchRecord != .off)
    }
    /// A new hotkey from the Benchmarks sheet; nil goes back to the default. Says if macOS or
    /// another app may also use it (RegisterEventHotKey fails for a combo already registered).
    func setBenchHotkey(_ combo:String?) {
        do { try Benchmarks.setHotkey(combo ?? Benchmarks.defaultHotkey) } catch { notice=error.localizedDescription;return }
        benchHotkey=Benchmarks.hotkey()
        BenchmarkRecorder.shared.register()
        if let use=KeyOverrides.macOSUse(benchHotkey) {
            notice="\(Benchmarks.hotkeyTitle) is also macOS’s shortcut for \(use); while recording is on, \(Brand.name) takes it."
        }
    }
    func deleteRun(_ run:Benchmarks.Run) {
        Benchmarks.delete(run);benchRuns=Benchmarks.runs()
    }
    func renameRun(_ run:Benchmarks.Run,_ label:String) {
        var renamed=run;renamed.label=label.trimmingCharacters(in:.whitespaces)
        do { try Benchmarks.save(renamed) } catch { notice=error.localizedDescription }
        benchRuns=Benchmarks.runs()
    }
}
