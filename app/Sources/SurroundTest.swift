import Foundation
import SwiftUI

/// The Test beside Experimental › Headphone surround: scripts/surround_test.c (shipped as
/// Resources/SurroundTest/surround_test.exe) with the installed SteamDriver tool's Wine, in a
/// prefix of its own (SteamDriver/surround-test/prefix), never a game's. It plays a 7.1 stream the
/// way a game does. Two players run at once on one timeline (-sync): one with
/// WINE_HEADPHONE_SURROUND set (On), one without (Off: a plain stereo mix of the same sound, what a
/// game makes for a stereo output without the setting; the player downmixes, since macOS handed 7.1
/// would drop centre, sides and backs). Both always play the same place and only the chosen one is heard, so
/// switching On and Off swaps at once, without a gap or a restart. The On player takes the
/// commands for both and reports the place. Its sound is one ball bounce
/// (Resources/SurroundTest/ball-bounce.wav, Pixabay Content License) repeated at different spots
/// around you in five patterns (scripts/surround_signal.h); Play a File… plays a WAV of the
/// player's own (5.1, 7.1, …) through the same path.
@MainActor @Observable final class SurroundTest {
    enum Phase:Equatable { case idle, preparing, playing, paused, failed(String) }
    var phase:Phase = .idle
    /// The speaker heard now (FL, FC, FR, SR, BR, BL, SL) in the speakers test.
    var speaker:String?
    /// Where the ball is now, degrees clockwise from the front, and which pattern (1-5) is playing.
    var angle:Double?
    var pattern:Int?
    static let patternNames=["a slow lap clockwise","two fast laps counterclockwise","jumps clockwise, three hits a spot","front and back","right and left"]
    /// A file's name; the test's or file's length and how far it has played (seconds).
    var fileName:String?
    /// The file's channels ("file <channels> <rate> <seconds>"): surround needs more than 2.
    var fileChannels:Int?
    var length:Double=0
    var time:Double=0
    /// Which player is heard; switching while it plays swaps at once.
    var surround=true {
        didSet { if oldValue != surround { send("surround \(surround ? 1 : 0)") } }
    }
    /// The test's volume, 0…1: 20 % each time the app starts, so nobody gets blasted; not saved.
    var volume:Double=0.2 {
        didSet { send(String(format:"gain %.3f",volume)) }
    }

    nonisolated static var folder:URL { NotProton.ours.appendingPathComponent("surround-test") }
    nonisolated static var prefix:URL { folder.appendingPathComponent("prefix") }
    nonisolated static var tool:URL { NotProton.steamTools.appendingPathComponent("notproton-steamdriver") }
    nonisolated static var wine:URL { tool.appendingPathComponent("runtime/bin/wine") }
    nonisolated static let speakerNames=["FL":"Front left","FC":"Center","FR":"Front right","SR":"Right side","BR":"Back right","BL":"Back left","SL":"Left side"]

    /// The installed tool's runtime, and whether it has headphone surround (macfinals-192 on).
    nonisolated static func runtime()->(version:String,hasSurround:Bool)? {
        guard let data=try? Data(contentsOf:tool.appendingPathComponent("runtime/runtime.json")),
              let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any],let version=json["version"] as? String else { return nil }
        let number=Int(version.split(separator:"-").last ?? "") ?? 0
        return (version,number >= 192)
    }

    /// The On player (takes the commands, reports the place) and the Off player.
    private var lead:TestPlayer?
    private var follow:TestPlayer?
    private var ready:Set<Bool>=[]
    private var finished=false
    private var file:URL?
    private var generation=0
    var isBusy:Bool { phase == .playing || phase == .preparing || phase == .paused }

    /// The play/pause button: pauses, goes on, or starts (where the bar is; from the start at the end).
    func playPause(_ exe:URL) {
        switch phase {
        case .playing: send("pause");phase = .paused;stopWhenIdle()
        case .paused: send("play");phase = .playing
        case .preparing: break
        default: start(exe,file:file,at:time < length-0.1 ? time : 0)
        }
    }
    /// Replay: from the start (the test, or the last file), playing.
    func replay(_ exe:URL) {
        guard lead != nil,phase == .playing || phase == .paused else { start(exe,file:file,at:0);return }
        time=0;send("seek 0")
        if phase == .paused { send("play");phase = .playing }
    }
    /// The position bar: jumps there (paused stays paused); when stopped, Play starts there.
    func seek(to seconds:Double) {
        time=max(0,length > 0 ? min(seconds,length) : seconds)
        send(String(format:"seek %.2f",time))
    }
    /// Play a File…, or back to the test (nil): from the start.
    func play(_ exe:URL,file:URL?) { start(exe,file:file,at:0) }

    func stop() {
        let wasRunning=endPlayers()
        if phase != .idle,!phase.isFailure { phase = .idle }
        if wasRunning { Task.detached(priority:.utility) { Self.killServer() } }
    }

    /// macSteam quitting: ends the players and waits for their Wine server (a second or so).
    func stopForQuit() {
        if endPlayers() { Self.killServer() }
    }

    private func send(_ line:String) {
        guard let lead,lead.isRunning else { return }
        try? lead.input.write(contentsOf:Data((line+"\n").utf8))
    }
    /// Paused for 5 minutes: the players end (Play starts them again at the same place).
    private func stopWhenIdle() {
        let mine=generation
        Task { @MainActor in
            try? await Task.sleep(for:.seconds(300))
            if mine == self.generation,self.phase == .paused { self.stop() }
        }
    }

    private func start(_ exe:URL,file:URL?,at start:Double) {
        let wasRunning=endPlayers()
        self.file=file
        if fileName != file?.lastPathComponent { length=0 }
        if fileName != file?.lastPathComponent { fileChannels=nil }
        fileName=file?.lastPathComponent;time=start
        guard FileManager.default.isExecutableFile(atPath:Self.wine.path) else {
            phase = .failed("The SteamDriver tool isn’t installed in Steam, so there’s no Wine to test with.");return
        }
        guard FileManager.default.fileExists(atPath:exe.path) else { phase = .failed("This copy of \(Brand.name) has no surround test.");return }
        phase = .preparing
        let mine=generation
        Task.detached(priority:.userInitiated) {
            if wasRunning { Self.killServer() }
            let failure=Self.preparePrefix()
            let ready=failure,path=file
            await MainActor.run {
                guard mine == self.generation else { return }
                if let ready { self.phase = .failed(ready);return }
                self.launch(exe,file:path,start:start,generation:mine)
            }
        }
    }
    /// Ends both players; true when they were running (their prefix's wineserver then goes too).
    private func endPlayers()->Bool {
        generation+=1
        speaker=nil;angle=nil;pattern=nil
        let running=(lead?.isRunning ?? false) || (follow?.isRunning ?? false)
        for player in [lead,follow].compactMap({ $0 }) {
            player.output.readabilityHandler=nil
            player.terminate()
            try? player.input.close()
        }
        lead=nil;follow=nil;ready=[];finished=false
        return running || isBusy
    }
    nonisolated private static func killServer() {
        let server=wine.deletingLastPathComponent().appendingPathComponent("wineserver")
        guard FileManager.default.isExecutableFile(atPath:server.path) else { return }
        let kill=Process();kill.executableURL=server;kill.arguments=["-k"]
        kill.environment=["WINEPREFIX":prefix.path]
        if (try? kill.run()) != nil { kill.waitUntilExit() }
    }

    /// First run: the prefix is a copy of the runtime's shared template when there is one (about a
    /// second, docs/RUNTIMES.md); otherwise Wine makes it at the first start (about 15 s).
    nonisolated private static func preparePrefix()->String? {
        let fm=FileManager.default
        if fm.fileExists(atPath:prefix.appendingPathComponent("system.reg").path) { return nil }
        do { try fm.createDirectory(at:folder,withIntermediateDirectories:true) } catch { return error.localizedDescription }
        try? fm.removeItem(at:prefix)
        if let version=runtime()?.version {
            let template=NotProton.ours.appendingPathComponent("prefix-templates/\(version)")
            if fm.fileExists(atPath:template.appendingPathComponent(".complete").path) {
                let cp=Process();cp.executableURL=URL(fileURLWithPath:"/bin/cp");cp.arguments=["-c","-R","-p",template.path+"/.",prefix.path]
                try? fm.createDirectory(at:prefix,withIntermediateDirectories:true)
                if (try? cp.run()) != nil { cp.waitUntilExit();if cp.terminationStatus == 0 { try? fm.removeItem(at:prefix.appendingPathComponent(".complete"));return nil } }
                try? fm.removeItem(at:prefix)
            }
        }
        return nil
    }

    private func launch(_ exe:URL,file:URL?,start:Double,generation mine:Int) {
        FileManager.default.createFile(atPath:Self.log.path,contents:nil)
        let sync="SteamDriverSurroundTest-\(ProcessInfo.processInfo.processIdentifier)-\(mine)"
        for on in [true,false] {
            do {
                let player=try player(exe,file:file,start:start,sync:sync,on:on,generation:mine)
                if on { lead=player } else { follow=player }
            } catch {
                if on { phase = .failed("Couldn’t start the SteamDriver Wine: \(error.localizedDescription)");_=endPlayers();return }
            }
        }
        // Both players ready: play. A slow Off player joins at the right place when it's ready.
        Task { @MainActor in
            try? await Task.sleep(for:.seconds(20))
            if mine == self.generation,self.phase == .preparing,self.ready.contains(true) { self.begin() }
        }
    }
    private func begin() {
        guard phase == .preparing else { return }
        send("play");phase = .playing
    }

    private func player(_ exe:URL,file:URL?,start:Double,sync:String,on:Bool,generation mine:Int) throws->TestPlayer {
        // Windows paths for Wine (Z: is the Mac's root)
        func windows(_ url:URL)->String { "Z:"+url.path.replacingOccurrences(of:"/",with:"\\") }
        let clip=exe.deletingLastPathComponent().appendingPathComponent("ball-bounce.wav")
        var arguments=[exe.path,"-gain",String(format:"%.3f",volume),"-sync",sync,"-voice",on ? "on" : "off"]
        if !surround { arguments.append("-off") }
        if start > 0 { arguments+=["-start",String(format:"%.2f",start)] }
        arguments+=file.map { [windows($0)] } ?? ["-test",windows(clip)]
        var env=ProcessInfo.processInfo.environment
        env["WINEPREFIX"]=Self.prefix.path
        env["WINEDEBUG"]="err+all"
        env["WINEDLLOVERRIDES"]="mscoree,mshtml="
        if on { env["WINE_HEADPHONE_SURROUND"]="1" } else { env.removeValue(forKey:"WINE_HEADPHONE_SURROUND") }
        let log=(try? FileHandle(forWritingTo:Self.log)).map { $0.seekToEndOfFile();return $0 }
        let player=try TestPlayer(Self.wine,arguments:arguments,environment:env,standardError:log) { [weak self] in
            guard let self,mine == self.generation,on else { return }   // the On player ending ends the test
            if self.finished { self.time=self.length }   // played to the end: the bar stays full
            self.stop()
        }
        // Lines end in \r\n (Wine's C runtime writes Windows line ends), which Swift's String takes
        // as one character, not "\n": split the bytes at the newline byte instead.
        var pending=Data()
        player.output.readabilityHandler={ [weak self] handle in
            let data=handle.availableData
            guard !data.isEmpty else { handle.readabilityHandler=nil;return }
            pending.append(data)
            var lines:[String]=[]
            while let newline=pending.firstIndex(of:0x0A) {
                lines.append(String(decoding:pending[pending.startIndex..<newline],as:UTF8.self).trimmingCharacters(in:.whitespacesAndNewlines))
                pending=Data(pending[(newline+1)...])
            }
            Task { @MainActor in for line in lines { self?.line(line,on:on,generation:mine) } }
        }
        return player
    }
    nonisolated static var log:URL { folder.appendingPathComponent("wine.log") }

    private func line(_ line:String,on:Bool,generation mine:Int) {
        guard mine == generation else { return }
        let parts=line.split(separator:" ",maxSplits:1).map(String.init)
        guard let head=parts.first else { return }
        let rest=parts.count > 1 ? parts[1] : ""
        if head == "ready" {
            ready.insert(on)
            if ready.count == 2 { begin() }
            return
        }
        guard on else { return }   // the Off player follows; the On player reports
        switch head {
        case "speaker": speaker=rest
        case "hit":
            let values=rest.split(separator:" ").compactMap { Double($0) }
            if values.count == 2 { angle=values[0];pattern=Int(values[1]) }
        case "file": fileChannels=Int(rest.split(separator:" ").first ?? "")
        case "length": length=Double(rest) ?? length
        case "time": if phase != .preparing { time=Double(rest) ?? time }
        case "done": finished=true
        case "error": phase = .failed(file == nil ? "The test couldn’t play: \(rest)." : "\(fileName ?? "The file") couldn’t play: \(rest).")
        default: break
        }
    }
}

/// One test player: the SteamDriver Wine as a child process of macSteam. On an output device
/// that also has inputs (an audio interface such as a Scarlett, Loopback), Core Audio files a
/// microphone request when playback starts, and a child's request is macSteam's: one answer for
/// macSteam, the permission its voice chat uses. Run as its own process (tried 2026-11-27), Wine
/// became an app of its own to macOS, which asked about "wine" and held playback until answered.
@MainActor final class TestPlayer {
    private let task=Process()
    let input:FileHandle,output:FileHandle
    var isRunning:Bool { task.isRunning }
    init(_ executable:URL,arguments:[String],environment:[String:String],standardError:FileHandle?,onExit:@escaping @MainActor ()->Void) throws {
        let inPipe=Pipe(),outPipe=Pipe()
        task.executableURL=executable;task.arguments=arguments;task.environment=environment
        task.standardInput=inPipe;task.standardOutput=outPipe;task.standardError=standardError ?? FileHandle.nullDevice
        input=inPipe.fileHandleForWriting;output=outPipe.fileHandleForReading
        task.terminationHandler={ _ in Task { @MainActor in onExit() } }
        signal(SIGPIPE,SIG_IGN)   // writing to a player that has ended must not end macSteam
        try task.run()
    }
    func terminate() { if task.isRunning { task.terminate() } }
}

extension SurroundTest.Phase {
    var isFailure:Bool { if case .failed=self { return true } else { return false } }
}


/// A head with the seven 7.1 speakers around it (top is in front); the one playing lights up, and
/// the bouncing ball shows as a dot where it is.
struct SurroundSpeakers:View {
    var active:String?
    var angle:Double?
    private static let speakers:[(String,Double)]=[("FL",-30),("FC",0),("FR",30),("SR",90),("BR",150),("BL",-150),("SL",-90)]
    var body:some View {
        ZStack {
            Circle().strokeBorder(Color.primary.opacity(0.12),lineWidth:1).frame(width:54,height:54)
            Image(systemName:"headphones").font(.system(size:15,weight:.medium)).foregroundStyle(.secondary)
            ForEach(Self.speakers,id:\.0) { speaker in
                let on=speaker.0 == active
                Circle().fill(on ? LauncherStyle.accent : Color.primary.opacity(0.25))
                    .frame(width:on ? 10 : 7,height:on ? 10 : 7)
                    .offset(x:27*sin(speaker.1 * .pi/180),y:-27*cos(speaker.1 * .pi/180))
                    .animation(.easeOut(duration:0.12),value:on)
            }
            if let angle {
                Circle().fill(LauncherStyle.accent).frame(width:11,height:11)
                    .offset(x:27*sin(angle * .pi/180),y:-27*cos(angle * .pi/180))
                    .animation(.easeOut(duration:0.25),value:angle)
            }
        }.frame(width:66,height:66).accessibilityElement().accessibilityLabel(active.flatMap { SurroundTest.speakerNames[$0] } ?? "Speakers")
    }
}
