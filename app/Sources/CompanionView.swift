import SwiftUI
import AppKit

/// The main window: MacVolvo for NotProton (NotProton.swift). Like NotProton's own app it installs,
/// updates and checks, and launches nothing. MacVolvo is an extra option in each game's
/// Compatibility settings in Steam, next to NotProton's CrossOver, which it doesn't replace.
/// Nothing here installs Steam or a Wine environment of MacVolvo's own; Advanced switches to that
/// (SetupView) for troubleshooting.
struct CompanionView:View {
    @Bindable var model:AppModel
    @State private var advanced=false
    @State private var confirmRemove=false
    @State private var confirmWindowsSteam=false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body:some View {
        GeometryReader { geometry in
            ScrollView {
                VStack(spacing:0) {
                    VStack(spacing:20) {
                        Wordmark(height:84)
                        LauncherHeading(title:"MacVolvo for NotProton",subtitle:"Mouse and display fixes for the games you play in the macOS Steam app.")
                        statusPanel
                        VStack(spacing:12) { actions }
                        if !model.notice.isEmpty { LauncherNotice(text:model.notice) }
                        mousePanel
                        advancedPanel
                    }.frame(maxWidth:LauncherStyle.contentWidth).padding(.top,34)
                    Spacer(minLength:28)
                    VersionStatus(model:model).padding(.bottom,12)
                    HStack(spacing:12) {
                        FooterTile(title:"Help & FAQ",symbol:"questionmark.circle") { model.makeReport();model.sheet = .help }
                        FooterTile(title:"About",symbol:"info.circle") { model.sheet = .about }
                    }.frame(maxWidth:LauncherStyle.contentWidth).padding(.bottom,24)
                }.padding(.horizontal,36).frame(maxWidth:.infinity).frame(minHeight:geometry.size.height)
            }.scrollIndicators(.hidden)
        }
        .background { LauncherBackground() }
        .font(LauncherStyle.font(14)).tint(.primary)
        .sheet(item:$model.sheet) { sheet in
            switch sheet {
            case .help: HelpSheet(model:model)
            case .about: AboutSheet(model:model)
            case .uninstall: UninstallSheet(model:model).interactiveDismissDisabled(model.uninstall == .working)
            case .update: UpdateSheet(model:model).interactiveDismissDisabled(model.update == .installing)
                .onDisappear { model.updateSheetClosed() }
            case .whatsNew: WhatsNewSheet()
            case .settings: SettingsSheet(model:model)
            }
        }
        .alert("Remove MacVolvo from Steam?",isPresented:$confirmRemove) {
            Button("Cancel",role:.cancel) {}
            Button("Remove",role:.destructive,action:model.removeFromNotProton)
        } message: { Text("Removes MacVolvo’s options from your games’ Compatibility settings, and its driver. NotProton, CrossOver, Steam and your games stay as they are; games that used MacVolvo run on NotProton’s CrossOver as before.") }
        .alert("Use MacVolvo’s own Steam?",isPresented:$confirmWindowsSteam) {
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

    // MARK: Status
    private var statusPanel:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:12) {
                switch model.notProton {
                case .notInstalled:
                    StatusLine(.problem,"NotProton isn’t installed. It turns on Steam Play in the macOS Steam app. Install it, open it once so it sets up Steam, then come back here.")
                case .unsupported(let builds):
                    StatusLine(.good,"NotProton is installed.")
                    StatusLine(.warning,"\(Brand.name)’s driver is built for CrossOver 26.3. NotProton uses \(builds.isEmpty ? "no CrossOver yet; open NotProton to set it up" : builds.joined(separator:", ")).")
                case .available(let tools):
                    StatusLine(.good,"NotProton is installed, with \(names(tools,\.display)).")
                    StatusLine(.pending,"\(Brand.name)’s options aren’t in Steam yet.")
                case .added(let tools):
                    StatusLine(.good,"NotProton is installed, with \(names(tools,\.display)).")
                    if model.notProtonCurrent {
                        StatusLine(.good,"\(Brand.name)’s options are in your games’ Compatibility settings, up to date.")
                    } else {
                        StatusLine(.warning,"\(Brand.name)’s options are in Steam, but this version of \(Brand.name) has newer files: choose Update.")
                    }
                }
            }
        }
    }
    private func names(_ tools:[NotProton.Tool],_ key:KeyPath<NotProton.Tool,String>)->String {
        tools.map { "\u{201C}\($0[keyPath:key])\u{201D}" }.joined(separator:" and ")
    }

    @ViewBuilder private var actions:some View {
        switch model.notProton {
        case .notInstalled:
            LauncherPrimaryAction(title:"Get NotProton") { NSWorkspace.shared.open(NotProton.projectURL) }
        case .unsupported:
            EmptyView()
        case .available:
            LauncherPrimaryAction(title:"Add to Steam",action:model.addToNotProton).keyboardShortcut(.defaultAction)
        case .added:
            if !model.notProtonCurrent {
                LauncherPrimaryAction(title:"Update",action:model.addToNotProton).keyboardShortcut(.defaultAction)
            }
            LauncherNotice(text:"In Steam, open a game’s Properties › Compatibility and keep NotProton’s CrossOver. Under Developer, choose \u{201C}\(NotProton.variantGroup)\u{201D} as the runner variant and turn on \u{201C}MacVolvo mouse\u{201D}, with High-precision if you like. Restart Steam after adding or removing \(Brand.name).",symbol:"gamecontroller")
            Button("Remove from Steam…",role:.destructive) { confirmRemove=true }
                .buttonStyle(.borderless).foregroundStyle(.red).font(LauncherStyle.font(13,.medium))
        }
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
                }.buttonStyle(.plain)
                if advanced {
                    HStack(alignment:.center,spacing:12) {
                        VStack(alignment:.leading,spacing:2) {
                            Text("\(Brand.name)’s own Steam").font(LauncherStyle.font(13,.semibold))
                            Text("For troubleshooting: Windows Steam in \(Brand.name)’s own Wine environment, instead of NotProton.")
                                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                        }
                        Spacer(minLength:12)
                        Button("Open…") { confirmWindowsSteam=true }.buttonStyle(.bordered).buttonBorderShape(.capsule).disabled(model.busy)
                    }
                }
            }
        }
    }
}
