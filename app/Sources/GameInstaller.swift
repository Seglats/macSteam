import Foundation

/// "Install a Windows game" (docs/GOG.md): a dropped installer becomes a non-Steam game in
/// macOS Steam that runs on the SteamDriver tool and looks like the game's Steam page.
///
/// 1. The installer (and the .bin parts beside it) is copied to SteamDriver/installers/<id>/
///    (an APFS clone, so it costs no space while the original stays), with the game's name in
///    "name" beside it.
/// 2. With Steam closed (it rewrites these files when it quits): a shortcut in
///    userdata/<account>/config/shortcuts.vdf pointing at the copy, SteamDriver as its
///    compatibility tool in config/config.vdf (CompatToolMapping), and the game's own artwork
///    from Steam's store in userdata/<account>/config/grid/.
/// 3. Steam starts again and runs the shortcut: the SteamDriver tool's run script installs the
///    game into the shortcut's prefix and, from the next launch on, starts the game instead.
enum GameInstaller {
    static let toolName="notproton-steamdriver"
    private static let fm=FileManager.default
    static var steam:URL { NotProton.home.appendingPathComponent("Library/Application Support/Steam") }
    static var installers:URL { NotProton.ours.appendingPathComponent("installers") }

    enum Failure:LocalizedError {
        case message(String)
        var errorDescription:String? { if case .message(let text)=self { return text };return nil }
    }

    // MARK: The dropped files
    /// The installer plus the parts GOG ships beside it (setup_x.exe with setup_x-1.bin, …).
    static func files(for installer:URL)->[URL] {
        let stem=installer.deletingPathExtension().lastPathComponent.lowercased()
        let siblings=(try? fm.contentsOfDirectory(at:installer.deletingLastPathComponent(),includingPropertiesForKeys:nil)) ?? []
        return [installer]+siblings.filter { $0.pathExtension.lowercased()=="bin" && $0.lastPathComponent.lowercased().hasPrefix(stem) }.sorted { $0.lastPathComponent<$1.lastPathComponent }
    }
    /// A name to start from: "setup_hollow_knight_1.5.12620_(64bit)_(89718).exe" → "Hollow Knight".
    static func guessName(_ installer:URL)->String {
        var stem=installer.deletingPathExtension().lastPathComponent
        for prefix in ["setup_","setup-","Setup_","Setup-"] where stem.hasPrefix(prefix) { stem.removeFirst(prefix.count) }
        stem=stem.replacingOccurrences(of:#"\([^)]*\)"#,with:" ",options:.regularExpression)
        var words:[String]=[]
        // Up to the version (1.5.12620, v4.04) or a word like "setup"; a plain number stays
        // (The Witcher 3).
        for word in stem.split(whereSeparator:{ "_- ".contains($0) }) {
            let w=word.lowercased()
            let version=(w.first?.isNumber==true && w.contains(".")) || (w.hasPrefix("v") && w.dropFirst().first?.isNumber==true)
            if version || ["setup","installer","install","x64","win64","win","gog"].contains(w) { break }
            words.append(String(word))
        }
        let name=words.map { $0.prefix(1).uppercased()+$0.dropFirst() }.joined(separator:" ")
        return name.isEmpty ? installer.deletingPathExtension().lastPathComponent : name
    }

    // MARK: Steam's store: the game's page and artwork
    struct StoreMatch:Identifiable,Hashable { let id:Int; let name:String }
    static func search(_ name:String) async->[StoreMatch] {
        var url=URLComponents(string:"https://store.steampowered.com/api/storesearch/")!
        url.queryItems=[.init(name:"term",value:name),.init(name:"cc",value:"us"),.init(name:"l",value:"en")]
        guard let (data,_)=try? await URLSession.shared.data(from:url.url!),
              let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any],
              let items=json["items"] as? [[String:Any]] else { return [] }
        let matches=items.compactMap { item in (item["id"] as? Int).map { StoreMatch(id:$0,name:item["name"] as? String ?? "App \($0)") } }
        // The exact name first: a search for a game also finds its soundtrack and sequels.
        return matches.sorted { a,b in (a.name.caseInsensitiveCompare(name) == .orderedSame ? 0 : 1) < (b.name.caseInsensitiveCompare(name) == .orderedSame ? 0 : 1) }
    }
    /// The app id in a Steam store link (store.steampowered.com/app/367520/Hollow_Knight/,
    /// steamcommunity.com/app/…, steam://store/…) or a plain id.
    static func appID(fromLink text:String)->Int? {
        let text=text.trimmingCharacters(in:.whitespacesAndNewlines)
        if let id=Int(text),id>0 { return id }
        guard let match=text.range(of:#"(?:/app/|steam://(?:store|run|rungameid)/)(\d+)"#,options:.regularExpression) else { return nil }
        return Int(text[match].filter(\.isNumber))
    }
    /// The store's name for an app id (appdetails), so a pasted link shows which game it is.
    static func storeApp(_ id:Int) async->StoreMatch? {
        guard let url=URL(string:"https://store.steampowered.com/api/appdetails?appids=\(id)&filters=basic"),
              let (data,_)=try? await URLSession.shared.data(from:url),
              let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any],
              let entry=json[String(id)] as? [String:Any],entry["success"] as? Bool==true,
              let details=entry["data"] as? [String:Any] else { return nil }
        return StoreMatch(id:id,name:details["name"] as? String ?? "App \(id)")
    }
    static func artURL(_ app:Int,_ file:String)->URL { URL(string:"https://cdn.cloudflare.steamstatic.com/steam/apps/\(app)/\(file)")! }
    /// Steam's library artwork names for a shortcut: <id>p.jpg cover, <id>.jpg wide capsule,
    /// <id>_hero.jpg banner, <id>_logo.png logo over it.
    static let art:[(store:[String],grid:String)]=[
        (["library_600x900_2x.jpg","library_600x900.jpg"],"p.jpg"),(["header.jpg"],".jpg"),
        (["library_hero.jpg"],"_hero.jpg"),(["logo.png"],"_logo.png"),
    ]
    static func downloadArt(_ app:Int) async->[String:Data] {
        var out:[String:Data]=[:]
        for (store,grid) in art {
            for file in store {
                if let (data,response)=try? await URLSession.shared.data(from:artURL(app,file)),
                   (response as? HTTPURLResponse)?.statusCode==200,!data.isEmpty { out[grid]=data;break }
            }
        }
        // The small icon Steam's library list shows: its file name is a hash the store pages
        // don't give, but Steam's public community API does (ICommunityService/GetApps).
        if let hash=await iconHash(app),
           let (data,response)=try? await URLSession.shared.data(from:URL(string:"https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/\(app)/\(hash).jpg")!),
           (response as? HTTPURLResponse)?.statusCode==200,!data.isEmpty { out[iconSuffix]=data }
        return out
    }
    /// Saved as grid/<id>_icon.jpg; the shortcut's "icon" field points at it.
    static let iconSuffix="_icon.jpg"
    static func iconHash(_ app:Int) async->String? {
        guard let url=URL(string:"https://api.steampowered.com/ICommunityService/GetApps/v1/?appids%5B0%5D=\(app)"),
              let (data,_)=try? await URLSession.shared.data(from:url),
              let json=try? JSONSerialization.jsonObject(with:data) as? [String:Any],
              let apps=(json["response"] as? [String:Any])?["apps"] as? [[String:Any]],
              let hash=apps.first?["icon"] as? String,hash.allSatisfy(\.isHexDigit),!hash.isEmpty else { return nil }
        return hash
    }

    // MARK: Steam's files
    /// The signed-in account's userdata folder: the newest login in loginusers.vdf.
    static func userFolder()->URL? {
        let users=steam.appendingPathComponent("userdata")
        if let vdf=try? String(contentsOf:steam.appendingPathComponent("config/loginusers.vdf"),encoding:.utf8) {
            let tree=TextVDF.parse(vdf)
            let logins=(tree["users"]?.children ?? []).compactMap { id,node -> (UInt64,Int)? in
                guard let steamID=UInt64(id) else { return nil }
                return (steamID,Int(node["timestamp"]?.value ?? "") ?? 0)
            }
            if let newest=logins.max(by:{ $0.1<$1.1 }),newest.0>76561197960265728 {
                let folder=users.appendingPathComponent(String(newest.0-76561197960265728))
                if fm.fileExists(atPath:folder.path) { return folder }
            }
        }
        let folders=((try? fm.contentsOfDirectory(at:users,includingPropertiesForKeys:nil)) ?? []).filter { UInt64($0.lastPathComponent).map { $0>0 } ?? false }
        return folders.count==1 ? folders[0] : nil
    }
    /// Steam's own id for a new shortcut: CRC-32 of the quoted exe and the name, top bit set.
    static func shortcutID(exe:String,name:String)->UInt32 { crc32(Array((exe+name).utf8)) | 0x8000_0000 }
    private static func crc32(_ bytes:[UInt8])->UInt32 {
        var crc:UInt32=0xFFFF_FFFF
        for b in bytes {
            crc^=UInt32(b)
            for _ in 0..<8 { crc=(crc>>1)^(0xEDB8_8320 & (0 &- (crc&1))) }
        }
        return ~crc
    }

    /// Adds the shortcut (or updates the one pointing at the same file), its compatibility tool
    /// and its artwork. Steam must not be running. Returns the shortcut's id.
    @discardableResult
    static func addToSteam(name:String,exe:URL,art:[String:Data]) throws->UInt32 {
        guard let user=userFolder() else { throw Failure.message("Steam’s user folder wasn’t found. Sign in to Steam once, then try again.") }
        let config=user.appendingPathComponent("config")
        try fm.createDirectory(at:config.appendingPathComponent("grid"),withIntermediateDirectories:true)
        let file=config.appendingPathComponent("shortcuts.vdf")
        var root:BinaryVDF.Map=[("shortcuts",.map([]))]
        if let data=try? Data(contentsOf:file) {
            guard let parsed=BinaryVDF.parse(data) else { throw Failure.message("Steam’s shortcuts.vdf couldn’t be read, so nothing was changed.") }
            root=parsed
            backup(file)
        }
        guard case .map(var shortcuts)=root.first(where:{ $0.0=="shortcuts" })?.1 ?? .map([]) else { throw Failure.message("Steam’s shortcuts.vdf has an unexpected layout.") }
        let quoted="\""+exe.path+"\""
        var id=shortcutID(exe:quoted,name:name)
        let start="\""+exe.deletingLastPathComponent().path+"/\""
        // The same installer again, or a newer one in the same installers/<Name>/ folder (an
        // update): that shortcut is updated, keeping its id and so its prefix and the game.
        let sameFolder="\""+exe.deletingLastPathComponent().path+"/"
        if let i=shortcuts.firstIndex(where:{ entry in
            guard case .map(let fields)=entry.1,case .string(let old)?=fields.first(where:{ $0.0.lowercased()=="exe" })?.1 else { return false }
            return old==quoted || (exe.path.hasPrefix(installers.path+"/") && old.hasPrefix(sameFolder))
        }), case .map(var fields)=shortcuts[i].1 {
            if case .int(let old)?=fields.first(where:{ $0.0.lowercased()=="appid" })?.1 { id=old }
            set(&fields,"AppName",.string(name))
            set(&fields,"Exe",.string(quoted))
            set(&fields,"StartDir",.string(start))
            if art[iconSuffix] != nil { set(&fields,"icon",.string(config.appendingPathComponent("grid/\(id)\(iconSuffix)").path)) }
            shortcuts[i].1 = .map(fields)
        } else {
            let used=Set(shortcuts.compactMap { entry -> UInt32? in
                guard case .map(let f)=entry.1,case .int(let v)?=f.first(where:{ $0.0.lowercased()=="appid" })?.1 else { return nil };return v
            })
            while used.contains(id) { id=(id &+ 1) | 0x8000_0000 }
            let fields:BinaryVDF.Map=[
                ("appid",.int(id)),("AppName",.string(name)),("Exe",.string(quoted)),("StartDir",.string(start)),
                ("icon",.string(art[iconSuffix] != nil ? config.appendingPathComponent("grid/\(id)\(iconSuffix)").path : "")),("ShortcutPath",.string("")),("LaunchOptions",.string("")),("IsHidden",.int(0)),
                ("AllowDesktopConfig",.int(1)),("AllowOverlay",.int(1)),("OpenVR",.int(0)),("Devkit",.int(0)),
                ("DevkitGameID",.string("")),("DevkitOverrideAppID",.int(0)),("LastPlayTime",.int(0)),
                ("FlatpakAppID",.string("")),("tags",.map([])),
            ]
            shortcuts.append((String(shortcuts.count),.map(fields)))
        }
        root=root.map { $0.0=="shortcuts" ? ($0.0,.map(shortcuts)) : $0 }
        try BinaryVDF.encode(root).write(to:file,options:.atomic)
        try setCompatTool(id)
        for (suffix,data) in art { try data.write(to:config.appendingPathComponent("grid/\(id)\(suffix)"),options:.atomic) }
        return id
    }
    private static func set(_ fields:inout BinaryVDF.Map,_ key:String,_ value:BinaryVDF.Node) {
        if let i=fields.firstIndex(where:{ $0.0.lowercased()==key.lowercased() }) { fields[i].1=value } else { fields.append((key,value)) }
    }
    /// SteamDriver as the shortcut's compatibility tool (what Properties › Compatibility sets).
    static func setCompatTool(_ id:UInt32) throws {
        let file=steam.appendingPathComponent("config/config.vdf")
        guard var text=try? String(contentsOf:file,encoding:.utf8) else { throw Failure.message("Steam’s config.vdf wasn’t found; start Steam once first.") }
        let entry={ (indent:String) in
            "\(indent)\"\(id)\"\n\(indent){\n\(indent)\t\"name\"\t\t\"\(toolName)\"\n\(indent)\t\"config\"\t\t\"\"\n\(indent)\t\"priority\"\t\t\"250\"\n\(indent)}\n"
        }
        if let block=text.range(of:#"\n([ \t]*)"CompatToolMapping"[ \t]*\n[ \t]*\{[ \t]*\n"#,options:.regularExpression) {
            let indent=String(text[block].dropFirst().prefix { $0==" " || $0=="\t" })+"\t"
            if !blockContains(text,after:block.upperBound,id:id) {
                text.insert(contentsOf:entry(indent),at:block.upperBound)
            }
        } else if let steamBlock=text.range(of:#"\n([ \t]*)"Steam"[ \t]*\n[ \t]*\{[ \t]*\n"#,options:.regularExpression) {
            let indent=String(text[steamBlock].dropFirst().prefix { $0==" " || $0=="\t" })+"\t"
            text.insert(contentsOf:"\(indent)\"CompatToolMapping\"\n\(indent){\n"+entry(indent+"\t")+"\(indent)}\n",at:steamBlock.upperBound)
        } else { throw Failure.message("Steam’s config.vdf has no Steam section; nothing was changed.") }
        backup(file)
        try text.write(to:file,atomically:true,encoding:.utf8)
    }
    /// Whether the CompatToolMapping block starting at `start` already has an entry for `id`.
    private static func blockContains(_ text:String,after start:String.Index,id:UInt32)->Bool {
        var depth=1,i=start
        let key="\"\(id)\""
        while i<text.endIndex && depth>0 {
            if text[i]=="{" { depth+=1 } else if text[i]=="}" { depth-=1 }
            else if depth==1 && text[i...].hasPrefix(key) { return true }
            i=text.index(after:i)
        }
        return false
    }
    /// One copy of a Steam file as it was before macSteam first changed it.
    private static func backup(_ file:URL) {
        let copy=file.appendingPathExtension("steamdriver-backup")
        if !fm.fileExists(atPath:copy.path) { try? fm.copyItem(at:file,to:copy) }
    }

    // MARK: The whole step, off the main thread
    /// Copies the installer files, adds the game to Steam (restarting it) and starts the
    /// shortcut. `progress` says what is happening.
    /// `location`: a folder to install the game into instead of Steam's (docs/GOG.md, "Install
    /// to"). The game's prefix then lives in <location>/<Name>-<id>, and Steam's
    /// compatdata/<id> is a link to it, so Steam and the run script find it as usual.
    static func install(name:String,installer:URL,store:StoreMatch?,location:URL?=nil,progress:@escaping @Sendable (String)->Void) async throws {
        let name=name.trimmingCharacters(in:.whitespaces)
        guard !name.isEmpty else { throw Failure.message("Give the game a name.") }
        // The installer's folder is named after the shortcut's id, which comes from the path:
        // the path is made from the name first, so the id is stable across tries.
        let slug=name.components(separatedBy:CharacterSet.alphanumerics.inverted).filter { !$0.isEmpty }.joined(separator:"-")
        let folder=installers.appendingPathComponent(slug.isEmpty ? "game" : slug)
        if let location { try checkLocation(location) }
        progress("Copying the installer…")
        try fm.createDirectory(at:folder,withIntermediateDirectories:true)
        for file in files(for:installer) {
            let target=folder.appendingPathComponent(file.lastPathComponent)
            if !fm.fileExists(atPath:target.path) { try fm.copyItem(at:file,to:target) }   // APFS clone on the same volume
        }
        try (name+"\n").write(to:folder.appendingPathComponent("name"),atomically:true,encoding:.utf8)
        var artwork:[String:Data]=[:]
        if let store { progress("Getting \(store.name)’s artwork from Steam…");artwork=await downloadArt(store.id) }
        progress("Closing Steam to add the game…")
        if NotProton.steamRunning {
            _ = NotProton.tool("/usr/bin/open",["-g","steam://exit"])
            for _ in 0..<60 where NotProton.steamRunning { try await Task.sleep(for:.milliseconds(500)) }
            guard !NotProton.steamRunning else { throw Failure.message("Steam didn’t quit, perhaps because a game or download is running. Quit Steam, then try again; the installer is already copied.") }
            try await Task.sleep(for:.seconds(1))
        }
        let id=try addToSteam(name:name,exe:folder.appendingPathComponent(installer.lastPathComponent),art:artwork)
        try? "\(id)\n".write(to:folder.appendingPathComponent("shortcut"),atomically:true,encoding:.utf8)   // InstalledGames
        if let location { try placePrefix(id:id,slug:folder.lastPathComponent,in:location) }
        progress("Starting Steam…")
        let log=steam.appendingPathComponent("steamapps/compatdata/\(id)/steamdriver-run.log")
        let start=fm.fileExists(atPath:log.path) ? Benchmarks.size(log) : 0
        let compatLog=steam.appendingPathComponent("logs/compat_log.txt")
        let compatStart=Benchmarks.size(compatLog)
        _ = NotProton.tool("/usr/bin/open",[NotProton.steamApp.path])
        try await launch(id:id,name:name,compatLog:compatLog,from:compatStart,progress:progress)
        try await follow(log:log,from:start,name:name,installer:installer.lastPathComponent,progress:progress)
    }

    /// Whether a process's command line contains `text` (Wine shows a program's Windows path there).
    static func processRunning(containing text:String)->Bool {
        let ps=Process(),pipe=Pipe()
        ps.executableURL=URL(fileURLWithPath:"/bin/ps");ps.arguments=["-axww","-o","command="];ps.standardOutput=pipe
        ps.standardError=FileHandle.nullDevice
        guard (try? ps.run()) != nil else { return false }
        let out=pipe.fileHandleForReading.readDataToEndOfFile();ps.waitUntilExit()
        return String(decoding:out,as:UTF8.self).split(separator:"\n").contains { $0.contains(text) && !$0.contains("/run waitforexitandrun") && !$0.contains("steam.exe") }
    }

    /// A Wine prefix needs symbolic links (its drive letters are links), which exFAT and FAT
    /// disks can't hold; and the folder must be writable.
    static func checkLocation(_ location:URL) throws {
        let probe=location.appendingPathComponent(".steamdriver-probe-\(getpid())")
        do {
            try fm.createDirectory(at:location,withIntermediateDirectories:true)
            try fm.createSymbolicLink(atPath:probe.path,withDestinationPath:"target")
            try? fm.removeItem(atPath:probe.path)
        } catch {
            try? fm.removeItem(atPath:probe.path)
            throw Failure.message("\(location.path) can’t hold a game: \(fm.isWritableFile(atPath:location.path) ? "its disk’s format (exFAT or FAT, common on USB drives) has no symbolic links, which Wine needs. Use an APFS or Mac OS Extended disk" : "it isn’t writable"). Nothing was added to Steam.")
        }
    }
    /// Steam's compatdata/<id> as a link to <location>/<slug>-<id>. A prefix that already exists
    /// (the same game installed before) stays where it is.
    static func placePrefix(id:UInt32,slug:String,in location:URL) throws {
        let compat=steam.appendingPathComponent("steamapps/compatdata")
        let link=compat.appendingPathComponent(String(id))
        if (try? fm.destinationOfSymbolicLink(atPath:link.path)) != nil || fm.fileExists(atPath:link.path) { return }
        let target=location.appendingPathComponent("\(slug)-\(id)")
        try fm.createDirectory(at:target,withIntermediateDirectories:true)
        try fm.createDirectory(at:compat,withIntermediateDirectories:true)
        try fm.createSymbolicLink(at:link,withDestinationURL:target)
    }

    /// Runs the shortcut once Steam is ready for it. A run link sent while Steam is still
    /// starting is dropped (2026-11-26), so: as soon as Steam's compat_log says it mapped this
    /// shortcut to SteamDriver (it has signed in and read its shortcuts), send
    /// steam://rungameid/<game id>, and again (20 times half a second apart, then 10 two seconds apart) until Steam
    /// takes it: its console_log says "GameAction [AppID <id>" the moment it starts a launch
    /// (before StartSession), so no second request reaches a launch already under way. A Steam
    /// that never logs the mapping gets the first try 3 seconds after it started.
    static func launch(id:UInt32,name:String,compatLog:URL,from start:UInt64,progress:@escaping @Sendable (String)->Void) async throws {
        progress("Waiting for Steam to load \(name)…")
        let consoleLog=steam.appendingPathComponent("logs/console_log.txt")
        let began=Date()
        var ready=false
        while Date().timeIntervalSince(began)<180 {
            if Benchmarks.read(compatLog,from:start).contains("Mapping AppID \(id) to tool") { ready=true;break }
            try await Task.sleep(for:.milliseconds(250))
        }
        if !ready { try await Task.sleep(for:.seconds(3)) }
        let gameID=(UInt64(id)<<32) | 0x0200_0000
        let consoleStart=Benchmarks.size(consoleLog),compatSent=Benchmarks.size(compatLog)
        func taken()->Bool {
            Benchmarks.read(consoleLog,from:consoleStart).contains("GameAction [AppID \(id),")
                || Benchmarks.read(compatLog,from:compatSent).contains("StartSession: appID \(id)")
        }
        progress("Asking Steam to start \(name)’s installer…")
        // 20 tries half a second apart, then 10 two seconds apart (about 30 s in all), looking
        // every 0.1 s whether Steam took one.
        for (tries,wait) in [(20,5),(10,20)] {
            for _ in 1...tries {
                // Not in the background: Steam may show a dialog before the launch (below), and
                // it has to be seen.
                _ = NotProton.tool("/usr/bin/open",["steam://rungameid/\(gameID)"])
                for _ in 0..<wait {
                    try await Task.sleep(for:.milliseconds(100))
                    if taken() { return }
                }
            }
            progress("Still asking Steam to start \(name)’s installer…")
        }
        throw Failure.message("\(name) is in Steam, but Steam didn’t start its installer. Launch \(name) from your Steam library to install it.")
    }

    /// What the installer is doing, from the SteamDriver tool's run log for this launch
    /// (compat-tool/run says each step), until it has installed the game or stopped. Creating a
    /// new game's Wine prefix and a big installer starting can take 20 seconds or more, and
    /// nothing shows meanwhile.
    /// The installer counts as finished once its processes (the .exe, and Inno Setup's .tmp
    /// stage, both named after the installer file) were seen and are gone: a GOG installer can
    /// start the game from its last page, and the run script only logs "installed" after the
    /// game exits too.
    static func follow(log:URL,from start:UInt64,name:String,installer:String,progress:@escaping @Sendable (String)->Void) async throws {
        var said=""
        func say(_ text:String) { if text != said { said=text;progress(text) } }
        say("Steam is starting \(name)’s installer…")
        var began=Date()
        let deadline=began.addingTimeInterval(4*3600)
        var launched:Date?
        var waitingSeconds=0
        let stem=(installer as NSString).deletingPathExtension
        var installerSeen=false
        // Steam can stop a launch to ask something first (a dialog: "ShowInterstitials" and
        // the like, waiting for the player's answer); its console log says so.
        let consoleLog=steam.appendingPathComponent("logs/console_log.txt")
        let consoleStart=Benchmarks.size(consoleLog)>65536 ? Benchmarks.size(consoleLog)-65536 : 0
        let appID=log.deletingLastPathComponent().lastPathComponent
        while Date()<deadline {
            try await Task.sleep(for:.seconds(1))
            let text=Benchmarks.read(log,from:start)
            if text.isEmpty {
                let steamText=Benchmarks.read(consoleLog,from:consoleStart)
                let mine=steamText.split(separator:"\n").filter { $0.contains("GameAction [AppID \(appID),") }
                // A wait that lasts (a normal launch passes through one within the second): say
                // so, bring Steam's window forward once, and don't count the time against the
                // two minutes SteamDriver gets to start.
                if let last=mine.last,last.contains("waiting for user response") {
                    waitingSeconds+=1
                    if waitingSeconds>=2 {
                        say("Steam is asking something before it starts \(name). Answer it in Steam’s window.")
                        if waitingSeconds==2 { _ = NotProton.tool("/usr/bin/open",["-a",NotProton.steamApp.path]) }
                        began=Date()
                        continue
                    }
                } else { waitingSeconds=0 }
            }
            if text.contains("launching: ") {
                let running=processRunning(containing:stem)
                if running { installerSeen=true } else if installerSeen { return }
            }
            if text.isEmpty && Date().timeIntervalSince(began)>120 {
                throw Failure.message("Steam started \(name), but SteamDriver never ran it. Check that SteamDriver is its compatibility tool (Properties › Compatibility), then launch it from Steam.")
            }
            if text.contains("installer: ") && text.contains(" installed ") {
                return
            }
            if text.contains(" left no game shortcut") {
                throw Failure.message("\(name)’s installer closed without installing a game that macSteam could find. Launch it from Steam to run the installer again; if it did install, point the shortcut at the game’s .exe in its Properties.")
            }
            if text.contains("game exited (") {
                throw Failure.message("\(name)’s installer closed before it finished. Launch \(name) from Steam to run it again.")
            }
            if text.contains("installed game: ") { return }          // already installed: the game started
            if text.contains("launching: ") {
                if launched==nil { launched=Date() }
                say(Date().timeIntervalSince(launched!)<20
                    ? "\(name)’s installer is opening; a big installer takes a little while…"
                    : "Installing \(name). Finish the installer’s window; install to the folder it suggests.")
            } else if text.contains("installer: running") {
                say("Starting \(name)’s installer…")
            } else if text.contains("creating the prefix") {
                say("Preparing Wine for \(name) (only the first time, about 15 seconds)…")
            } else if !text.isEmpty {
                say("SteamDriver is starting \(name)’s installer…")
            }
        }
    }
}

/// Steam's binary KeyValues (shortcuts.vdf): 0x00 map, 0x01 string, 0x02 int32, 0x03 float,
/// 0x07 uint64, each followed by a NUL-terminated name; 0x08 ends a map.
enum BinaryVDF {
    typealias Map=[(String,Node)]
    indirect enum Node:Equatable {
        case map(Map),string(String),int(UInt32),float(Float),uint64(UInt64)
        static func == (a:Node,b:Node)->Bool {
            switch (a,b) {
            case (.string(let x),.string(let y)): return x==y
            case (.int(let x),.int(let y)): return x==y
            case (.float(let x),.float(let y)): return x==y
            case (.uint64(let x),.uint64(let y)): return x==y
            case (.map(let x),.map(let y)): return x.count==y.count && zip(x,y).allSatisfy { $0.0==$1.0 && $0.1==$1.1 }
            default: return false
            }
        }
    }
    static func parse(_ data:Data)->Map? {
        let bytes=[UInt8](data);var i=0
        func cstring()->String? {
            guard let end=bytes[i...].firstIndex(of:0) else { return nil }
            defer { i=end+1 };return String(decoding:bytes[i..<end],as:UTF8.self)
        }
        func le<T:FixedWidthInteger>(_:T.Type)->T? {
            let n=MemoryLayout<T>.size
            guard i+n<=bytes.count else { return nil }
            var v:T=0
            for k in 0..<n { v|=T(bytes[i+k])<<(8*k) }
            i+=n;return v
        }
        func map()->Map? {
            var out:Map=[]
            while i<bytes.count {
                let type=bytes[i];i+=1
                if type==8 { return out }
                guard let name=cstring() else { return nil }
                switch type {
                case 0: guard let m=map() else { return nil };out.append((name,.map(m)))
                case 1: guard let s=cstring() else { return nil };out.append((name,.string(s)))
                case 2: guard let v=le(UInt32.self) else { return nil };out.append((name,.int(v)))
                case 3: guard let v=le(UInt32.self) else { return nil };out.append((name,.float(Float(bitPattern:v))))
                case 7: guard let v=le(UInt64.self) else { return nil };out.append((name,.uint64(v)))
                default: return nil
                }
            }
            return nil
        }
        return map()
    }
    static func encode(_ root:Map)->Data {
        var out=[UInt8]()
        func cstring(_ s:String) { out+=Array(s.utf8)+[0] }
        func le<T:FixedWidthInteger>(_ v:T) { for k in 0..<MemoryLayout<T>.size { out.append(UInt8(truncatingIfNeeded:v>>(8*k))) } }
        func map(_ m:Map) {
            for (name,node) in m {
                switch node {
                case .map(let c): out.append(0);cstring(name);map(c)
                case .string(let s): out.append(1);cstring(name);cstring(s)
                case .int(let v): out.append(2);cstring(name);le(v)
                case .float(let f): out.append(3);cstring(name);le(f.bitPattern)
                case .uint64(let v): out.append(7);cstring(name);le(v)
                }
            }
            out.append(8)
        }
        map(root)
        return Data(out)
    }
}

/// Steam's text KeyValues, read only (loginusers.vdf): "key" "value" and "key" { … }.
struct TextVDF {
    var value:String?
    var children:[(String,TextVDF)]=[]
    subscript(key:String)->TextVDF? { children.first { $0.0.caseInsensitiveCompare(key) == .orderedSame }?.1 }
    static func parse(_ text:String)->TextVDF {
        var tokens:[String]=[],i=text.startIndex
        while i<text.endIndex {
            let c=text[i]
            if c=="\"" {
                var s="",j=text.index(after:i)
                while j<text.endIndex && text[j] != "\"" {
                    if text[j]=="\\",text.index(after:j)<text.endIndex { j=text.index(after:j) }
                    s.append(text[j]);j=text.index(after:j)
                }
                tokens.append(s);i=j<text.endIndex ? text.index(after:j) : j
            } else if c=="{" || c=="}" { tokens.append(String(c));i=text.index(after:i) }
            else { i=text.index(after:i) }
        }
        var k=0
        func block()->TextVDF {
            var node=TextVDF()
            while k<tokens.count {
                let key=tokens[k];k+=1
                if key=="}" { return node }
                guard k<tokens.count else { break }
                if tokens[k]=="{" { k+=1;node.children.append((key,block())) }
                else { node.children.append((key,TextVDF(value:tokens[k])));k+=1 }
            }
            return node
        }
        return block()
    }
}
