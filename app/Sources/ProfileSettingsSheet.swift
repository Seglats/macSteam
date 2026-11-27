import SwiftUI

/// A profile's renderer settings, saved as they change: a DX11 profile's SteamDriver DXMT settings
/// (Profiles.Settings, dxmt.conf; Steam's rows stay as they are), or a DX12 profile's D3DMetal
/// settings (Profiles.D3DMetalSettings, d3dmetal.conf, read by the SteamDriver tool). They apply
/// from the game's next launch.
struct ProfileSettingsSheet:View {
    @Bindable var model:AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var settings=Profiles.Settings()
    @State private var metal=Profiles.D3DMetalSettings()
    @State private var loaded=false
    @State private var notice=""
    @State private var ownRuntime=false
    private var profile:Profiles.Profile? { Profiles.profile(model.editingProfile) }
    private var isDX12:Bool { profile?.kind == .dx12 }
    private static let caps=[0,30,60,90,120,144,165,240]

    var body:some View {
        VStack(spacing:16) {
            LauncherHeading(title:profile?.name ?? "Profile",subtitle:"",compact:true)
            Text(intro).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                .frame(maxWidth:.infinity,alignment:.leading)
            if isDX12 { d3dmetalPanel } else { dxmtPanel }
            if model.surroundOn { surroundPanel }
            if model.customRuntimes,let profile,!profile.isGlobal { runtimePanel(profile) }
            if !notice.isEmpty { LauncherNotice(text:notice) }
            Text("Changes apply the next time the game starts.").font(LauncherStyle.font(11)).foregroundStyle(.secondary)
                .frame(maxWidth:.infinity,alignment:.leading)
            HStack {
                if !isDX12 {
                    Button("Reset to \(Brand.name)’s") { reset() }.buttonStyle(.borderless).hoverGrow().font(LauncherStyle.font(12,.medium))
                        .help(profile?.appID.flatMap { Profiles.tuned[$0] } != nil ? "The settings \(Brand.name) tuned for this game" : "The settings \(Brand.name) brings for DX11 games")
                }
                Spacer()
                Button("Done") { dismiss() }.buttonStyle(.bordered).hoverGrow().buttonBorderShape(.capsule).controlSize(.large)
                    .keyboardShortcut(.cancelAction)
            }
        }.font(LauncherStyle.font(14)).padding(.horizontal,32).padding(.vertical,28).frame(width:560)
            .tint(.primary).background { LauncherBackground() }
            .onAppear { load() }
            .onChange(of:settings) { if loaded { save() } }
            .onChange(of:metal) { if loaded { save() } }
    }

    private var intro:String {
        let global=profile?.isGlobal ?? true, name=profile?.name ?? "this game"
        if isDX12 {
            return global ? "D3DMetal’s settings for every DX12 game that runs with the SteamDriver compatibility tool and has no game profile of its own." :
                "D3DMetal’s settings for \(name) when it runs with the SteamDriver compatibility tool, used instead of Global DX12."
        }
        return global ? "SteamDriver DXMT’s settings for every DX11 game that has \u{201C}SteamDriver DX11 profile\u{201D} switched on in its Compatibility settings." :
            "SteamDriver DXMT’s settings for \(name), used instead of Global DX11 when its row is switched on."
    }

    private var d3dmetalPanel:some View {
        LauncherPanel(inset:14) {
            VStack(alignment:.leading,spacing:14) {
                setting("MetalFX upscaling (DLSS)",
                        "Games that offer NVIDIA DLSS can turn it on in their graphics settings; Apple’s MetalFX does the upscaling. For this, D3DMetal presents an NVIDIA graphics card to the game.") {
                    Toggle("",isOn:$metal.upscaling).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                }
            }
        }
    }

    private var dxmtPanel:some View {
            LauncherPanel(inset:14) {
                VStack(alignment:.leading,spacing:14) {
                    setting("Frame latency","How many frames the game may prepare ahead of the screen. Fewer means less input lag; more can smooth an uneven frame rate.") {
                        Picker("",selection:$settings.frameLatency) {
                            Text("1 frame").tag(1);Text("2 frames").tag(2);Text("3 frames").tag(3);Text("Game decides").tag(16)
                        }.labelsHidden().fixedSize()
                    }
                    setting("Late input sampling","Waits through most of each frame’s spare time before the game starts the next one, so it reads your input later. Lowered latency by 16 % in play-tests.") {
                        Toggle("",isOn:$settings.lateInput).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
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
                        Toggle("",isOn:$settings.shaderNormalPriority).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                    }
                    setting("Sharp scaling","Sharper picture when the game’s resolution is scaled to the window or screen.") {
                        Toggle("",isOn:$settings.sharpScaling).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                    }
                    setting("MetalFX upscaling (DLSS)",
                            "With the SteamDriver compatibility tool: games that offer NVIDIA DLSS can turn it on, and Apple’s MetalFX does the upscaling. The game stays on DXMT with the settings above; DXMT presents an NVIDIA graphics card for it.") {
                        Toggle("",isOn:$settings.upscaling).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                    }
                    setting("Free shader memory","Drops each shader’s intermediate code once it’s compiled. Off only for troubleshooting.") {
                        Toggle("",isOn:$settings.releaseShaderIR).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
                    }
                }
            }
    }

    /// Experimental › Headphone surround on: this profile can use it (Surround.swift).
    private var surroundPanel:some View {
        LauncherPanel(inset:14) {
            setting("Headphone surround",
                    "With the SteamDriver compatibility tool: your headphones appear to the game as 7.1 speakers, and the game’s 5.1 or 7.1 sound is turned into 3D sound for headphones, so you can hear what’s behind you. Stereo sound stays as it is: if the game has its own audio option, choose 7.1 or surround, not Stereo or Headphones. Only when the Mac’s output is headphones or other stereo output.") {
                Toggle("",isOn:isDX12 ? $metal.surround : $settings.surround).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
            }
        }
    }

    /// Advanced › Custom runtimes on: this game can have its own runtime copy (Runtimes.swift).
    private func runtimePanel(_ profile:Profiles.Profile)->some View {
        LauncherPanel(inset:14) {
            setting("Own runtime copy",
                    "\(profile.name) gets a Wine prefix of its own, built from scratch, instead of a copy of the shared one. It takes about 400 MB more and 15 seconds when it’s made. It applies when the game’s prefix is made, so a game that already has one keeps it.") {
                Toggle("",isOn:Binding(get:{ ownRuntime },set:{ on in
                    do { try Runtimes.setOwnCopy(profile.id,on);ownRuntime=on } catch { notice=error.localizedDescription }
                })).labelsHidden().toggleStyle(.switch).hoverGrow().tint(LauncherStyle.accent)
            }
        }
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
        ownRuntime=Runtimes.ownCopy(model.editingProfile)
        if isDX12 { metal=Profiles.d3dmetalSettings(model.editingProfile);notice="" }
        else if let s=Profiles.settings(model.editingProfile) { settings=s;notice="" }
        else { notice="This profile has no renderer settings." }
        DispatchQueue.main.async { loaded=true }
    }
    private func save() {
        do {
            if isDX12 { try Profiles.saveD3DMetal(model.editingProfile,metal) }
            else { try Profiles.save(model.editingProfile,settings) }
            notice="";model.checkNotProton()
        } catch { notice="Couldn’t save: \(error.localizedDescription)" }
    }
    private func reset() {
        do { try Profiles.resetSettings(model.editingProfile,resources:model.resources);load();model.checkNotProton() }
        catch { notice=error.localizedDescription }
    }
}
