import Foundation
import Darwin

/// Benchmarks: fps, frame times and latency of a stretch of play, started and stopped with a
/// hotkey (BenchmarkRecorder). docs/BENCHMARKS.md describes what is recorded and how it is
/// computed, so the same benchmark can be run on any Wine build.
///
/// Sources, all written while the game runs:
/// - Apple's Metal HUD log (MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1, the HUD itself hidden with
///   MTL_HUD_OPACITY=0): one line on the game's stderr about every 0.7 s,
///   `<date> <process>[<pid>:<tid>] metal-HUD: <frame count>,<a>,<b>,<interval ms>,<GPU ms>,…`,
///   one interval/GPU pair per frame since the previous line. Same on D3DMetal and DXMT and on
///   every Wine, because Metal itself writes it. The SteamDriver tool's stderr goes to
///   compatdata/<id>/steamdriver-wine.log, NotProton's to compatdata/<id>/notproton-run.log.
/// - The renderer's per-frame present log, for the time from a frame's present to the display:
///   D3DMetal through SteamDriver's Mac driver (WINEMAC_INPUT_STATS, d3dm-frames-<pid>.csv) or
///   DXMT's own (DXMT_DISPLAY_LOG, display-<pid>.csv). Both on the host clock (CACurrentMediaTime).
/// The SteamDriver tool's run script turns these on and writes a session folder per launch
/// (benchmarks/sessions/<app id>-<date>/session.json); a NotProton game is found by its log.
enum Benchmarks {
    static var folder:URL { NotProton.ours.appendingPathComponent("benchmarks") }
    static var recordFile:URL { folder.appendingPathComponent("record") }
    static var sessionsFolder:URL { folder.appendingPathComponent("sessions") }
    static var runsFolder:URL { folder.appendingPathComponent("runs") }
    private static let fm=FileManager.default

    // MARK: The hotkey (benchmarks/hotkey: one combo as in KeyOverrides, "ctrl+alt+b")
    static let defaultHotkey="ctrl+alt+b"
    static var hotkeyFile:URL { folder.appendingPathComponent("hotkey") }
    static func hotkey()->String {
        let saved=((try? String(contentsOf:hotkeyFile,encoding:.utf8)) ?? "").trimmingCharacters(in:.whitespacesAndNewlines)
        return hotkeyProblem(saved)==nil ? saved : defaultHotkey
    }
    static var hotkeyTitle:String { KeyOverrides.caps(hotkey()).joined() }
    /// Why a combo can't be the hotkey, or nil. It is taken system-wide while recording is on,
    /// so it needs Control, Option or Command, except a function key (F1–F20) alone.
    static func hotkeyProblem(_ combo:String)->String? {
        guard let (_,mods)=KeyOverrides.keyCode(combo) else { return "That key can’t be a hotkey." }
        let key=combo.split(separator:"+").last.map(String.init) ?? ""
        let functionKey=key.count>1 && key.hasPrefix("f") && Int(key.dropFirst()) != nil
        if mods.isDisjoint(with:["ctrl","alt","cmd"]) && !functionKey {
            return "Use Control, Option or Command with the key (or a function key alone), so typing elsewhere isn’t taken."
        }
        return nil
    }
    static func setHotkey(_ combo:String) throws {
        if combo==defaultHotkey { try? fm.removeItem(at:hotkeyFile);return }
        if let problem=hotkeyProblem(combo) { throw Failure.message(problem) }
        try write(combo+"\n",to:hotkeyFile)
    }

    // MARK: Which games record (read by the run script: "all" or one app id per line)
    enum Record:Equatable { case off, all, games(Set<String>) }
    static func record()->Record {
        let lines=((try? String(contentsOf:recordFile,encoding:.utf8)) ?? "").split(whereSeparator:\.isNewline).map { $0.trimmingCharacters(in:.whitespaces) }
        if lines.contains("all") { return .all }
        let games=Set(lines.filter(Profiles.valid))
        return games.isEmpty ? .off : .games(games)
    }
    static func setRecord(_ record:Record) throws {
        switch record {
        case .off: try? fm.removeItem(at:recordFile)
        case .all: try write("all\n",to:recordFile)
        case .games(let ids):
            if ids.isEmpty { try? fm.removeItem(at:recordFile) } else { try write(ids.sorted().joined(separator:"\n")+"\n",to:recordFile) }
        }
    }
    private static func write(_ text:String,to url:URL) throws {
        try fm.createDirectory(at:url.deletingLastPathComponent(),withIntermediateDirectories:true)
        try text.write(to:url,atomically:true,encoding:.utf8)
    }

    // MARK: Sessions: a running game whose HUD log can be read
    struct Session:Equatable {
        var dir:URL?
        var appID:String
        var game:String
        var source:String          // "SteamDriver" or "CrossOver (NotProton)"
        var renderer:String        // d3dmetal, dxmt or "" (unknown)
        var runtime:String
        var upscaling:Bool
        var log:URL
        var logOffset:UInt64
    }
    /// The game being played now: the newest SteamDriver session whose run script is still
    /// running, else a NotProton game whose log got a Metal HUD line in the last few seconds.
    static func liveSession(now:Date=Date())->Session? {
        let dirs=((try? fm.contentsOfDirectory(at:sessionsFolder,includingPropertiesForKeys:[.contentModificationDateKey])) ?? [])
            .sorted { modified($0) > modified($1) }
        for dir in dirs where !fm.fileExists(atPath:dir.appendingPathComponent("ended").path) {
            guard let data=try? Data(contentsOf:dir.appendingPathComponent("session.json")),
                  let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any],
                  let pid=json["script_pid"] as? Int,kill(pid_t(pid),0)==0,
                  let log=json["wine_log"] as? String else { continue }
            return Session(dir:dir,appID:json["app_id"] as? String ?? "",game:json["game"] as? String ?? "",source:"SteamDriver",
                           renderer:json["renderer"] as? String ?? "",runtime:json["runtime"] as? String ?? "",
                           upscaling:(json["upscaling"] as? Int ?? 0)==1,log:URL(fileURLWithPath:log),
                           logOffset:(json["wine_log_offset"] as? NSNumber)?.uint64Value ?? 0)
        }
        // NotProton (CrossOver): no session folder; its log carries the HUD lines when the game
        // was started with MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 in its launch options.
        var best:(Session,Date)?
        for library in NotProton.steamLibraries() {
            let compat=library.appendingPathComponent("steamapps/compatdata")
            for app in (try? fm.contentsOfDirectory(atPath:compat.path)) ?? [] where Profiles.valid(app) {
                let log=compat.appendingPathComponent(app).appendingPathComponent("notproton-run.log")
                let date=modified(log)
                guard now.timeIntervalSince(date)<10,tail(log,bytes:65536).contains("metal-HUD:"),best.map({ date>$0.1 }) ?? true else { continue }
                let head=String(decoding:(try? FileHandle(forReadingFrom:log).read(upToCount:16384)) ?? Data(),as:UTF8.self)
                let runner=head.split(separator:"\n").first { $0.hasPrefix("runner: build ") }.map { String($0.dropFirst("runner: build ".count).prefix { $0 != " " }) } ?? ""
                best=(Session(dir:nil,appID:app,game:Profiles.steamGames().first { $0.appID==app }?.name ?? "App \(app)",source:"CrossOver (NotProton)",
                              renderer:"",runtime:runner.isEmpty ? "" : "CrossOver \(runner)",upscaling:false,log:log,logOffset:0),date)
            }
        }
        return best?.0
    }
    private static func modified(_ url:URL)->Date {
        (try? url.resourceValues(forKeys:[.contentModificationDateKey]).contentModificationDate) ?? .distantPast
    }
    static func size(_ url:URL)->UInt64 { ((try? fm.attributesOfItem(atPath:url.path)[.size]) as? NSNumber)?.uint64Value ?? 0 }
    private static func tail(_ url:URL,bytes:UInt64)->String {
        guard let handle=try? FileHandle(forReadingFrom:url) else { return "" }
        defer { try? handle.close() }
        let end=(try? handle.seekToEnd()) ?? 0
        try? handle.seek(toOffset:end>bytes ? end-bytes : 0)
        return String(decoding:(try? handle.readToEnd()) ?? Data(),as:UTF8.self)
    }
    static func read(_ url:URL,from offset:UInt64)->String {
        guard let handle=try? FileHandle(forReadingFrom:url) else { return "" }
        defer { try? handle.close() }
        try? handle.seek(toOffset:offset)
        return String(decoding:(try? handle.readToEnd()) ?? Data(),as:UTF8.self)
    }

    // MARK: Clocks
    /// The host clock the renderers' present logs use (CACurrentMediaTime: mach_absolute_time in
    /// seconds, not counting sleep).
    static func hostSeconds()->Double {
        var base=mach_timebase_info_data_t();mach_timebase_info(&base)
        return Double(mach_absolute_time())*Double(base.numer)/Double(base.denom)/1e9
    }

    // MARK: Metal HUD log
    struct Frame:Equatable { var end:Double; var interval:Double; var gpu:Double }   // end in Unix seconds, others ms
    private static let hudLine=try! NSRegularExpression(pattern:#"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) (.+?)\[(\d+):[0-9a-fA-Fx]+\] metal-HUD: ([0-9.,]+)"#)
    private static let lineDate:DateFormatter={ let f=DateFormatter();f.locale=Locale(identifier:"en_US_POSIX");f.dateFormat="yyyy-MM-dd HH:mm:ss.SSS";f.timeZone = .current;return f }()
    /// Frames per process ("name[pid]"). A line is written after its frames, so the last frame
    /// of a line ends at the line's time and each earlier one an interval before the next.
    static func hudFrames(_ text:String)->[String:[Frame]] {
        var out:[String:[Frame]]=[:]
        for line in text.split(separator:"\n",omittingEmptySubsequences:true) where line.contains("metal-HUD:") {
            let s=String(line),ns=s as NSString
            guard let m=hudLine.firstMatch(in:s,range:NSRange(location:0,length:ns.length)),
                  let date=lineDate.date(from:ns.substring(with:m.range(at:1))) else { continue }
            let values=ns.substring(with:m.range(at:4)).split(separator:",").compactMap { Double($0) }
            guard values.count>=5 else { continue }
            let pairs=stride(from:3,to:values.count-1,by:2).map { (values[$0],values[$0+1]) }
            var end=date.timeIntervalSince1970,frames:[Frame]=[]
            for (interval,gpu) in pairs.reversed() { frames.append(Frame(end:end,interval:interval,gpu:gpu));end-=interval/1000 }
            out[ns.substring(with:m.range(at:2))+"[\(ns.substring(with:m.range(at:3)))]",default:[]].append(contentsOf:frames.reversed())
        }
        return out
    }

    // MARK: Present-to-display latency (renderer logs, host clock)
    /// Milliseconds from a frame's present (D3DMetal: it asked for its drawable; DXMT: it reached
    /// Metal's present call) to Metal's time it was on the display, for presents in [from, to].
    /// Frames never shown are left out. The process with most frames in the window wins.
    static func presentLatencies(_ dir:URL,from:Double,to:Double)->[Double] {
        let files=((try? fm.contentsOfDirectory(atPath:dir.path)) ?? []).sorted()
        var best:[Double]=[]
        // D3DMetal (wine-winemac-present-frames.patch): "# t0 <host s>", then ms after t0.
        for name in files where name.hasPrefix("d3dm-frames-") && name.hasSuffix(".csv") {
            var t0:Double?,col:[String:Int]=[:],values:[Double]=[]
            for line in read(dir.appendingPathComponent(name),from:0).split(separator:"\n") {
                if line.hasPrefix("# t0 ") { t0=Double(line.dropFirst(5).prefix { $0 != " " });continue }
                if line.hasPrefix("#") { continue }
                let cells=line.split(separator:",",omittingEmptySubsequences:false).map(String.init)
                if cells.first=="requested" { col=Dictionary(uniqueKeysWithValues:cells.enumerated().map { ($1,$0) });continue }
                guard let t0,let r=col["requested"],let p=col["presented"],cells.count>max(r,p),
                      let req=Double(cells[r]),let shown=Double(cells[p]),req>=0,shown>0 else { continue }
                let at=t0+req/1000
                if at>=from && at<=to { values.append(shown-req) }
            }
            if values.count>best.count { best=values }
        }
        // DXMT (tests/display_log/README.md): request and presented rows joined by pid+sequence,
        // across rotated segments (.2.csv oldest).
        var requests:[String:[String:Double]]=[:],presented:[String:[String:Double]]=[:]
        let display=files.filter { $0.hasPrefix("display-") && $0.hasSuffix(".csv") && !$0.contains("hitches") }
        for name in display.sorted(by:{ $0.filter { $0=="." }.count > $1.filter { $0=="." }.count }) {
            var col:[String:Int]=[:]
            for line in read(dir.appendingPathComponent(name),from:0).split(separator:"\n") {
                let cells=line.split(separator:",",omittingEmptySubsequences:false).map(String.init)
                if cells.first=="schema_version" { col=Dictionary(uniqueKeysWithValues:cells.enumerated().map { ($1,$0) });continue }
                guard let e=col["event"],let pid=col["native_pid"],let seq=col["sequence"],cells.count==col.count else { continue }
                let key=cells[seq]
                if cells[e]=="request",let i=col["request_host_s"],let t=Double(cells[i]) { requests[cells[pid],default:[:]][key]=t }
                if cells[e]=="presented",let i=col["presented_s"],let t=Double(cells[i]),t>0 { presented[cells[pid],default:[:]][key]=t }
            }
        }
        for (pid,reqs) in requests {
            let values=reqs.compactMap { seq,req in (req>=from && req<=to) ? presented[pid]?[seq].map { ($0-req)*1000 } : nil }.filter { $0>=0 }
            if values.count>best.count { best=values }
        }
        return best
    }

    // MARK: Summary (formulas in docs/BENCHMARKS.md)
    struct Summary:Codable,Equatable {
        var frames:Int
        var seconds:Double
        var avgFPS:Double
        var low1FPS:Double
        var low01FPS:Double
        var p50:Double,p95:Double,p99:Double,maxMs:Double
        var stutters:Int
        var gpuP50:Double,gpuP95:Double
        var gpuBound:Double            // share of frames whose GPU time is ≥ 90 % of their interval
        var latencyFrames:Int
        var latencyP50:Double?,latencyP95:Double?,latencyP99:Double?
    }
    static func percentile(_ sorted:[Double],_ p:Double)->Double {
        guard !sorted.isEmpty else { return 0 }
        let rank=p*Double(sorted.count-1),lo=Int(rank.rounded(.down)),hi=min(lo+1,sorted.count-1)
        return sorted[lo]+(sorted[hi]-sorted[lo])*(rank-Double(lo))
    }
    static func summarize(intervals:[Double],gpu:[Double],latency:[Double])->Summary {
        let sorted=intervals.sorted(),total=intervals.reduce(0,+)
        func low(_ share:Double)->Double {
            let worst=sorted.suffix(max(1,Int((Double(sorted.count)*share).rounded(.down))))
            let mean=worst.reduce(0,+)/Double(max(worst.count,1))
            return mean>0 ? 1000/mean : 0
        }
        let median=percentile(sorted,0.5),gpuSorted=gpu.sorted(),lat=latency.sorted()
        let bound=zip(intervals,gpu).filter { $0.0>0 && $0.1 >= 0.9*$0.0 }.count
        return Summary(frames:intervals.count,seconds:total/1000,avgFPS:total>0 ? Double(intervals.count)*1000/total : 0,
                       low1FPS:low(0.01),low01FPS:low(0.001),p50:median,p95:percentile(sorted,0.95),p99:percentile(sorted,0.99),
                       maxMs:sorted.last ?? 0,stutters:intervals.filter { $0>2*median }.count,
                       gpuP50:percentile(gpuSorted,0.5),gpuP95:percentile(gpuSorted,0.95),
                       gpuBound:intervals.isEmpty ? 0 : Double(bound)/Double(intervals.count),
                       latencyFrames:lat.count,latencyP50:lat.isEmpty ? nil : percentile(lat,0.5),
                       latencyP95:lat.isEmpty ? nil : percentile(lat,0.95),latencyP99:lat.isEmpty ? nil : percentile(lat,0.99))
    }

    // MARK: Runs (benchmarks/runs/<app id>/<date>.json)
    struct Run:Codable,Identifiable,Equatable {
        var id:String
        var appID:String
        var game:String
        var label:String
        var startedUnix:Double
        var source:String
        var renderer:String
        var runtime:String
        var upscaling:Bool
        var process:String
        var refreshHz:Int
        var summary:Summary
        var intervals:[Float]
        var gpu:[Float]
    }
    enum Failure:LocalizedError {
        case message(String)
        var errorDescription:String? { if case .message(let why)=self { return why };return nil }
    }
    /// A run from a session's log between two hotkey presses (Unix and host seconds).
    static func analyse(_ session:Session,startUnix:Double,stopUnix:Double,startHost:Double,stopHost:Double,refreshHz:Int)throws->Run {
        let byProcess=hudFrames(read(session.log,from:session.logOffset))
        let inWindow=byProcess.mapValues { $0.filter { $0.end>startUnix && $0.end<=stopUnix } }
        guard let (process,frames)=inWindow.max(by:{ $0.value.count<$1.value.count }).map({ ($0.key,$0.value) }),frames.count>=2 else {
            throw Failure.message(byProcess.isEmpty
                ? "No Metal HUD lines in \(session.log.lastPathComponent): the game wasn’t started with benchmark recording on."
                : "No frames between the two key presses.")
        }
        let latency=session.dir.map { presentLatencies($0,from:startHost,to:stopHost) } ?? []
        let intervals=frames.map(\.interval),gpu=frames.map(\.gpu)
        let date=Date(timeIntervalSince1970:startUnix)
        let id=ISO8601DateFormatter.string(from:date,timeZone:.current,formatOptions:[.withFullDate,.withTime,.withDashSeparatorInDate]).replacingOccurrences(of:":",with:"")
        return Run(id:"\(session.appID)-\(id)",appID:session.appID,game:session.game,label:"",startedUnix:startUnix,source:session.source,
                   renderer:session.renderer,runtime:session.runtime,upscaling:session.upscaling,process:process,refreshHz:refreshHz,
                   summary:summarize(intervals:intervals,gpu:gpu,latency:latency),intervals:intervals.map(Float.init),gpu:gpu.map(Float.init))
    }
    static func file(_ run:Run)->URL { runsFolder.appendingPathComponent(run.appID).appendingPathComponent(run.id+".json") }
    static func save(_ run:Run) throws {
        let encoder=JSONEncoder();encoder.outputFormatting=[.sortedKeys]
        try fm.createDirectory(at:file(run).deletingLastPathComponent(),withIntermediateDirectories:true)
        try encoder.encode(run).write(to:file(run),options:.atomic)
    }
    static func runs()->[Run] {
        var out:[Run]=[]
        for app in (try? fm.contentsOfDirectory(atPath:runsFolder.path)) ?? [] {
            let dir=runsFolder.appendingPathComponent(app)
            for name in (try? fm.contentsOfDirectory(atPath:dir.path)) ?? [] where name.hasSuffix(".json") {
                if let data=try? Data(contentsOf:dir.appendingPathComponent(name)),let run=try? JSONDecoder().decode(Run.self,from:data) { out.append(run) }
            }
        }
        return out.sorted { $0.startedUnix>$1.startedUnix }
    }
    static func delete(_ run:Run) { try? fm.removeItem(at:file(run)) }

    // MARK: Text for the app
    static func settingsLine(_ run:Run)->String {
        var parts=[run.source]
        if !run.renderer.isEmpty { parts.append(run.renderer=="dxmt" ? "DXMT" : "D3DMetal") }
        if run.upscaling { parts.append("MetalFX upscaling") }
        if !run.runtime.isEmpty { parts.append(run.runtime) }
        if run.refreshHz>0 { parts.append("\(run.refreshHz) Hz display") }
        return parts.joined(separator:" · ")
    }
}
