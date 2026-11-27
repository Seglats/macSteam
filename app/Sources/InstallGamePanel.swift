import SwiftUI
import AppKit
import UniformTypeIdentifiers

/// "Install a Windows game" (GameInstaller): drop a game's installer here, or choose it; macSteam
/// adds it to Steam as a non-Steam game on SteamDriver, with the game's Steam artwork, and starts
/// its installer. The next launch from Steam starts the game.
struct InstallGamePanel:View {
    @Bindable var model:AppModel
    @State private var installer:URL?
    @State private var name=""
    @State private var matches:[GameInstaller.StoreMatch]=[]
    @State private var match:GameInstaller.StoreMatch?
    @State private var searching=false
    @State private var targeted=false
    @State private var search:Task<Void,Never>?
    @State private var showLink=false
    @State private var link=""
    @State private var linkProblem=""
    /// Whether any game was installed here (shows the gear for InstalledGamesSheet).
    private var hasInstalled:Bool { _ = model.installedGamesVersion;return !InstalledGames.list().isEmpty }

    var body:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:10) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Install a Windows game").font(LauncherStyle.font(14,.semibold))
                    Text("Drop a game’s installer here, such as a GOG offline installer. \(Brand.name) adds it to Steam with the game’s own Steam artwork and SteamDriver as its compatibility tool, then runs the installer. Launch it from Steam again afterwards to play.")
                        .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                if let installer { chosen(installer) } else {
                    HStack(spacing:10) {
                        dropZone
                        if hasInstalled { installedButton }
                    }.fixedSize(horizontal:false,vertical:true)
                }
                if !model.installingGame.isEmpty {
                    HStack(spacing:8) { ProgressView().controlSize(.small);Text(model.installingGame).font(LauncherStyle.font(12)) }
                }
                if !model.installError.isEmpty {
                    Label(model.installError,systemImage:"exclamationmark.triangle.fill")
                        .font(LauncherStyle.font(12)).foregroundStyle(.orange).fixedSize(horizontal:false,vertical:true)
                        .textSelection(.enabled)
                }
            }
        }
        // Installed: back to the drop zone (an error stays, with its installer, to try again).
        .onChange(of:model.installedGamesVersion) { if model.installError.isEmpty { installer=nil } }
        .onDrop(of:[.fileURL],isTargeted:$targeted) { providers in
            _ = providers.first?.loadObject(ofClass:URL.self) { url,_ in
                if let url,url.pathExtension.lowercased()=="exe" { Task { @MainActor in choose(url) } }
            }
            return true
        }
    }

    /// The games installed here (InstalledGamesSheet): a square beside the drop zone, as tall.
    private var installedButton:some View {
        Button { model.sheet = .installedGames } label: {
            VStack(spacing:6) {
                Image(systemName:"gearshape").font(.system(size:20))
                Text("Installed").font(LauncherStyle.font(11,.medium))
            }.frame(width:84).frame(maxHeight:.infinity).contentShape(Rectangle())
                .background(RoundedRectangle(cornerRadius:12,style:.continuous).fill(.primary.opacity(0.06)))
                .overlay(RoundedRectangle(cornerRadius:12,style:.continuous).strokeBorder(.primary.opacity(0.12),lineWidth:1))
        }.buttonStyle(.plain).hoverGrow().help("Review installed games")
            .disabled(!model.installingGame.isEmpty)
    }
    private var dropZone:some View {
        VStack(spacing:6) {
            Image(systemName:"arrow.down.app").font(.system(size:22)).foregroundStyle(.secondary)
            Text("Drop an installer (.exe) here").font(LauncherStyle.font(12,.medium))
            Button("Choose…",action:pick).buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
        }.frame(maxWidth:.infinity).padding(.vertical,16)
            .background(RoundedRectangle(cornerRadius:12,style:.continuous).fill(.primary.opacity(targeted ? 0.10 : 0.03)))
            .overlay(RoundedRectangle(cornerRadius:12,style:.continuous).strokeBorder(style:StrokeStyle(lineWidth:1.5,dash:[6,4])).foregroundStyle(.secondary.opacity(targeted ? 0.9 : 0.4)))
            .disabled(!model.installingGame.isEmpty)
    }

    private func chosen(_ installer:URL)->some View {
        HStack(alignment:.top,spacing:14) {
            cover
            VStack(alignment:.leading,spacing:8) {
                Text(GameInstaller.files(for:installer).map(\.lastPathComponent).joined(separator:", "))
                    .font(LauncherStyle.font(11)).foregroundStyle(.secondary).lineLimit(2)
                TextField("Game name",text:$name).textFieldStyle(.roundedBorder).font(LauncherStyle.font(13))
                    .onSubmit(find)
                HStack(spacing:8) {
                    Picker("Artwork",selection:$match) {
                        Text("No artwork").tag(GameInstaller.StoreMatch?.none)
                        ForEach(matches) { Text($0.name).tag(GameInstaller.StoreMatch?.some($0)) }
                    }.labelsHidden().frame(maxWidth:240)
                    if searching { ProgressView().controlSize(.small) }
                    Button("Search",action:find).buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                        .help("Look the name up in Steam’s store again")
                }
                storeLink
                installTo
                HStack(spacing:10) {
                    Button("Add to Steam") { model.installGame(name:name,installer:installer,store:match) }
                        .buttonStyle(.borderedProminent).hoverGrow().buttonBorderShape(.capsule)
                        .disabled(name.trimmingCharacters(in:.whitespaces).isEmpty || !model.installingGame.isEmpty)
                    Button("Cancel") { self.installer=nil;model.installError="" }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                        .disabled(!model.installingGame.isEmpty)
                }
                Text("Steam restarts to add the game.").font(LauncherStyle.font(11)).foregroundStyle(.secondary)
            }
        }
    }
    /// When the name search finds nothing, or the wrong game: the game's Steam store link (or its
    /// app id) picks it directly (GameInstaller.appID, storeApp).
    @ViewBuilder private var storeLink:some View {
        let notFound=matches.isEmpty && !searching
        if showLink || notFound {
            VStack(alignment:.leading,spacing:4) {
                Text(notFound ? "Steam’s store found nothing for \u{201C}\(name)\u{201D}. Paste the game’s Steam store link, or its app id:"
                              : "Paste the game’s Steam store link, or its app id:")
                    .font(LauncherStyle.font(11)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                HStack(spacing:8) {
                    TextField("https://store.steampowered.com/app/…",text:$link).textFieldStyle(.roundedBorder).font(LauncherStyle.font(12))
                        .onSubmit(useLink)
                    Button("Use",action:useLink).buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.small)
                        .disabled(link.trimmingCharacters(in:.whitespaces).isEmpty)
                }
                if !linkProblem.isEmpty { Text(linkProblem).font(LauncherStyle.font(11)).foregroundStyle(.orange) }
            }
        } else {
            Button("Wrong game? Paste a Steam store link…") { showLink=true }
                .buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(11,.medium))
        }
    }
    private func useLink() {
        linkProblem=""
        guard let id=GameInstaller.appID(fromLink:link) else { linkProblem="That isn’t a Steam store link. It looks like store.steampowered.com/app/367520/…";return }
        searching=true
        Task {
            let found=await GameInstaller.storeApp(id)
            searching=false
            guard let found else { linkProblem="Steam’s store has no game with id \(id).";return }
            matches=[found]+matches.filter { $0.id != found.id }
            match=found;name=found.name
            showLink=false;link=""
        }
    }
    /// Where the game goes: Steam's folder, or another folder (another disk).
    private var installTo:some View {
        HStack(spacing:8) {
            Text("Install to").font(LauncherStyle.font(12)).foregroundStyle(.secondary)
            Menu {
                Button("Steam’s folder") { model.installLocation=nil }
                Button("Choose a Folder…",action:chooseLocation)
            } label: {
                Text(model.installLocation.map { $0.path.replacingOccurrences(of:NSHomeDirectory(),with:"~") } ?? "Steam’s folder")
                    .font(LauncherStyle.font(12,.medium)).lineLimit(1).truncationMode(.middle)
            }.menuStyle(.borderlessButton).fixedSize()
            if let free=freeSpace { Text(free+" free").font(LauncherStyle.font(11)).foregroundStyle(.secondary) }
        }
    }
    private var freeSpace:String? {
        let place=model.installLocation ?? GameInstaller.steam
        guard let bytes=try? place.resolvingSymlinksInPath().resourceValues(forKeys:[.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage else { return nil }
        return ByteCountFormatter.string(fromByteCount:bytes,countStyle:.file)
    }
    private func chooseLocation() {
        let panel=NSOpenPanel()
        panel.canChooseDirectories=true;panel.canChooseFiles=false;panel.canCreateDirectories=true
        panel.message="Choose where games installed here go (an APFS or Mac OS Extended disk)"
        panel.prompt="Install Here"
        if panel.runModal() == .OK,let url=panel.url {
            do { try GameInstaller.checkLocation(url);model.installLocation=url;model.installError="" }
            catch { model.installError=error.localizedDescription }
        }
    }
    /// The game's Steam cover as it will show in the library.
    private var cover:some View {
        Group {
            if let match {
                AsyncImage(url:GameInstaller.artURL(match.id,"library_600x900.jpg")) { image in image.resizable().scaledToFill() }
                    placeholder: { Color.primary.opacity(0.06) }
            } else {
                ZStack { Color.primary.opacity(0.06);Image(systemName:"photo").foregroundStyle(.secondary) }
            }
        }.frame(width:80,height:120).clipShape(RoundedRectangle(cornerRadius:8,style:.continuous))
    }

    private func pick() {
        let panel=NSOpenPanel()
        panel.allowedContentTypes=[UTType(filenameExtension:"exe") ?? .data]
        panel.message="Choose a game’s Windows installer"
        if panel.runModal() == .OK,let url=panel.url { choose(url) }
    }
    private func choose(_ url:URL) {
        model.installError=""
        installer=url;showLink=false;link="";linkProblem=""
        name=GameInstaller.guessName(url)
        find()
    }
    private func find() {
        search?.cancel()
        let term=name
        searching=true
        search=Task {
            let found=await GameInstaller.search(term)
            guard !Task.isCancelled else { return }
            matches=found;match=found.first;searching=false
        }
    }
}
