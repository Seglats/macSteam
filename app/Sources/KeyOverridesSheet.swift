import SwiftUI
import AppKit

/// Key combo overrides (KeyOverrides): a searchable dropdown (Global, then the Steam games, those
/// with combos first), the chosen list's combos as key caps in a list that scrolls once it gets
/// long (a game's also shows Global's), and a + under the list that records the next key combo.
struct KeyOverridesSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @Environment(\.colorScheme) private var scheme
    @State private var list=KeyOverrides.globalID
    @State private var combos:[String]=[]
    @State private var inherited:[String]=[]
    @State private var recording=false
    @State private var notice=""
    @State private var recorder=KeyComboRecorder()
    @State private var games:[Profiles.Game]=[]
    @State private var choosing=false
    @State private var search=""

    private var isGlobal:Bool { list==KeyOverrides.globalID }
    private var listTitle:String {
        isGlobal ? "Global" : games.first { KeyOverrides.gameID($0.appID)==list }?.name ?? "App \(list.dropFirst(4))"
    }

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:"Key combo overrides",subtitle:"",compact:true)
            Text(KeyOverrides.blurb+" In the game, ⌃ is Ctrl, ⌘ is Alt and ⇧ is Shift. As soon as you switch to another app, macOS gets the keys back.")
                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            listDropdown
            LauncherPanel(inset:12) {
                VStack(alignment:.leading,spacing:0) {
                    ScrollView {
                        VStack(alignment:.leading,spacing:6) {
                            if combos.isEmpty && inherited.isEmpty && !recording {
                                Text(isGlobal ? "No global overrides." : "No overrides for this game.")
                                    .font(LauncherStyle.font(12)).foregroundStyle(.secondary).padding(.vertical,6)
                            }
                            ForEach(combos,id:\.self) { row($0,from:nil) }
                            ForEach(inherited,id:\.self) { row($0,from:"From Global") }
                        }.frame(maxWidth:.infinity,alignment:.leading)
                    }.frame(maxHeight:240).fixedSize(horizontal:false,vertical:true)
                    Divider().padding(.vertical,8)
                    addRow
                }
            }
            if !notice.isEmpty { LauncherNotice(text:notice) }
            Text("Works in every game that uses SteamDriver; not yet with SteamDriver on CrossOver. Force Quit (⌥⌘⎋), Lock Screen (⌃⌘Q) and other apps’ own shortcuts always stay with macOS.")
                .font(LauncherStyle.font(11)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            HStack {
                Spacer()
                Button("Done") { recorder.stop();dismiss() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.large)
                    .keyboardShortcut(recording ? nil : .cancelAction)
            }
        }.font(LauncherStyle.font(14)).padding(.horizontal,32).padding(.vertical,28).frame(width:540)
            .tint(.primary).background { LauncherBackground() }
            .onAppear {
                let withCombos=Set(KeyOverrides.profilesWithCombos())
                games=Profiles.steamGames().sorted { withCombos.contains(KeyOverrides.gameID($0.appID)) && !withCombos.contains(KeyOverrides.gameID($1.appID)) }
                reload()
            }
            .onChange(of:list) { recorder.stop();recording=false;reload() }
            .onDisappear { recorder.stop() }
    }

    // MARK: Dropdown (searchable)
    private var listDropdown:some View {
        Button { search="";choosing=true } label: {
            HStack(spacing:8) {
                Image(systemName:isGlobal ? "globe" : "gamecontroller").foregroundStyle(.secondary)
                Text(listTitle).font(LauncherStyle.font(13,.semibold)).lineLimit(1)
                Spacer()
                Image(systemName:"chevron.up.chevron.down").font(.system(size:11,weight:.semibold)).foregroundStyle(.secondary)
            }.padding(.horizontal,12).padding(.vertical,8).contentShape(Rectangle())
                .background(LauncherStyle.surface(scheme),in:RoundedRectangle(cornerRadius:10,style:.continuous))
                .overlay { RoundedRectangle(cornerRadius:10,style:.continuous).strokeBorder(.primary.opacity(0.08),lineWidth:1) }
        }.buttonStyle(.plain).hoverGrow().help("Choose Global or one game’s own key combos")
            .popover(isPresented:$choosing,arrowEdge:.bottom) { chooser }
    }
    private var matches:[Profiles.Game] {
        let query=search.trimmingCharacters(in:.whitespaces)
        guard !query.isEmpty else { return games }
        return games.filter { $0.name.localizedCaseInsensitiveContains(query) || $0.appID.hasPrefix(query) }
    }
    private var chooser:some View {
        VStack(alignment:.leading,spacing:8) {
            TextField("Search games or type an app id",text:$search).textFieldStyle(.roundedBorder)
                .onSubmit {
                    if let first=matches.first,!search.isEmpty { choose(KeyOverrides.gameID(first.appID)) }
                    else if Profiles.valid(search) { choose(KeyOverrides.gameID(search)) }
                }
            ScrollView {
                VStack(alignment:.leading,spacing:2) {
                    if search.isEmpty { chooserRow(KeyOverrides.globalID,title:"Global",detail:"Every game") }
                    ForEach(matches) { game in chooserRow(KeyOverrides.gameID(game.appID),title:game.name,detail:"App \(game.appID)"+(game.detected.map { " · \($0.title)" } ?? "")) }
                    if matches.isEmpty && !search.isEmpty {
                        Text(Profiles.valid(search) ? "Press Return to use app \(search)." : "No game matches \u{201C}\(search)\u{201D}.")
                            .font(LauncherStyle.font(11)).foregroundStyle(.secondary).padding(6)
                    }
                }.frame(maxWidth:.infinity,alignment:.leading)
            }.frame(maxHeight:280).fixedSize(horizontal:false,vertical:true)
        }.padding(12).frame(width:340)
    }
    private func chooserRow(_ id:String,title:String,detail:String)->some View {
        Button { choose(id) } label: {
            HStack(spacing:8) {
                Image(systemName:"checkmark").font(.system(size:11,weight:.bold)).opacity(id==list ? 1 : 0)
                VStack(alignment:.leading,spacing:1) {
                    Text(title).font(LauncherStyle.font(13,.medium)).lineLimit(1)
                    Text(detail).font(LauncherStyle.font(11)).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength:0)
                let count=KeyOverrides.load(id).count
                if count>0 { Text("\(count)").font(LauncherStyle.font(11,.semibold)).foregroundStyle(.secondary) }
            }.padding(.horizontal,6).padding(.vertical,5).contentShape(Rectangle())
        }.buttonStyle(.plain).hoverGrow()
    }
    private func choose(_ id:String) { list=id;choosing=false }

    private func row(_ combo:String,from:String?)->some View {
        let use=KeyOverrides.macOSUse(combo)
        return HStack(spacing:10) {
            HStack(spacing:4) {
                ForEach(Array(KeyOverrides.caps(combo).enumerated()),id:\.offset) { _,cap in
                    Text(cap).font(LauncherStyle.font(12,.semibold)).monospaced()
                        .padding(.horizontal,7).padding(.vertical,3)
                        .background(.primary.opacity(0.08),in:RoundedRectangle(cornerRadius:6,style:.continuous))
                }
                Text("/ "+KeyOverrides.windows(combo)).font(LauncherStyle.font(11)).foregroundStyle(.secondary).lineLimit(1).fixedSize()
            }.frame(width:200,alignment:.leading)
            VStack(alignment:.leading,spacing:1) {
                Text(use ?? "Not a macOS shortcut").font(LauncherStyle.font(13,.medium)).fixedSize(horizontal:false,vertical:true)
                Text([use==nil ? "The game gets it anyway" : "Goes to the game instead",from].compactMap { $0 }.joined(separator:" · "))
                    .font(LauncherStyle.font(11)).foregroundStyle(.secondary)
            }
            Spacer(minLength:8)
            if from==nil {
                Button { remove(combo) } label: { Image(systemName:"minus.circle.fill").foregroundStyle(.secondary) }
                    .buttonStyle(.borderless).hoverGrow().help("Give \(KeyOverrides.caps(combo).joined(separator:" ")) back to macOS")
            }
        }.padding(.vertical,3)
    }

    @ViewBuilder private var addRow:some View {
        if recording {
            HStack(spacing:10) {
                Image(systemName:"keyboard").foregroundStyle(.secondary)
                Text("Press a key combo…").font(LauncherStyle.font(13,.medium))
                Spacer()
                Button("Cancel") { recorder.stop();recording=false }.buttonStyle(.borderless).hoverGrow()
            }
        } else {
            Button { startRecording() } label: {
                Label("Add a key combo",systemImage:"plus.circle.fill").font(LauncherStyle.font(13,.medium))
            }.buttonStyle(.borderless).hoverGrow()
        }
    }

    private func reload() {
        combos=KeyOverrides.load(list)
        inherited=isGlobal ? [] : KeyOverrides.load(KeyOverrides.globalID).filter { !combos.contains($0) }
        notice=""
    }
    private func startRecording() {
        notice="";recording=true
        recorder.start { combo in
            recording=false
            if combos.contains(combo) || inherited.contains(combo) {
                notice="\(KeyOverrides.caps(combo).joined(separator:" ")) is already in the list."
                return
            }
            save(combos+[combo])
        }
    }
    private func remove(_ combo:String) { save(combos.filter { $0 != combo }) }
    private func save(_ list:[String]) {
        do { try KeyOverrides.save(self.list,list);combos=list;notice="" }
        catch { notice="Couldn’t save the list: \(error.localizedDescription)" }
    }
}
