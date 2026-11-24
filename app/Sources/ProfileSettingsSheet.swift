import SwiftUI

/// A DX11 profile's SteamDriver DXMT settings (Profiles.Settings), saved as they change into the
/// profile's dxmt.conf. They apply from the game's next launch; Steam's rows stay as they are.
struct ProfileSettingsSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var settings=Profiles.Settings()
    @State private var loaded=false
    @State private var notice=""
    private var profile:Profiles.Profile? { Profiles.profile(model.editingProfile) }
    private static let caps=[0,30,60,90,120,144,165,240]

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:profile?.name ?? "Profile",subtitle:"",compact:true)
            Text(profile?.isGlobal ?? true ? "SteamDriver DXMT’s settings for every DX11 game that has \u{201C}SteamDriver DX11 profile\u{201D} switched on in its Compatibility settings." :
                    "SteamDriver DXMT’s settings for \(profile?.name ?? "this game"), used instead of Global DX11 when its row is switched on.")
                .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            LauncherPanel(inset:14) {
                VStack(alignment:.leading,spacing:14) {
                    setting("Frame latency","How many frames the game may prepare ahead of the screen. Fewer means less input lag; more can smooth an uneven frame rate.") {
                        Picker("",selection:$settings.frameLatency) {
                            Text("1 frame").tag(1);Text("2 frames").tag(2);Text("3 frames").tag(3);Text("Game decides").tag(16)
                        }.labelsHidden().fixedSize()
                    }
                    setting("Late input sampling","Waits through most of each frame’s spare time before the game starts the next one, so it reads your input later. Lowered latency by 16 % in play-tests.") {
                        Toggle("",isOn:$settings.lateInput).labelsHidden().toggleStyle(.switch).tint(LauncherStyle.accent)
                    }
                    setting("Frame-rate cap","At most this many frames a second.") {
                        Picker("",selection:$settings.frameCap) {
                            ForEach(Self.caps+(Self.caps.contains(settings.frameCap) ? [] : [settings.frameCap]),id:\.self) { cap in
                                Text(cap==0 ? "None" : "\(cap) fps").tag(cap)
                            }
                        }.labelsHidden().fixedSize()
                    }
                    setting("Shader compiler threads","Threads that compile shaders while you play. More helps stutter in shader-heavy moments, at some CPU cost.") {
                        Stepper(settings.shaderThreads==0 ? "Automatic" : "\(settings.shaderThreads)",value:$settings.shaderThreads,in:0...16)
                            .font(LauncherStyle.font(13,.medium)).fixedSize()
                    }
                    setting("Compile at normal priority","Shader compiling yields to the game instead of running at the highest priority.") {
                        Toggle("",isOn:$settings.shaderNormalPriority).labelsHidden().toggleStyle(.switch).tint(LauncherStyle.accent)
                    }
                    setting("Sharp scaling","Sharper picture when the game’s resolution is scaled to the window or screen.") {
                        Toggle("",isOn:$settings.sharpScaling).labelsHidden().toggleStyle(.switch).tint(LauncherStyle.accent)
                    }
                    setting("Free shader memory","Drops each shader’s intermediate code once it’s compiled. Off only for troubleshooting.") {
                        Toggle("",isOn:$settings.releaseShaderIR).labelsHidden().toggleStyle(.switch).tint(LauncherStyle.accent)
                    }
                }
            }
            if !notice.isEmpty { LauncherNotice(text:notice) }
            Text("Changes apply the next time the game starts.").font(LauncherStyle.font(11)).foregroundStyle(.secondary)
                .frame(maxWidth:.infinity,alignment:.leading)
            HStack {
                Button("Reset to \(Brand.name)’s") { reset() }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                    .help(profile?.appID.flatMap { Profiles.tuned[$0] } != nil ? "The settings \(Brand.name) tuned for this game" : "The settings \(Brand.name) brings for DX11 games")
                Spacer()
                Button("Done") { dismiss() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.large)
                    .keyboardShortcut(.cancelAction)
            }
        }.font(LauncherStyle.font(14)).padding(.horizontal,32).padding(.vertical,28).frame(width:560)
            .tint(.primary).background { LauncherBackground() }
            .onAppear { load() }
            .onChange(of:settings) { if loaded { save() } }
    }

    private func setting<Control:View>(_ title:String,_ detail:String,@ViewBuilder control:()->Control)->some View {
        HStack(alignment:.center,spacing:14) {
            VStack(alignment:.leading,spacing:2) {
                Text(title).font(LauncherStyle.font(13,.semibold))
                Text(detail).font(LauncherStyle.font(11)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
            }
            Spacer(minLength:12)
            control()
        }
    }
    private func load() {
        loaded=false
        if let s=Profiles.settings(model.editingProfile) { settings=s;notice="" }
        else { notice="This profile has no renderer settings." }
        DispatchQueue.main.async { loaded=true }
    }
    private func save() {
        do { try Profiles.save(model.editingProfile,settings);notice="";model.checkNotProton() }
        catch { notice="Couldn’t save: \(error.localizedDescription)" }
    }
    private func reset() {
        do { try Profiles.resetSettings(model.editingProfile,resources:model.resources);load();model.checkNotProton() }
        catch { notice=error.localizedDescription }
    }
}
