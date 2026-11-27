import Foundation
import SwiftUI

/// Headphone surround › Settings…: the player's own version of the fitted model, heard live while
/// it's tuned and baked for Wine on Save (docs/SURROUND.md, "Settings").
///
/// The live preview is Contents/Helpers/surround_preview (scripts/surround_preview.c): the fit's own
/// model (scripts/surround_fit/render2.c) with these knobs, on the Mac's output directly, so a change
/// is heard at once without Wine. Save has it bake the same filters, gains and tail into
/// SteamDriver/surround-settings.sdsurr, which SteamDriver's run script hands Wine's driver
/// (WINE_HEADPHONE_SURROUND_FILTERS, runtime 206 on) from each game's next launch. Back to the
/// fitted values and saved: the file goes, and the driver plays its built-in fitted filters.
/// One set for every game: the same headphones and ears.
enum SurroundKnobs {
    struct Knob:Identifiable {
        let key:String,title:String,group:String,fitted:Double,range:ClosedRange<Double>,unit:String,help:String
        var id:String { key }
    }
    /// What the page offers, with the fitted values (docs/SURROUND.md, "The fitted model").
    static let all:[Knob]=[
        Knob(key:"front",title:"Front",group:"Speaker positions",fitted:42.6106,range:20...70,unit:"°",help:"How far left and right the front speakers sit"),
        Knob(key:"side",title:"Sides",group:"Speaker positions",fitted:85.3018,range:60...120,unit:"°",help:"Where the side speakers sit (90° is straight out to the side)"),
        Knob(key:"back",title:"Back",group:"Speaker positions",fitted:136.767,range:110...170,unit:"°",help:"Where the back speakers sit (180° is straight behind)"),
        Knob(key:"el_front",title:"Front",group:"Tilt",fitted:-17.9653,range:-30...30,unit:"°",help:"Higher or lower in front. It also changes the overall tone a little: the fitted model evens out the front’s tone for every speaker"),
        Knob(key:"el_side",title:"Sides",group:"Tilt",fitted:17.3703,range:-30...45,unit:"°",help:"Higher or lower at the sides"),
        Knob(key:"el_back",title:"Back",group:"Tilt",fitted:3.02427,range:-30...45,unit:"°",help:"Higher or lower behind"),
        Knob(key:"ild",title:"Left/right",group:"Width",fitted:0.747875,range:0.3...1.5,unit:"×",help:"How much louder the near ear is: more is wider"),
        Knob(key:"itd",title:"Timing",group:"Width",fitted:0.980094,range:0.5...1.5,unit:"×",help:"How much earlier the near ear hears it"),
        Knob(key:"tail",title:"Amount",group:"Room",fitted:0.716398,range:0...2,unit:"×",help:"How much room sound: none is dry and close, more is farther"),
        Knob(key:"rt",title:"Length",group:"Room",fitted:0.43569,range:0.15...1.2,unit:" s",help:"How long the room sound lasts"),
        Knob(key:"lo_g",title:"Bass",group:"Tone",fitted:-3.53549,range:-12...6,unit:" dB",help:"Low frequencies, below about 170 Hz"),
        Knob(key:"hi_g",title:"Treble",group:"Tone",fitted:-9.11723,range:-18...6,unit:" dB",help:"High frequencies, above about 5.6 kHz"),
        Knob(key:"g_side",title:"Sides",group:"Levels",fitted:0.136804,range:-6...6,unit:" dB",help:"The side speakers' level"),
        Knob(key:"g_back",title:"Back",group:"Levels",fitted:-0.535721,range:-6...6,unit:" dB",help:"The back speakers' level"),
    ]
    static let groups=["Speaker positions","Tilt","Width","Room","Tone","Levels"]
    /// The rest of the fitted model, not on the page (render2's knobs).
    static let fixed="ild_lo=0.834084 ild_hi=1.0258 contrast=0.434911 smooth=0.9529 xover=18.1179 eq=0.663981 lo_f=171.974 hi_f=5578.91 d_side=0 d_back=0 early=0 room_lp=1033.36 tail_lp=6271.11 tail_ms=5.04038"
    static var fitted:[String:Double] { Dictionary(uniqueKeysWithValues:all.map { ($0.key,$0.fitted) }) }
}

@MainActor @Observable final class SurroundSettings {
    enum Phase:Equatable { case idle, starting, ready, playing, failed(String) }
    var phase:Phase = .idle
    /// The values heard now, and the ones saved for games.
    var values=SurroundKnobs.fitted
    private(set) var saved=SurroundKnobs.fitted
    var surround=true { didSet { send("surround \(surround ? 1 : 0)") } }
    var volume:Double=0.2 { didSet { send(String(format:"gain %.3f",volume)) } }
    var length:Double=0
    var time:Double=0
    var angle:Double?
    var pattern:Int?
    var fileName:String?
    var fileChannels:Int?
    var notice=""
    var isPlaying:Bool { phase == .playing }
    var changed:Bool { SurroundKnobs.all.contains { abs((values[$0.key] ?? $0.fitted)-(saved[$0.key] ?? $0.fitted)) > 1e-6 } }
    var isFitted:Bool { SurroundKnobs.all.allSatisfy { abs((values[$0.key] ?? $0.fitted)-$0.fitted) < 1e-6 } }

    nonisolated static var settingsFile:URL { NotProton.ours.appendingPathComponent("surround-settings.json") }
    nonisolated static var bakedFile:URL { NotProton.ours.appendingPathComponent("surround-settings.sdsurr") }
    private var task:Process?
    private var input:FileHandle?
    private var pendingSet:Task<Void,Never>?
    private var onBaked:((Bool)->Void)?

    init() {
        if let data=try? Data(contentsOf:Self.settingsFile),
           let json=try? JSONSerialization.jsonObject(with:data) as? [String:Double] {
            for knob in SurroundKnobs.all { if let v=json[knob.key] { saved[knob.key]=min(max(v,knob.range.lowerBound),knob.range.upperBound) } }
        }
        values=saved
    }

    /// Starts the preview (when the page opens).
    func start(resources:URL,helpers:URL) {
        guard task == nil else { return }
        let exe=helpers.appendingPathComponent("surround_preview")
        guard FileManager.default.isExecutableFile(atPath:exe.path) else { phase = .failed("This copy of \(Brand.name) has no live preview.");return }
        let process=Process(),out=Pipe(),inPipe=Pipe()
        process.executableURL=exe
        process.arguments=[resources.appendingPathComponent("SurroundTest/kemar.head").path,resources.appendingPathComponent("SurroundTest/ball-bounce.wav").path]
            + SurroundKnobs.fixed.split(separator:" ").map(String.init) + knobArguments()
        process.standardOutput=out;process.standardInput=inPipe;process.standardError=FileHandle.nullDevice
        var pending=Data()
        out.fileHandleForReading.readabilityHandler={ [weak self] handle in
            let data=handle.availableData
            guard !data.isEmpty else { handle.readabilityHandler=nil;return }
            pending.append(data)
            var lines:[String]=[]
            while let newline=pending.firstIndex(of:0x0A) {
                lines.append(String(decoding:pending[pending.startIndex..<newline],as:UTF8.self).trimmingCharacters(in:.whitespacesAndNewlines))
                pending=Data(pending[(newline+1)...])
            }
            Task { @MainActor in for line in lines { self?.line(line) } }
        }
        process.terminationHandler={ [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.task=nil;self.input=nil
                if self.phase != .idle,!self.phase.isFailure { self.phase = .idle }
            }
        }
        signal(SIGPIPE,SIG_IGN)
        do { try process.run();task=process;input=inPipe.fileHandleForWriting;phase = .starting;send(String(format:"gain %.3f",volume)) }
        catch { phase = .failed("The live preview couldn’t start: \(error.localizedDescription)") }
    }
    /// Ends the preview (when the page closes, or macSteam quits).
    func stop() {
        send("quit")
        let running=task
        Task { try? await Task.sleep(for:.seconds(1));if running?.isRunning == true { running?.terminate() } }
        task=nil;input=nil;phase = .idle;angle=nil
    }

    func playPause() {
        switch phase {
        case .playing: send("pause");phase = .ready
        case .ready: send("play");phase = .playing
        default: break
        }
    }
    func replay() { time=0;send("seek 0");send("play");phase = .playing }
    func seek(to seconds:Double) { time=max(0,min(seconds,length));send(String(format:"seek %.2f",time)) }
    func playFile(_ url:URL) { send("pause");phase = .ready;fileName=url.lastPathComponent;fileChannels=nil;send("file \(url.path)") }
    func backToTest() { send("pause");phase = .ready;fileName=nil;fileChannels=nil;send("test") }

    /// A slider moved: heard after a short pause in the movement (each change fades the sound briefly).
    func set(_ key:String,_ value:Double) {
        values[key]=value
        pendingSet?.cancel()
        pendingSet=Task { @MainActor in
            try? await Task.sleep(for:.milliseconds(120))
            guard !Task.isCancelled else { return }
            self.send("set "+self.knobArguments().joined(separator:" "))
        }
    }
    func resetToFitted() {
        values=SurroundKnobs.fitted
        send("set "+knobArguments().joined(separator:" "))
    }
    /// Save for games: bakes the filters (or, at the fitted values, removes the file so Wine plays
    /// its built-in ones) and keeps the values for next time.
    func save(_ done:@escaping (Bool)->Void) {
        let fm=FileManager.default
        try? fm.createDirectory(at:NotProton.ours,withIntermediateDirectories:true)
        if isFitted {
            try? fm.removeItem(at:Self.bakedFile);try? fm.removeItem(at:Self.settingsFile)
            saved=values;notice="Saved: games use the fitted settings from their next launch.";done(true);return
        }
        guard input != nil else { notice="The live preview isn’t running, so nothing was saved.";done(false);return }
        onBaked={ [weak self] ok in
            guard let self else { return }
            if ok,let data=try? JSONSerialization.data(withJSONObject:self.values,options:[.prettyPrinted,.sortedKeys]) {
                try? data.write(to:Self.settingsFile,options:.atomic)
                self.saved=self.values
                self.notice="Saved: games use these settings from their next launch."
            } else { self.notice="The settings couldn’t be saved." }
            done(ok)
        }
        send("set "+knobArguments().joined(separator:" "))
        send("bake \(Self.bakedFile.path)")
    }
    /// Closing without saving: back to the saved values.
    func discardChanges() { values=saved }

    private func knobArguments()->[String] {
        SurroundKnobs.all.map { String(format:"%@=%.6g",$0.key,values[$0.key] ?? $0.fitted) }
    }
    private func send(_ line:String) {
        guard let input,task?.isRunning == true else { return }
        try? input.write(contentsOf:Data((line+"\n").utf8))
    }
    private func line(_ line:String) {
        let parts=line.split(separator:" ",maxSplits:1).map(String.init)
        guard let head=parts.first else { return }
        let rest=parts.count > 1 ? parts[1] : ""
        switch head {
        case "ready": if phase == .starting { phase = .ready }
        case "length": length=Double(rest) ?? length;time=0
        case "time": time=Double(rest) ?? time
        case "hit":
            let v=rest.split(separator:" ").compactMap { Double($0) }
            if v.count == 2 { angle=v[0];pattern=Int(v[1]) }
        case "end": phase = .ready;time=length;angle=nil
        case "file": fileChannels=Int(rest.split(separator:" ").first ?? "")
        case "baked": onBaked?(true);onBaked=nil
        case "error":
            if let onBaked { onBaked(false);self.onBaked=nil }
            else { notice=rest.prefix(1).uppercased()+rest.dropFirst()+"." }
        default: break
        }
    }
}

extension SurroundSettings.Phase {
    var isFailure:Bool { if case .failed=self { return true } else { return false } }
}
