import SwiftUI
import AppKit
import UniformTypeIdentifiers

/// The main window: macSteam for NotProton (NotProton.swift). Like NotProton's own app it installs,
/// updates and checks, and launches nothing. macSteam is an extra option in each game's
/// Compatibility settings in Steam, next to NotProton's CrossOver, which it doesn't replace.
/// Nothing here installs Steam or a Wine environment of macSteam's own; Advanced switches to that
/// (SetupView) for troubleshooting.
struct CompanionView:View {
    @Bindable var model:AppModel
    @State private var advanced=false
    @State private var experimental=false
    @State private var confirmRemove=false
    @State private var confirmWindowsSteam=false
    @State private var choosingGame=false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body:some View {
        GeometryReader { geometry in
            ScrollView {
                VStack(spacing:0) {
                    VStack(spacing:20) {
                        // The wordmark is the name; no second "macSteam" as text under it.
                        Wordmark(height:84)
                        statusPanel
                        VStack(spacing:12) { actions }
                        if !model.notice.isEmpty { LauncherNotice(text:model.notice) }
                        gameToolsPanel
                        renderersPanel
                        profilesPanel
                        mousePanel
                        keyOverridesPanel
                        benchmarksPanel
                        advancedPanel
                        experimentalPanel
                        InstallGamePanel(model:model)
                    }.frame(maxWidth:LauncherStyle.contentWidth).padding(.top,34)
                    Spacer(minLength:28)
                    VersionStatus(model:model).padding(.bottom,12)
                    VStack(spacing:12) {
                        HStack(spacing:12) {
                            FooterTile(title:"About",symbol:"info.circle") { model.sheet = .about }
                            FooterTile(title:"Help & FAQ",symbol:"questionmark.circle") { model.makeReport();model.sheet = .help }
                        }
                        // Everything macSteam and its game tools created (UninstallSheet, Uninstaller.swift).
                        Button(role:.destructive) { model.sheet = .uninstall } label: {
                            Label("Uninstall \(Brand.name)…",systemImage:"trash").font(LauncherStyle.font(13,.medium))
                                .frame(maxWidth:.infinity,minHeight:36)
                        }.buttonStyle(.borderless).hoverGrow().foregroundStyle(.red.opacity(0.9))
                            .background(.red.opacity(0.10),in:RoundedRectangle(cornerRadius:LauncherStyle.radius,style:.continuous))
                            .disabled(model.uninstall == .working)
                            .help("Remove \(Brand.name) and everything it set up")
                    }.frame(maxWidth:LauncherStyle.contentWidth).padding(.bottom,24)
                }.padding(.horizontal,36).frame(maxWidth:.infinity).frame(minHeight:geometry.size.height)
            }.scrollIndicators(.hidden)
        }
        .background { LauncherBackground() }
        .font(LauncherStyle.font(14)).tint(.primary)
        .sheet(item:$model.sheet) { sheetView($0) }
        .alert("Remove macSteam from Steam?",isPresented:$confirmRemove) {
            Button("Cancel",role:.cancel) {}
            Button("Remove",role:.destructive,action:model.removeFromNotProton)
        } message: { Text("Removes macSteam’s options from your games’ Compatibility settings, and its driver. NotProton, CrossOver, Steam and your games stay as they are; games that used macSteam run on NotProton’s CrossOver as before.") }
        .alert("Restart Steam now?",isPresented:$model.askRestartSteam) {
            Button("Later",role:.cancel) {}
            Button("Restart Steam",action:model.restartSteam).keyboardShortcut(.defaultAction)
        } message: { Text("Steam reads \(Brand.name)’s options when it starts, so it shows the change after a restart. Steam is asked to quit as usual; if a game or download is running, it stays open.") }
        .alert("Use macSteam’s own Steam?",isPresented:$confirmWindowsSteam) {
            Button("Cancel",role:.cancel) {}
            Button("Continue") { model.useWindowsSteam(true) }
        } message: { Text("For troubleshooting. \(Brand.name) installs Windows Steam in a Wine environment of its own, apart from NotProton and the macOS Steam app. It needs about 70 GB per game. You can come back here at any time.") }
        .task {
            model.startup()
            // A game started from Steam reports the mouse while this window is open.
            while !Task.isCancelled {
                try? await Task.sleep(for:.seconds(4))
                model.checkNotProton()
            }
        }
    }

    @ViewBuilder private func sheetView(_ sheet:AppModel.Sheet)->some View {
        switch sheet {
        case .help: HelpSheet(model:model)
        case .about: AboutSheet(model:model)
        case .uninstall: UninstallSheet(model:model).interactiveDismissDisabled(model.uninstall == .working)
        case .removeOwnSteam: RemoveOwnSteamSheet(model:model).interactiveDismissDisabled(model.ownSteamRemoval != .idle)
        case .surroundSettings: SurroundSettingsSheet(model:model)
        case .update: UpdateSheet(model:model).interactiveDismissDisabled(model.update == .installing)
            .onDisappear { model.updateSheetClosed() }
        case .whatsNew: WhatsNewSheet()
        case .settings: SettingsSheet(model:model)
        case .keyOverrides: KeyOverridesSheet(model:model)
        case .profileSettings: ProfileSettingsSheet(model:model)
        case .controllerCheck: ControllerCheckSheet(model:model)
        case .benchmarks: BenchmarksSheet(model:model)
        case .installedGames: InstalledGamesSheet(model:model)
        }
    }

    // MARK: Status
    private var statusPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:12) {
                switch model.notProton {
                case .notInstalled:
                    StatusLine(.problem,"NotProton isn’t installed. It turns on Steam Play in the macOS Steam app. Install it, open it once so it sets up Steam, then come back here.")
                case .unsupported(let builds):
                    StatusLine(.good,"NotProton is installed.")
                    if builds.isEmpty {
                        StatusLine(.warning,"NotProton has no CrossOver yet. SteamDriver on CrossOver needs CrossOver 26.3 added in NotProton.")
                    } else {
                        StatusLine(.warning,"\(Brand.name)’s driver is built for CrossOver 26.3. NotProton uses \(builds.joined(separator:", ")).")
                    }
                case .available(let tools):
                    StatusLine(.good,"NotProton is installed, with \(names(tools,\.display)).")
                case .added(let tools):
                    StatusLine(.good,"NotProton is installed, with \(names(tools,\.display)).")
                    if model.rowsRemoved {
                        StatusLine(.warning,"The SteamDriver rows (mouse and profiles) were removed in NotProton’s plugin page, so \(Brand.name) leaves them out. The driver and renderers are still there.",
                                   button:"Put Rows Back",action:model.restoreRows)
                    }
                }
            }
        }
    }
    private func names(_ tools:[NotProton.Tool],_ key:KeyPath<NotProton.Tool,String>)->String {
        tools.map { "\u{201C}\($0[keyPath:key])\u{201D}" }.joined(separator:" and ")
    }

    /// The first step, large at the top until it's done: get NotProton, then add the SteamDriver
    /// tool (also in Game runtimes, where it stays once added).
    @ViewBuilder private var actions:some View {
        if case .notInstalled=model.notProton {
            LauncherPrimaryAction(title:"Get NotProton") { NSWorkspace.shared.open(NotProton.projectURL) }
        } else if let tool=model.gameTools.first,model.gameToolStates[tool.id] ?? .notInstalled == .notInstalled {
            LauncherPrimaryAction(title:model.installingGameTool != nil ? "Adding SteamDriver…" : "Add to Steam") { model.installGameTool(tool) }
                .keyboardShortcut(.defaultAction).disabled(model.installingGameTool != nil || model.crossOverLicense != .valid)
        }
    }

    // MARK: Game runtimes: SteamDriver for every game (NotProton.swift), and games with a Wine of
    // their own (GameTools.swift)
    private var gameToolsPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:12) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Game runtimes").font(LauncherStyle.font(14,.semibold))
                    Text("How your games run in Steam. SteamDriver is \(Brand.name)’s own Wine, for every game: in a game’s Properties › Compatibility, turn on \u{201C}Force the use of a specific Steam Play compatibility tool\u{201D} and choose \u{201C}SteamDriver\u{201D}. SteamDriver on CrossOver instead adds \(Brand.name)’s driver, renderer and mouse options to NotProton’s CrossOver, for games that run better there.")
                        .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                if case .notInstalled=model.notProton {
                    StatusLine(.problem,"Needs NotProton: install it and open it once first.")
                }
                if !model.gameTools.isEmpty,model.crossOverLicense != .valid {
                    StatusLine(.problem,model.crossOverLicense.message)
                }
                ForEach(model.gameTools) { tool in
                    runtimeRow(tool.display,subtitle:"Global",detail:gameToolDetail(tool)) { gameToolButtons(tool) }
                }
                runtimeRow("SteamDriver on CrossOver",subtitle:"NotProton options",detail:steamDriverDetail) { steamDriverButtons }
                if case .added=model.notProton {
                    Button(model.restartingSteam ? "Restarting Steam…" : "Restart Steam",action:model.restartSteam)
                        .buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium)).disabled(model.restartingSteam)
                }
            }
        }
    }
    private func runtimeRow<Buttons:View>(_ title:String,subtitle:String,detail:String,@ViewBuilder buttons:()->Buttons)->some View {
        HStack(alignment:.center,spacing:12) {
            VStack(alignment:.leading,spacing:2) {
                HStack(spacing:6) {
                    Text(title).font(LauncherStyle.font(13,.semibold))
                    Text(subtitle).font(LauncherStyle.font(11,.semibold)).foregroundStyle(.secondary)
                }
                Text(detail).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
            }
            Spacer(minLength:12)
            buttons()
        }
    }
    private var steamDriverDetail:String {
        switch model.notProton {
        case .notInstalled: return "macSteam’s Mac driver, renderers and mouse options on NotProton’s CrossOver. Needs NotProton."
        case .unsupported(let builds): return "Built for CrossOver 26.3; \(builds.isEmpty ? "NotProton has no CrossOver: add CrossOver 26.3 in NotProton to use this." : "NotProton uses "+builds.joined(separator:", ")+".")"
        case .available: return "macSteam’s Mac driver, renderers and mouse options on NotProton’s CrossOver. Not in Steam yet."
        case .added:
            if !model.notProtonCurrent { return "In Steam; this version of \(Brand.name) has newer files." }
            return "In Steam. In a game’s Properties › Compatibility, keep NotProton’s CrossOver; under Developer choose \u{201C}SteamDriver driver\u{201D}, a \u{201C}SteamDriver renderer\u{201D} if you like, and turn on \u{201C}SteamDriver mouse\u{201D}. Save them as NotProton’s defaults for CrossOver 26.3 to use them everywhere."
        }
    }
    @ViewBuilder private var steamDriverButtons:some View {
        switch model.notProton {
        case .notInstalled, .unsupported: EmptyView()
        case .available:
            Button("Add to Steam",action:model.addToNotProton).buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
        case .added:
            HStack(spacing:10) {
                if !model.notProtonCurrent {
                    Button("Update",action:model.addToNotProton).buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                }
                Button("Remove") { confirmRemove=true }.buttonStyle(.borderless).hoverGrow().foregroundStyle(.red).font(LauncherStyle.font(12,.medium))
            }
        }
    }
    private func gameToolDetail(_ tool:GameTools.Tool)->String {
        if model.installingGameTool==tool.id { return "Installing \u{201C}\(tool.display)\u{201D}…" }
        switch model.gameToolStates[tool.id] ?? .notInstalled {
        case .notInstalled: return "\(Brand.name)’s own Wine for every game: Apple D3DMetal, key combo overrides, the High-precision mouse. Not in Steam yet (runtime \(tool.runtime))."+(GameTools.legacyInstalled ? " Replaces the older \u{201C}SteamDriver: THE FINALS\u{201D} tool; choose SteamDriver for THE FINALS again afterwards." : "")
        case .installed: return "In Steam (runtime \(tool.runtime)). Choose \u{201C}SteamDriver\u{201D} as a game’s compatibility tool."+(model.gptk.version == nil ? " It needs Apple’s Game Porting Toolkit, below under Renderers." : "")
        case .outdated(let old): return "In Steam with runtime \(old); this version of \(Brand.name) has \(tool.runtime)."
        case .foreign: return "Steam has a \u{201C}\(tool.id)\u{201D} tool \(Brand.name) didn’t install; it is left as it is."
        }
    }
    @ViewBuilder private func gameToolButtons(_ tool:GameTools.Tool)->some View {
        let busy=model.installingGameTool != nil || model.crossOverLicense != .valid
        switch model.gameToolStates[tool.id] ?? .notInstalled {
        case .notInstalled:
            Button("Add to Steam") { model.installGameTool(tool) }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).disabled(busy)
        case .outdated:
            HStack(spacing:10) {
                Button("Update") { model.installGameTool(tool) }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).disabled(busy)
                Button("Remove") { model.removeGameTool(tool) }.buttonStyle(.borderless).hoverGrow().foregroundStyle(.red).disabled(model.installingGameTool != nil)
            }
        case .installed:
            Button("Remove") { model.removeGameTool(tool) }.buttonStyle(.borderless).hoverGrow().foregroundStyle(.red).font(LauncherStyle.font(12,.medium)).disabled(model.installingGameTool != nil)
        case .foreign:
            EmptyView()
        }
    }

    // MARK: Apple's Game Porting Toolkit (NotProtonExtras.gptkStatus)
    @ViewBuilder private var gptkStatus:some View {
        let steps="1. Download Game Porting Toolkit 4 from [Apple](https://developer.apple.com/games/game-porting-toolkit/).\n2. Open the Game Porting Toolkit, then open \u{201C}Evaluation environment for Windows games\u{201D} in it.\n3. Copy its redist folder into the folder that Show Folder opens."
        switch model.gptk {
        case .ready(let version):
            StatusLine(.good,"Apple D3DMetal \(version), from your Game Porting Toolkit. THE FINALS uses it; NotProton games can choose it under SteamDriver renderer, with D3DMetal as the graphics backend.")
        case .empty:
            StatusLine(.pending,"\(Brand.name) doesn’t include Apple D3DMetal (DirectX 12). To add it:\n"+steps,markdown:true)
        case .missing(let files):
            StatusLine(.warning,"The Game Porting Toolkit folder is incomplete: \(files.prefix(3).joined(separator:", ").replacingOccurrences(of:"_",with:"\\_"))\(files.count>3 ? " and \(files.count-3) more" : "").\n"+steps,markdown:true)
        case .notApple(let file):
            StatusLine(.problem,"\(file) in the Game Porting Toolkit folder isn’t signed by Apple, so nothing uses it. Copy the redist folder again from Apple’s image.")
        case .tooOld(let version):
            StatusLine(.warning,"D3DMetal \(version) is too old: THE FINALS needs Game Porting Toolkit 4 or later.")
        }
    }

    // MARK: Renderers
    private var renderersPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:12) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Renderers").font(LauncherStyle.font(14,.semibold))
                    Text("Offered per game under \u{201C}SteamDriver renderer\u{201D}, next to NotProton’s own. Pick the matching graphics backend for the game too.")
                        .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                StatusLine(.good,"SteamDriver DXMT: included with \(Brand.name). Use it with DXMT as the graphics backend.")
                gptkStatus
                Button { model.showGPTKFolder() } label: { Label("Show GPTK Folder",systemImage:"folder") }
                    .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                    .help("Open the folder Apple’s files go in")
            }
        }
    }

    // MARK: Profiles (Profiles.swift)
    private var profilesPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:10) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Profiles").font(LauncherStyle.font(14,.semibold))
                    Text("Renderer settings for every DX11 or DX12 game, or for one game, which wins over the global one.")
                        .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                ForEach(model.profiles) { profile in
                    HStack(alignment:.center,spacing:12) {
                        Image(systemName:profile.isGlobal ? "square.stack.3d.up" : "gamecontroller").foregroundStyle(.secondary).frame(width:18)
                        VStack(alignment:.leading,spacing:2) {
                            HStack(spacing:6) {
                                Text(profile.name).font(LauncherStyle.font(13,.semibold)).lineLimit(1)
                                if !profile.isGlobal { Text(profile.kind.title).font(LauncherStyle.font(11,.semibold)).foregroundStyle(.secondary) }
                            }
                            Text(profileDetail(profile)).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                        }
                        Spacer(minLength:12)
                        if let app=profile.appID,let seen=Profiles.detected(app),seen != profile.kind {
                            Button("Switch to \(seen.title)") { model.switchProfile(profile,to:seen) }
                                .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                                .help("Make this a \(seen.title) profile; DX11 settings are kept for switching back")
                        } else {
                            Button("Settings…") { model.editProfile(profile) }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                        }
                        Button { model.removeProfile(profile) } label: { Image(systemName:"minus.circle.fill").foregroundStyle(.secondary) }
                            .buttonStyle(.borderless).hoverGrow().help("Remove \(profile.name)")
                    }
                }
                HStack(spacing:10) {
                    ForEach(Profiles.Kind.allCases) { kind in
                        if !model.profiles.contains(where: { $0.id==Profiles.globalID(kind) }) {
                            Button { model.addGlobalProfile(kind) } label: { Label("Global \(kind.title)",systemImage:"plus") }
                                .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                        }
                    }
                    Button { choosingGame=true } label: { Label("Game Profile…",systemImage:"plus") }
                        .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                        .popover(isPresented:$choosingGame,arrowEdge:.bottom) {
                            GameProfilePicker(taken:Set(model.profiles.compactMap(\.appID))) { game,kind in
                                choosingGame=false;model.addGameProfile(game,kind)
                            }
                        }
                }
            }
        }
    }
    private func profileDetail(_ p:Profiles.Profile)->String {
        // A game whose last launch through SteamDriver loaded the other Direct3D.
        if let app=p.appID,let seen=Profiles.detected(app),seen != p.kind {
            return "Its last launch used \(seen.title), so this \(p.kind.title) profile doesn’t apply. If the game runs as \(seen.title) now, switch the profile."
        }
        if p.kind == .dx12 {
            let summary=Profiles.d3dmetalSummary(Profiles.d3dmetalSettings(p.id))
            return (p.isGlobal ? "Every DX12 game on the SteamDriver tool without its own profile. " : "")+"\(summary). Read by the SteamDriver tool at launch; no row in Steam."
        }
        let row=p.isGlobal ? "\u{201C}SteamDriver DX11 profile\u{201D}" : "\u{201C}SteamDriver profile: \(p.name)\u{201D}"
        let settings=Profiles.settings(p.id).map { Profiles.summary($0).prefix(1).uppercased()+Profiles.summary($0).dropFirst() } ?? "No settings file"
        let shown=NotProton.rowProfiles().contains(p) ? "Row in Steam: \(row)" : "Not in Steam: more than \(NotProton.maxProfileRows) profiles"
        return "\(settings). \(shown)."
    }

    // MARK: Mouse
    private var mousePanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:8) {
                Text("High-precision mouse").font(LauncherStyle.font(14,.semibold))
                SteamMouseStatus(model:model)
            }
        }
    }

    // MARK: Key combo overrides (KeyOverridesSheet)
    private var keyOverridesPanel:some View {
        LauncherPanel(inset:16) {
            HStack(alignment:.center,spacing:12) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Key combo overrides").font(LauncherStyle.font(14,.semibold))
                    Text(keyOverridesSummary).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                Spacer(minLength:12)
                Button("Key Combos…") { model.sheet = .keyOverrides }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
            }
        }
    }
    private var keyOverridesSummary:String {
        let count=KeyOverrides.profilesWithCombos().reduce(0) { $0+KeyOverrides.load($1).count }
        return KeyOverrides.blurb+(count==0 ? "" : " \(count) set up now.")
    }

    /// Advanced › Custom runtimes (Runtimes.swift).
    private var customRuntimesRow:some View {
        HStack(alignment:.center,spacing:12) {
            VStack(alignment:.leading,spacing:2) {
                Text("Custom runtimes").font(LauncherStyle.font(13,.semibold))
                let shared=Runtimes.sharedTemplates()
                Text("SteamDriver games share one Wine setup per runtime: a new game’s prefix is a copy of it, ready at once and sharing its disk space\(shared.isEmpty ? "" : " (now: "+shared.joined(separator:", ")+")"). Turn this on to give a game its own copy instead, in its game profile’s Settings.")
                    .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
            }
            Spacer(minLength:12)
            Toggle("",isOn:Binding(get:{ model.customRuntimes },set:{ model.setCustomRuntimes($0) }))
                .labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                .help("Let game profiles have their own runtime copy")
        }
    }

    // MARK: Benchmarks (BenchmarksSheet)
    private var benchmarksPanel:some View {
        LauncherPanel(inset:16) {
            HStack(alignment:.center,spacing:12) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Benchmarks").font(LauncherStyle.font(14,.semibold))
                    Text(benchmarksSummary).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                    if !model.benchStatus.isEmpty {
                        Text(model.benchStatus).font(LauncherStyle.font(12,.medium)).fixedSize(horizontal:false,vertical:true)
                    }
                }
                Spacer(minLength:12)
                Button("Benchmarks…") { model.sheet = .benchmarks }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
            }
        }
    }
    private var benchmarksSummary:String {
        let recording:String
        switch model.benchRecord {
        case .off: recording="Recording is off."
        case .all: recording="Recording in every SteamDriver game; \(KeyOverrides.caps(model.benchHotkey).joined()) starts and stops a run."
        case .games(let ids): recording="Recording in \(ids.count) game\(ids.count==1 ? "" : "s"); \(KeyOverrides.caps(model.benchHotkey).joined()) starts and stops a run."
        }
        let runs=model.benchRuns.count
        return "fps, frame times and latency of a stretch of play. "+recording+(runs==0 ? "" : " \(runs) run\(runs==1 ? "" : "s") saved.")
    }

    // MARK: Experimental (ControllerSupport.swift)
    /// The experimental features, listed by name until opened (like Advanced).
    private static let experimentalItems=["More controller support","Headphone surround"]
    /// Experimental › Headphone surround (Surround.swift): unlocks the setting in profiles.
    private var surroundRow:some View {
        VStack(alignment:.leading,spacing:10) {
        HStack(alignment:.top,spacing:12) {
            VStack(alignment:.leading,spacing:4) {
                Text("Headphone surround").font(LauncherStyle.font(13,.semibold))
                Text("Turns a game’s 5.1 or 7.1 surround sound into 3D sound for headphones, so you can hear where things are, behind you too. Stereo sound stays as it is, so the game must play surround: most do once your headphones appear as 7.1; set a game’s own audio option to 7.1 or surround if it has one, not Stereo or Headphones. Wine’s audio driver does it, in SteamDriver games only.")
                    .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                Text(model.surroundOn ? "Turn it on per profile: Global DX11, Global DX12 or a game profile, under Settings…. It applies from the game’s next launch."
                                      : "When it’s on, profiles get a Headphone surround setting.")
                    .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
            }
            Spacer(minLength:12)
            VStack(alignment:.trailing,spacing:8) {
                Toggle("",isOn:Binding(get:{ model.surroundOn },set:{ model.setSurround($0) }))
                    .labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                    .help("Show the Headphone surround setting in profiles")
                Button("Settings…") { model.sheet = .surroundSettings }
                    .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                    .help("Listen and tune: speaker positions, tilt, width, room and tone, with a live test; Save to use them in games")
            }
        }
        }
    }
    private var experimentalPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:10) {
                Button { withAnimation(reduceMotion ? nil : .easeOut(duration:0.15)) { experimental.toggle() } } label: {
                    HStack(spacing:6) {
                        Text("Experimental").font(LauncherStyle.font(14,.semibold))
                        Image(systemName:"chevron.down").font(.system(size:10,weight:.semibold)).rotationEffect(.degrees(experimental ? 180 : 0))
                        Spacer(minLength:0)
                    }.contentShape(Rectangle())
                }.buttonStyle(.plain).hoverGrow()
                if !experimental {
                    Text(Self.experimentalItems.joined(separator:" · ")).font(LauncherStyle.font(12)).foregroundStyle(.secondary)
                } else {
                    HStack(alignment:.top,spacing:12) {
                        VStack(alignment:.leading,spacing:4) {
                            Text("More controller support").font(LauncherStyle.font(13,.semibold))
                            Text("Adds XInput support for controllers that offer it, which work as XInput controllers on Windows and Linux but not on a Mac. Steam would otherwise keep these controllers for itself, so games couldn’t see them.")
                                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                            Text("While it’s on, these controllers work in your games but not in Steam’s own menus or Big Picture. Microsoft Xbox controllers aren’t affected. Steam restarts to apply a change.")
                                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                        }
                        Spacer(minLength:12)
                        VStack(alignment:.trailing,spacing:10) {
                            Toggle("",isOn:Binding(get:{ model.controllerSupportOn },set:{ model.setControllerSupport($0) }))
                                .labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                                .help("Hand XInput controllers to SteamDriver games instead of Steam")
                            Button("Controller Problems…") { model.sheet = .controllerCheck }
                                .buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                                .help("A guided check of how your controller reaches games, with a report for adding support")
                        }
                    }
                    controllerStatus
                    Text("Tested so far: 8BitDo Ultimate 2.").font(LauncherStyle.font(11)).foregroundStyle(.tertiary)
                    Divider().padding(.vertical,4)
                    surroundRow
                }
            }
        }
    }
    @ViewBuilder private var controllerStatus:some View {
        let saved=model.controllerPads
        let new=model.connectedPads.filter { pad in !saved.contains { $0.id == pad.id } }
        if model.controllerSupportOn {
            Text("For games: "+saved.map(\.name).joined(separator:", ")+".")
                .font(LauncherStyle.font(12,.medium)).fixedSize(horizontal:false,vertical:true)
            let wanted=ControllerSupport.value(saved)
            if !model.steamRunningNow {
                Text("Applies when Steam starts.").font(LauncherStyle.font(12)).foregroundStyle(.secondary)
            } else if model.steamControllerValue == wanted {
                Label("Active: Steam started with it.",systemImage:"checkmark.circle.fill")
                    .font(LauncherStyle.font(12)).foregroundStyle(.green)
            } else {
                HStack(spacing:10) {
                    Text("Steam is still using the old setting.").font(LauncherStyle.font(12)).foregroundStyle(.orange)
                    Spacer(minLength:8)
                    Button("Restart Steam",action:model.restartSteam).buttonStyle(.bordered).hoverGrow()
                        .buttonBorderShape(.capsule).disabled(model.restartingSteam)
                }
            }
            if !new.isEmpty {
                HStack(spacing:10) {
                    Text("Also connected: "+new.map(\.name).joined(separator:", ")+".")
                        .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                    Spacer(minLength:8)
                    Button("Add",action:model.addConnectedControllers).buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule)
                }
            }
        } else if !model.connectedPads.isEmpty {
            Text("Connected now: "+model.connectedPads.map(\.name).joined(separator:", ")+".")
                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
        }
    }

    // MARK: Advanced
    private var advancedPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:10) {
                Button { withAnimation(reduceMotion ? nil : .easeOut(duration:0.15)) { advanced.toggle() } } label: {
                    HStack(spacing:6) {
                        Text("Advanced").font(LauncherStyle.font(14,.semibold))
                        Image(systemName:"chevron.down").font(.system(size:10,weight:.semibold)).rotationEffect(.degrees(advanced ? 180 : 0))
                        Spacer(minLength:0)
                    }.contentShape(Rectangle())
                }.buttonStyle(.plain).hoverGrow()
                if advanced {
                    HStack(alignment:.center,spacing:12) {
                        VStack(alignment:.leading,spacing:2) {
                            Text("\(Brand.name)’s own Steam").font(LauncherStyle.font(13,.semibold))
                            Text("For troubleshooting: Windows Steam in \(Brand.name)’s own Wine environment, instead of NotProton.")
                                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                        }
                        Spacer(minLength:12)
                        if model.ownSteamExists {
                            Button("Remove…") { model.sheet = .removeOwnSteam }.buttonStyle(.borderless).hoverGrow().foregroundStyle(.red).disabled(model.busy)
                                .help("Remove \(Brand.name)’s own Steam; its games can move to the Steam app")
                        }
                        Button("Open…") { confirmWindowsSteam=true }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).disabled(model.busy)
                    }
                    customRuntimesRow
                }
            }
        }
    }
}

/// Picks the game for a new game profile: installed Steam games and those started with a game
/// tool, searchable, with DX11 or DX12 (known games preset).
struct GameProfilePicker:View {
    let taken:Set<String>
    let pick:(Profiles.Game,Profiles.Kind)->Void
    @State private var games:[Profiles.Game]=[]
    @State private var search=""
    @State private var kind:Profiles.Kind = .dx11
    @State private var chosen:Profiles.Game?
    private var matches:[Profiles.Game] {
        let query=search.trimmingCharacters(in:.whitespaces)
        let open=games.filter { !taken.contains($0.appID) }
        guard !query.isEmpty else { return open }
        return open.filter { $0.name.localizedCaseInsensitiveContains(query) || $0.appID.hasPrefix(query) }
    }
    var body:some View {
        VStack(alignment:.leading,spacing:8) {
            TextField("Search games or type an app id",text:$search).textFieldStyle(.roundedBorder)
                .onSubmit { if let first=matches.first { choose(first) } else if Profiles.valid(search) { choose(Profiles.Game(appID:search,name:"App \(search)")) } }
            ScrollView {
                VStack(alignment:.leading,spacing:2) {
                    ForEach(matches) { game in
                        Button { choose(game) } label: {
                            HStack(spacing:8) {
                                Image(systemName:"checkmark").font(.system(size:11,weight:.bold)).opacity(chosen==game ? 1 : 0)
                                VStack(alignment:.leading,spacing:1) {
                                    Text(game.name).font(LauncherStyle.font(13,.medium)).lineLimit(1)
                                    Text("App \(game.appID)"+(game.detected.map { " · \($0.title), detected" } ?? "")).font(LauncherStyle.font(11)).foregroundStyle(.secondary)
                                }
                                Spacer(minLength:0)
                            }.padding(.horizontal,6).padding(.vertical,5).contentShape(Rectangle())
                        }.buttonStyle(.plain).hoverGrow()
                    }
                    if matches.isEmpty {
                        Text(Profiles.valid(search) ? "Press Return to use app \(search)." : games.isEmpty ? "No Steam games found." : "No game matches \u{201C}\(search)\u{201D}.")
                            .font(LauncherStyle.font(11)).foregroundStyle(.secondary).padding(6)
                    }
                }.frame(maxWidth:.infinity,alignment:.leading)
            }.frame(maxHeight:260).fixedSize(horizontal:false,vertical:true)
            Divider()
            HStack {
                Picker("",selection:$kind) { ForEach(Profiles.Kind.allCases) { Text($0.title).tag($0) } }
                    .pickerStyle(.segmented).labelsHidden().frame(width:120)
                Spacer()
                Button("Add") { if let chosen { pick(chosen,kind) } }.buttonStyle(.borderedProminent).hoverGrow().disabled(chosen==nil)
                    .keyboardShortcut(.defaultAction)
            }
        }.padding(12).frame(width:340)
            .onAppear { games=Profiles.steamGames() }
    }
    private func choose(_ game:Profiles.Game) {
        chosen=game
        kind=Profiles.suggestedKind(game.appID)
    }
}
