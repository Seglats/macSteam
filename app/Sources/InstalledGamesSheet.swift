import SwiftUI
import AppKit

/// The games macSteam installed (InstalledGames), each with Delete, which first asks whether to
/// keep its saves and settings.
struct InstalledGamesSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var games:[InstalledGames.Game]=[]
    @State private var sizes:[String:UInt64]=[:]
    @State private var deleting:InstalledGames.Game?

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:"Installed games",subtitle:"",compact:true)
            Text("Games installed with Install a Windows game. Delete removes the game from Steam and deletes its files; you choose whether its saves and settings are kept for a later reinstall.")
                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            LauncherPanel(inset:12) {
                ScrollView {
                    VStack(alignment:.leading,spacing:10) {
                        if games.isEmpty {
                            Text("No games installed with \(Brand.name).").font(LauncherStyle.font(12)).foregroundStyle(.secondary).padding(.vertical,6)
                        }
                        ForEach(games) { row($0) }
                    }.frame(maxWidth:.infinity,alignment:.leading)
                }.frame(maxHeight:360).fixedSize(horizontal:false,vertical:true)
            }
            if !model.installingGame.isEmpty {
                HStack(spacing:8) { ProgressView().controlSize(.small);Text(model.installingGame).font(LauncherStyle.font(12)) }
            }
            if !model.installError.isEmpty {
                Label(model.installError,systemImage:"exclamationmark.triangle.fill")
                    .font(LauncherStyle.font(12)).foregroundStyle(.orange).fixedSize(horizontal:false,vertical:true)
                    .textSelection(.enabled)
            }
            HStack {
                Spacer()
                Button("Done") { dismiss() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.large)
                    .keyboardShortcut(.cancelAction).disabled(!model.installingGame.isEmpty)
            }
        }.font(LauncherStyle.font(14)).padding(.horizontal,32).padding(.vertical,28).frame(width:540)
            .tint(.primary).background { LauncherBackground() }
            .onAppear(perform:reload)
            .onChange(of:model.installedGamesVersion) { reload() }
            .confirmationDialog(deleting.map { "Delete \($0.name)?" } ?? "",isPresented:Binding(get:{ deleting != nil },set:{ if !$0 { deleting=nil } }),
                                titleVisibility:.visible,presenting:deleting) { game in
                Button("Keep Saves and Settings") { model.deleteGame(game,keep:true) }
                Button("Delete Everything",role:.destructive) { model.deleteGame(game,keep:false) }
                Button("Cancel",role:.cancel) {}
            } message: { game in
                Text("\(game.name) is removed from Steam and its installed files are deleted. Keep Saves and Settings copies its saves and settings first (to SteamDriver/saved-games), and they come back if you install it again with \(Brand.name). Delete Everything deletes them too.")
            }
    }

    private func row(_ game:InstalledGames.Game)->some View {
        HStack(spacing:12) {
            cover(game)
            VStack(alignment:.leading,spacing:2) {
                Text(game.name).font(LauncherStyle.font(13,.semibold)).lineLimit(1)
                Text(detail(game)).font(LauncherStyle.font(11)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
            }
            Spacer(minLength:8)
            Button("Delete…",role:.destructive) { deleting=game }
                .buttonStyle(.borderless).hoverGrow().foregroundStyle(.red).font(LauncherStyle.font(12,.medium))
                .disabled(!model.installingGame.isEmpty)
        }
    }
    private func detail(_ game:InstalledGames.Game)->String {
        var parts=[game.inSteam ? "In Steam" : "Not in Steam any more"]
        if let size=sizes[game.id] { parts.append(size>0 ? ByteCountFormatter.string(fromByteCount:Int64(size),countStyle:.file)+" installed" : "not installed yet") }
        if let place=game.elsewhere { parts.append("in "+place.deletingLastPathComponent().path.replacingOccurrences(of:NSHomeDirectory(),with:"~")) }
        if InstalledGames.hasSaves(game) { parts.append("kept saves from before") }
        return parts.joined(separator:" · ")
    }
    private func cover(_ game:InstalledGames.Game)->some View {
        Group {
            if let url=game.cover,let image=NSImage(contentsOf:url) { Image(nsImage:image).resizable().scaledToFill() }
            else { ZStack { Color.primary.opacity(0.06);Image(systemName:"gamecontroller").foregroundStyle(.secondary) } }
        }.frame(width:40,height:60).clipShape(RoundedRectangle(cornerRadius:6,style:.continuous))
    }
    private func reload() {
        games=InstalledGames.list()
        let list=games
        Task.detached(priority:.utility) {
            for game in list {
                let size=InstalledGames.size(game)
                await MainActor.run { sizes[game.id]=size }
            }
        }
    }
}
