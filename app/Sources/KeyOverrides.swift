import AppKit
import Foundation

/// Key combo overrides: macOS system shortcuts that go to the game instead of macOS while the game
/// is in front (Ctrl+Space, Spotlight, Mission Control …), so key combos players are used to from
/// Windows work. Nothing is overridden until the player adds a combo. Two kinds of list, apart
/// from the DX11/DX12 profiles: Global (key-overrides/global.txt) and one per game
/// (key-overrides/app-<Steam app id>.txt), one combo per line ("ctrl+space"); a game gets Global's
/// plus its own. The game tool's run script reads them and passes them to the Mac driver
/// (WINEMAC_GAME_SHORTCUTS, environments/finals/patches/wine-macdrv-game-shortcuts.patch), which
/// switches the matching WindowServer shortcuts off only while the game is in front. NotProton
/// games don't get them yet (docs/PROFILES.md). The combo names here must match the driver's
/// gs_keycode table.
enum KeyOverrides {
    static let globalID="global"
    /// What the panel and the sheet say about them.
    static let blurb="Override specific key presses so they go to the game instead of macOS. For example, ⌃ Space (Ctrl+Space) normally switches your keyboard input on a Mac; add it here and it slides in the game instead."
    static func gameID(_ appID:String)->String { "app-"+appID }
    static var folder:URL { NotProton.ours.appendingPathComponent("key-overrides") }
    static func file(_ list:String)->URL { folder.appendingPathComponent(list+".txt") }
    /// Every list with combos: Global and each game's.
    static func profilesWithCombos()->[String] {
        let files=(try? FileManager.default.contentsOfDirectory(atPath:folder.path)) ?? []
        return files.filter { $0.hasSuffix(".txt") }.map { String($0.dropLast(4)) }.filter { !load($0).isEmpty }
    }
    /// Only the names macSteam makes, so a list name never becomes another path.
    static func valid(_ list:String)->Bool {
        list==globalID || (list.hasPrefix("app-") && Profiles.valid(String(list.dropFirst(4))))
    }

    static func load(_ profile:String)->[String] {
        guard let text=try? String(contentsOf:file(profile),encoding:.utf8) else { return [] }
        var seen=Set<String>()
        return text.split(whereSeparator:\.isNewline).map { $0.trimmingCharacters(in:.whitespaces).lowercased() }
            .filter { !$0.isEmpty && !$0.hasPrefix("#") && seen.insert($0).inserted }
    }
    static func save(_ profile:String,_ combos:[String]) throws {
        guard valid(profile) else { throw NotProton.failure("\u{201C}\(profile)\u{201D} isn’t a key combo list.") }
        try FileManager.default.createDirectory(at:folder,withIntermediateDirectories:true)
        if combos.isEmpty { try? FileManager.default.removeItem(at:file(profile));return }
        let text="# Key combos that go to the game instead of macOS (written by \(Brand.name)).\n"+combos.joined(separator:"\n")+"\n"
        try text.write(to:file(profile),atomically:true,encoding:.utf8)
    }

    // MARK: Keys (macOS virtual key codes; same names as the driver's gs_keycode)
    private static let keys:[(code:UInt16,name:String,label:String)]=[
        (0,"a","A"),(1,"s","S"),(2,"d","D"),(3,"f","F"),(4,"h","H"),(5,"g","G"),(6,"z","Z"),(7,"x","X"),
        (8,"c","C"),(9,"v","V"),(11,"b","B"),(12,"q","Q"),(13,"w","W"),(14,"e","E"),(15,"r","R"),
        (16,"y","Y"),(17,"t","T"),(18,"1","1"),(19,"2","2"),(20,"3","3"),(21,"4","4"),(22,"6","6"),
        (23,"5","5"),(24,"=","="),(25,"9","9"),(26,"7","7"),(27,"-","-"),(28,"8","8"),(29,"0","0"),
        (30,"]","]"),(31,"o","O"),(32,"u","U"),(33,"[","["),(34,"i","I"),(35,"p","P"),(36,"return","Return"),
        (37,"l","L"),(38,"j","J"),(39,"'","'"),(40,"k","K"),(41,";",";"),(42,"\\","\\"),(43,",",","),
        (44,"/","/"),(45,"n","N"),(46,"m","M"),(47,".","."),(48,"tab","Tab"),(49,"space","Space"),
        (50,"`","`"),(51,"delete","Delete"),(53,"esc","Esc"),
        (122,"f1","F1"),(120,"f2","F2"),(99,"f3","F3"),(118,"f4","F4"),(96,"f5","F5"),(97,"f6","F6"),
        (98,"f7","F7"),(100,"f8","F8"),(101,"f9","F9"),(109,"f10","F10"),(103,"f11","F11"),(111,"f12","F12"),
        (105,"f13","F13"),(107,"f14","F14"),(113,"f15","F15"),(106,"f16","F16"),(64,"f17","F17"),
        (79,"f18","F18"),(80,"f19","F19"),(90,"f20","F20"),(115,"home","Home"),(119,"end","End"),
        (116,"pageup","Page Up"),(121,"pagedown","Page Down"),(123,"left","←"),(124,"right","→"),
        (125,"down","↓"),(126,"up","↑"),
    ]
    private static let mods:[(name:String,flag:NSEvent.ModifierFlags,glyph:String,cgs:UInt32)]=[
        ("ctrl",.control,"⌃",0x40000),("alt",.option,"⌥",0x80000),("shift",.shift,"⇧",0x20000),("cmd",.command,"⌘",0x100000),
    ]

    /// "ctrl+space" for a key press, or nil for a key the driver doesn't know.
    static func combo(keyCode:UInt16,flags:NSEvent.ModifierFlags)->String? {
        guard let key=keys.first(where:{ $0.code==keyCode }) else { return nil }
        return (mods.filter { flags.contains($0.flag) }.map(\.name)+[key.name]).joined(separator:"+")
    }
    /// The macOS key code and modifier names of a combo, or nil for an unknown key or modifier.
    static func keyCode(_ combo:String)->(code:UInt16,mods:Set<String>)? {
        let (m,key)=parts(combo)
        guard let code=keys.first(where:{ $0.name==key })?.code,m.allSatisfy({ name in mods.contains { $0.name==name } }) else { return nil }
        return (code,Set(m))
    }
    private static func parts(_ combo:String)->(mods:[String],key:String) {
        let p=combo.split(separator:"+",omittingEmptySubsequences:false).map(String.init)
        return (Array(p.dropLast()),p.last ?? "")
    }
    /// How the game sees the combo: Wine's Mac driver turns Control into Ctrl, Command into Alt and
    /// Shift into Shift (keyboard.c); Option has no Windows key unless the driver is told so
    /// (LeftOptionIsAlt), so it is named as it is. "Ctrl+Space".
    static func windows(_ combo:String)->String {
        let (m,key)=parts(combo)
        let names:[String:String]=["ctrl":"Ctrl","cmd":"Alt","shift":"Shift","alt":"Option"]
        let order=["ctrl","cmd","shift","alt"]
        let keyNames:[String:String]=["return":"Enter","delete":"Backspace","esc":"Esc","up":"Up","down":"Down","left":"Left","right":"Right"]
        let label=keyNames[key] ?? keys.first { $0.name==key }?.label ?? key.uppercased()
        return (order.filter { m.contains($0) }.compactMap { names[$0] }+[label]).joined(separator:"+")
    }
    /// Key caps for the list: ["⌃","Space"].
    static func caps(_ combo:String)->[String] {
        let (m,key)=parts(combo)
        return mods.filter { m.contains($0.name) }.map(\.glyph)+[keys.first { $0.name==key }?.label ?? key.uppercased()]
    }

    // MARK: What macOS uses the combo for (WindowServer's live shortcut table; read only)
    private typealias GetValue=@convention(c) (Int32,UnsafeMutablePointer<UInt16>,UnsafeMutablePointer<UInt16>,UnsafeMutablePointer<UInt32>)->Int32
    private typealias IsEnabled=@convention(c) (Int32)->Bool
    private static let skyLight=dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",RTLD_LAZY)
    private static func symbol<T>(_ name:String,_:T.Type)->T? {
        guard let skyLight,let p=dlsym(skyLight,name) else { return nil }
        return unsafeBitCast(p,to:T.self)
    }
    private static let names:[Int32:String]=[
        1:"App switcher",2:"App switcher, backwards",7:"Focus the menu bar",8:"Focus the Dock",27:"Next window",32:"Mission Control",33:"Application windows",
        36:"Show desktop",52:"Dock hiding",60:"Previous input source",61:"Next input source",64:"Spotlight",
        65:"Finder search",79:"Move a Space left",81:"Move a Space right",
    ]
    /// What macOS uses the combo for today ("Spotlight"), or nil when no enabled macOS shortcut
    /// uses it (the game gets it anyway).
    static func macOSUse(_ combo:String)->String? {
        guard let get=symbol("CGSGetSymbolicHotKeyValue",GetValue.self),
              let enabled=symbol("CGSIsSymbolicHotKeyEnabled",IsEnabled.self),
              let key=keys.first(where:{ $0.name==parts(combo).key }) else { return nil }
        let want=mods.filter { parts(combo).mods.contains($0.name) }.reduce(UInt32(0)) { $0 | $1.cgs }
        for id in Int32(0)..<512 {
            var ch:UInt16=0,code:UInt16=0,flags:UInt32=0
            guard get(id,&ch,&code,&flags)==0,code==key.code,flags & 0x1e0000==want,enabled(id) else { continue }
            return names[id] ?? "macOS shortcut \(id)"
        }
        return nil
    }

    // MARK: Recording a combo
    /// While recording, this app's own presses of system shortcuts (Ctrl+Space, Cmd+Tab …) reach it
    /// instead of macOS, so they can be recorded: SDL's and Moonlight's keyboard grab
    /// (CGSSetGlobalHotKeyOperatingMode). It applies only while this app is in front and ends
    /// with it, so nothing can be left switched off.
    private typealias SetMode=@convention(c) (Int32,Int32)->Int32
    private typealias Connection=@convention(c) ()->Int32
    /// Switches WindowServer shortcuts back on (Uninstaller: ids a game left switched off).
    private typealias SetEnabled=@convention(c) (Int32,Bool)->Int32
    static func enableSystemShortcuts(_ ids:[Int32]) {
        guard let set=symbol("CGSSetSymbolicHotKeyEnabled",SetEnabled.self) else { return }
        for id in ids { _ = set(id,true) }
    }
    static func grabSystemShortcuts(_ on:Bool) {
        guard let set=symbol("CGSSetGlobalHotKeyOperatingMode",SetMode.self),
              let connection=symbol("_CGSDefaultConnection",Connection.self) else { return }
        _ = set(connection(),on ? 1 : 0)
    }
}

/// Records one key combo: swallows key presses while active and hands back the first one with a
/// key the driver knows (modifier-only presses wait for the key).
@MainActor final class KeyComboRecorder {
    private var monitor:Any?
    var recording:Bool { monitor != nil }
    func start(_ done:@escaping (String)->Void) {
        stop()
        KeyOverrides.grabSystemShortcuts(true)
        monitor=NSEvent.addLocalMonitorForEvents(matching:[.keyDown]) { [weak self] event in
            let flags=event.modifierFlags.intersection(.deviceIndependentFlagsMask)
            if let combo=KeyOverrides.combo(keyCode:event.keyCode,flags:flags) {
                self?.stop();done(combo)
            }
            return nil
        }
    }
    func stop() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor=nil
        KeyOverrides.grabSystemShortcuts(false)
    }
}
