import Foundation

/// Profiles (docs/PROFILES.md): renderer settings for every DX11 game (Global DX11), every DX12
/// game (Global DX12) or one game (a game profile, which wins over the global one). Neither the
/// mouse nor the key combos are part of a profile: the mouse is one setting for every game, and
/// key combos are Global or per game, whatever the Direct3D (KeyOverrides.swift).
///
/// Each profile is a folder in SteamDriver/profiles/<id> (ids "global-dx11", "global-dx12",
/// "app-<Steam app id>"):
/// - profile.json: its name, DX11 or DX12, and the game's app id;
/// - dxmt.conf: SteamDriver DXMT's settings (DX11 profiles only; D3DMetal has none to set yet).
///   Copied from the app's bundled ones (Resources/NotProton/profiles): CS2's and Deadlock's own,
///   dx11.conf for everything else. Lines outside an [exe] section apply to every program
///   (DXMT's util/config, ctx.active starts true), which is what a profile by app id needs.
///
/// How they reach games: NotProton rows (NotProtonExtras.profileRows), Global DX11 without
/// `apps`, a game's with `apps = <id>`.
enum Profiles {
    enum Kind:String,Codable,CaseIterable,Identifiable {
        case dx11, dx12
        var id:String { rawValue }
        var title:String { self == .dx11 ? "DX11" : "DX12" }
    }
    struct Profile:Identifiable,Hashable,Codable {
        let id:String
        let name:String
        let kind:Kind
        let appID:String?
        var isGlobal:Bool { appID == nil }
    }
    /// A Steam game a game profile can be made for.
    struct Game:Identifiable,Hashable {
        let appID:String
        let name:String
        var id:String { appID }
        /// The Direct3D the SteamDriver tool saw it load at its last launch (detected).
        var detected:Kind? { Profiles.detected(appID) }
    }
    /// DX11/DX12 detection: the SteamDriver tool's run script reads which Direct3D a launch loaded
    /// from Wine's own DLL-load log and writes "direct3d=dx11|dx12" as line 4 of games/<app id>
    /// (environments/finals/compat-tool/run). nil until the game has run through the tool.
    static func detected(_ appID:String)->Kind? {
        guard valid(appID),let text=try? String(contentsOf:launchedFolder.appendingPathComponent(appID),encoding:.utf8),
              let line=text.split(whereSeparator:\.isNewline).first(where: { $0.hasPrefix("direct3d=") }) else { return nil }
        return Kind(rawValue:String(line.dropFirst("direct3d=".count)))
    }
    /// The kind a new game profile starts as: detected, else known, else DX11.
    static func suggestedKind(_ appID:String)->Kind { detected(appID) ?? knownKinds[appID] ?? .dx11 }

    private static let fm=FileManager.default
    static var folder:URL { NotProton.ours.appendingPathComponent("profiles") }
    /// Every game launched through a game tool, recorded by its run script (name, tool, time).
    static var launchedFolder:URL { NotProton.ours.appendingPathComponent("games") }
    static func globalID(_ kind:Kind)->String { "global-"+kind.rawValue }
    static func gameID(_ appID:String)->String { "app-"+appID }
    static func dir(_ id:String)->URL { folder.appendingPathComponent(id) }
    static func dxmtFile(_ id:String)->URL { dir(id).appendingPathComponent("dxmt.conf") }

    /// Games macSteam has its own renderer settings for (bundled <name>.conf), and the Direct3D
    /// games known to use, as the default when making a profile.
    static let tuned=["730":"cs2","1422450":"deadlock"]
    static let knownKinds:[String:Kind]=["730":.dx11,"1422450":.dx11,"2073850":.dx12]
    static let deadlockAppID="1422450"

    // MARK: Reading
    static func list()->[Profile] {
        let names=(try? fm.contentsOfDirectory(atPath:folder.path)) ?? []
        let all=names.compactMap(profile)
        let globals=Kind.allCases.compactMap { kind in all.first { $0.id==globalID(kind) } }
        let games=all.filter { !$0.isGlobal }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        return globals+games
    }
    static func profile(_ id:String)->Profile? {
        guard let data=try? Data(contentsOf:dir(id).appendingPathComponent("profile.json")),
              let p=try? JSONDecoder().decode(Profile.self,from:data),p.id==id else { return nil }
        // Only the ids macSteam makes, so a stray folder never becomes a path in a row.
        if p.isGlobal { return id==globalID(p.kind) ? p : nil }
        guard let app=p.appID,valid(app),id==gameID(app) else { return nil }
        return p
    }
    static func exists(_ id:String)->Bool { profile(id) != nil }
    /// A Steam app id as NotProton's `apps` takes it: digits, no leading zero, 32 bits.
    static func valid(_ appID:String)->Bool {
        !appID.isEmpty && appID.count <= 10 && appID.first != "0" && appID.allSatisfy { $0.isASCII && $0.isNumber }
            && (UInt64(appID) ?? .max) <= 4_294_967_295
    }

    // MARK: Adding and removing
    static func addGlobal(_ kind:Kind,resources:URL) throws {
        try make(Profile(id:globalID(kind),name:"Global \(kind.title)",kind:kind,appID:nil),resources:resources)
    }
    static func addGame(appID:String,name:String,kind:Kind,resources:URL) throws {
        guard valid(appID) else { throw NotProton.failure("\u{201C}\(appID)\u{201D} isn’t a Steam app id.") }
        let clean=String(name.filter { !$0.isNewline && $0 != "\t" }.prefix(80)).trimmingCharacters(in:.whitespaces)
        try make(Profile(id:gameID(appID),name:clean.isEmpty ? "App \(appID)" : clean,kind:kind,appID:appID),resources:resources)
    }
    private static func make(_ p:Profile,resources:URL) throws {
        try fm.createDirectory(at:dir(p.id),withIntermediateDirectories:true)
        try writeJSON(p)
        // Renderer settings once, from the app; a DX12 profile has none to set yet.
        if p.kind == .dx11 {
            let name=p.appID.flatMap { tuned[$0] } ?? "dx11"
            if fm.fileExists(atPath:dxmtFile(p.id).path) { try fm.removeItem(at:dxmtFile(p.id)) }
            try fm.copyItem(at:resources.appendingPathComponent("NotProton/profiles/\(name).conf"),to:dxmtFile(p.id))
        } else { try? fm.removeItem(at:dxmtFile(p.id)) }
        if p.appID == deadlockAppID { NotProton.keepDeadlockOnDirectX11() }
    }
    private static func writeJSON(_ p:Profile) throws {
        let encoder=JSONEncoder();encoder.outputFormatting=[.prettyPrinted,.sortedKeys]
        try encoder.encode(p).write(to:dir(p.id).appendingPathComponent("profile.json"),options:.atomic)
    }
    static func remove(_ id:String) throws {
        guard exists(id) else { return }
        try fm.removeItem(at:dir(id))
    }

    // MARK: Renderer settings (dxmt.conf of a DX11 profile)
    /// The SteamDriver DXMT settings a profile holds, as macSteam shows them. Read from and
    /// written to dxmt.conf in place: only these keys' lines change, comments and an [exe]
    /// section (CS2's, Deadlock's) stay. DXMT reads the file when the game starts, so a change
    /// applies from the next launch and Steam's rows don't change.
    struct Settings:Equatable {
        /// dxgi.maxFrameLatency: frames the game may queue ahead of the GPU, at most (DXMT's
        /// default 16 leaves it to the game, which asks for 3).
        var frameLatency=16
        /// dxgi.presentPacing (mode 3, target 1000 µs): after the frame-latency wait, the game
        /// thread sleeps through most of the slack, so the next frame reads input later.
        var lateInput=false
        /// d3d11.preferredMaxFrameRate: frames per second at most; 0 = no cap.
        var frameCap=0
        /// d3d11.shaderCompilerThreads: 0 = DXMT decides.
        var shaderThreads=0
        /// d3d11.shaderCompilerNormalPriority: compile at normal priority instead of time-critical.
        var shaderNormalPriority=false
        /// dxgi.sharpPresentation: sharper filtering when the picture is scaled to the window.
        var sharpScaling=false
        /// d3d11.releaseShaderIR: free each shader's intermediate code once compiled (DXMT's default).
        var releaseShaderIR=true
    }
    private static let keys=(latency:"dxgi.maxFrameLatency",pacing:"dxgi.presentPacing",mode:"dxgi.presentPacingMode",
                             target:"dxgi.presentPacingTargetUs",cap:"d3d11.preferredMaxFrameRate",threads:"d3d11.shaderCompilerThreads",
                             priority:"d3d11.shaderCompilerNormalPriority",sharp:"dxgi.sharpPresentation",ir:"d3d11.releaseShaderIR")
    /// key = value lines (the first of each key), whatever section they are in.
    private static func values(_ text:String)->[String:String] {
        var out:[String:String]=[:]
        for raw in text.split(separator:"\n",omittingEmptySubsequences:false) {
            let line=raw.trimmingCharacters(in:.whitespaces)
            guard !line.hasPrefix("#"),!line.hasPrefix("["),let eq=line.firstIndex(of:"=") else { continue }
            let key=line[..<eq].trimmingCharacters(in:.whitespaces)
            if out[key]==nil { out[key]=line[line.index(after:eq)...].trimmingCharacters(in:.whitespaces) }
        }
        return out
    }
    static func settings(_ id:String)->Settings? {
        guard let text=try? String(contentsOf:dxmtFile(id),encoding:.utf8) else { return nil }
        let v=values(text)
        func bool(_ k:String,_ d:Bool)->Bool { v[k].map { $0.lowercased()=="true" || $0=="1" } ?? d }
        func int(_ k:String,_ d:Int)->Int { v[k].flatMap { Int($0) } ?? d }
        return Settings(frameLatency:min(max(int(keys.latency,16),1),16),
                        lateInput:bool(keys.pacing,false) && int(keys.mode,1)==3,
                        frameCap:max(int(keys.cap,0),0),shaderThreads:min(max(int(keys.threads,0),0),64),
                        shaderNormalPriority:bool(keys.priority,false),sharpScaling:bool(keys.sharp,false),
                        releaseShaderIR:bool(keys.ir,true))
    }
    static func save(_ id:String,_ s:Settings) throws {
        var lines=((try? String(contentsOf:dxmtFile(id),encoding:.utf8)) ?? "").components(separatedBy:"\n")
        if lines.last == "" { lines.removeLast() }
        func set(_ key:String,_ value:String) {
            let at=lines.firstIndex { line in
                let t=line.trimmingCharacters(in:.whitespaces)
                guard !t.hasPrefix("#"),let eq=t.firstIndex(of:"=") else { return false }
                return t[..<eq].trimmingCharacters(in:.whitespaces)==key
            }
            if let at { lines[at]="\(key) = \(value)" } else { lines.append("\(key) = \(value)") }
        }
        func b(_ v:Bool)->String { v ? "True" : "False" }
        set(keys.latency,"\(s.frameLatency)")
        set(keys.pacing,b(s.lateInput))
        if s.lateInput { set(keys.mode,"3");set(keys.target,"1000") }
        set(keys.cap,"\(s.frameCap)")
        set(keys.threads,"\(s.shaderThreads)")
        set(keys.priority,b(s.shaderNormalPriority))
        set(keys.sharp,b(s.sharpScaling))
        set(keys.ir,b(s.releaseShaderIR))
        try (lines.joined(separator:"\n")+"\n").write(to:dxmtFile(id),atomically:true,encoding:.utf8)
    }
    /// Back to the settings macSteam brings (CS2's and Deadlock's own, dx11.conf for others).
    static func resetSettings(_ id:String,resources:URL) throws {
        guard let p=profile(id),p.kind == .dx11 else { return }
        try make(p,resources:resources)
    }
    /// One line for the Profiles panel.
    static func summary(_ s:Settings)->String {
        [s.frameLatency>=16 ? "frame latency: game’s" : "frame latency \(s.frameLatency)",
         s.lateInput ? "late input sampling" : nil,
         s.frameCap==0 ? "no frame cap" : "capped at \(s.frameCap) fps",
         s.shaderThreads==0 ? "shader threads: automatic" : "\(s.shaderThreads) shader threads",
         s.sharpScaling ? "sharp scaling" : nil].compactMap { $0 }.joined(separator:" · ")
    }

    // MARK: Games to choose from
    /// Installed Steam games (appmanifests in every library) and games launched through a game
    /// tool, by name; Steam's own tools (Proton, runtimes, redistributables) left out.
    static func steamGames()->[Game] {
        var found:[String:String]=[:]
        let nameRE=try? NSRegularExpression(pattern:#""name"\s+"([^"]+)""#)
        for library in NotProton.steamLibraries() {
            let apps=library.appendingPathComponent("steamapps")
            for file in (try? fm.contentsOfDirectory(atPath:apps.path)) ?? [] where file.hasPrefix("appmanifest_") && file.hasSuffix(".acf") {
                let appID=String(file.dropFirst("appmanifest_".count).dropLast(4))
                guard valid(appID),appID != "228980",
                      let text=try? String(contentsOf:apps.appendingPathComponent(file),encoding:.utf8),
                      let m=nameRE?.firstMatch(in:text,range:NSRange(text.startIndex...,in:text)),
                      let r=Range(m.range(at:1),in:text) else { continue }
                let name=String(text[r])
                if name.hasPrefix("Proton") || name.hasPrefix("Steam Linux Runtime") || name.hasPrefix("Steamworks") { continue }
                found[appID]=name
            }
        }
        for file in (try? fm.contentsOfDirectory(atPath:launchedFolder.path)) ?? [] where valid(file) && found[file]==nil {
            let text=(try? String(contentsOf:launchedFolder.appendingPathComponent(file),encoding:.utf8)) ?? ""
            found[file]=text.split(whereSeparator:\.isNewline).first.map(String.init) ?? "App \(file)"
        }
        return found.map { Game(appID:$0.key,name:$0.value) }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    // MARK: Before profiles (Migration.run calls this after moving the folder)
    /// The flat renderer files before profiles, profiles/cs2.conf and deadlock.conf, become the
    /// CS2 and Deadlock game profiles; key-overrides/games/ (games started with a game tool)
    /// becomes games/. Builds of 2026-11-24 kept key combos in the profiles (keys.txt); those go
    /// back to key-overrides/, Global DX11's and DX12's into Global. Nothing already in a new
    /// place is overwritten.
    static func migrate(_ notes:inout [String]) {
        for (appID,name) in tuned {
            let old=folder.appendingPathComponent(name+".conf")
            guard isFile(old) else { continue }
            let id=gameID(appID)
            ensure(Profile(id:id,name:appID=="730" ? "Counter-Strike 2" : "Deadlock",kind:.dx11,appID:appID))
            if !fm.fileExists(atPath:dxmtFile(id).path),(try? fm.moveItem(at:old,to:dxmtFile(id))) != nil {
                notes.append("made the \(name) renderer profile the game profile \(id)")
            } else { try? fm.removeItem(at:old) }
        }
        let oldGames=KeyOverrides.folder.appendingPathComponent("games")
        if fm.fileExists(atPath:oldGames.path),!fm.fileExists(atPath:launchedFolder.path),(try? fm.moveItem(at:oldGames,to:launchedFolder)) != nil {
            notes.append("moved the list of games started with a game tool to \(launchedFolder.path)")
        }
        for name in (try? fm.contentsOfDirectory(atPath:folder.path)) ?? [] {
            let keys=dir(name).appendingPathComponent("keys.txt")
            guard fm.fileExists(atPath:keys.path) else { continue }
            let list=name.hasPrefix("global-") ? KeyOverrides.globalID : name
            if KeyOverrides.valid(list),let moved=try? String(contentsOf:keys,encoding:.utf8) {
                let combos=KeyOverrides.load(list)+moved.split(whereSeparator:\.isNewline).map { $0.trimmingCharacters(in:.whitespaces).lowercased() }
                    .filter { !$0.isEmpty && !$0.hasPrefix("#") }
                var seen=Set<String>()
                if (try? KeyOverrides.save(list,combos.filter { seen.insert($0).inserted })) != nil {
                    try? fm.removeItem(at:keys);notes.append("moved the key combos of \(name) to the list \(list)")
                }
            }
        }
    }
    private static func isFile(_ url:URL)->Bool {
        var dir:ObjCBool=false
        return fm.fileExists(atPath:url.path,isDirectory:&dir) && !dir.boolValue
    }
    private static func ensure(_ p:Profile) {
        try? fm.createDirectory(at:dir(p.id),withIntermediateDirectories:true)
        if profile(p.id)==nil { try? writeJSON(p) }
    }
}
