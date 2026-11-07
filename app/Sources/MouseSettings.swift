import SwiftUI
import AppKit

/// How games in MacVolvo's own Windows Steam get the mouse (Settings; AppModel.MouseInput, from
/// Recall 1.6): Standard, Classic or High-precision, and how often movement is sent. That Steam
/// runs as part of MacVolvo, so MacVolvo holds Input Monitoring and asks macOS itself; with
/// High-precision this says whether it has it and a mouse is connected. In NotProton's Steam the
/// same choices are per game in Steam (NotProton.swift) and Steam holds the permission
/// (SteamMouseStatus).
struct MousePanel:View {
    @Bindable var model:AppModel
    @State private var appAccess=InputMonitoring.access
    @State private var mouseConnected=InputMonitoring.mouseConnected
    @State private var explainHighPrecision=false
    @State private var advanced=false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    /// MacVolvo's own Steam takes the mouse settings when it opens, so they wait until it's closed.
    private var locked:Bool { model.running != nil }
    var body:some View {
        LauncherPanel(inset:16) {
            VStack(alignment:.leading,spacing:6) {
                VStack(alignment:.leading,spacing:2) {
                    Text("Mouse").font(LauncherStyle.font(14,.semibold))
                    Text("How your games get your mouse’s movement and clicks.").font(LauncherStyle.font(12))
                        .foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                VStack(alignment:.leading,spacing:10) {
                    MouseChoice(title:"Standard",detail:"Your game gets your mouse from macOS, with each click after the movement before it. No permission needed.",
                                selected:model.mouseInput == .standard,disabled:locked) { model.mouseInput = .standard }
                    MouseChoice(title:"Classic",detail:"The older mouse timing. Try it if Standard feels different to you.",
                                selected:model.mouseInput == .classic,disabled:locked) { model.mouseInput = .classic }
                    MouseChoice(title:"High-precision",detail:"Your game reads your mouse directly, skipping macOS’s mouse layer. Needs Input Monitoring.",
                                selected:model.mouseInput == .highPrecision,disabled:locked) {
                        if model.mouseInput != .highPrecision { explainHighPrecision=true }
                    }
                }.padding(.top,8)
                if model.mouseInput == .highPrecision { status.padding(.top,6) }
                rate
                if let running=model.running {
                    Text(running == .game ? "Quit your game to change the mouse." : "Close Steam to change the mouse.")
                        .font(LauncherStyle.font(12,.medium)).padding(.top,6)
                }
            }
        }
        .onAppear(perform:refresh)
        .onReceive(NotificationCenter.default.publisher(for:NSApplication.didBecomeActiveNotification)) { _ in refresh() }
        .alert("Use the High-precision mouse?",isPresented:$explainHighPrecision) {
            Button("Cancel",role:.cancel) {}
            Button("Turn On") { model.turnOnHighPrecision();refresh() }
        } message: { Text("Your game reads your mouse’s own reports instead of macOS’s mouse input for games, which drops some of them in game. macOS will ask to allow Input Monitoring for \(Brand.name). Its prompt mentions keystrokes, but only mice are opened, and only their movement and buttons are read. Until you allow it, the mouse works as Standard does.") }
    }
    private func refresh() {
        appAccess=InputMonitoring.access;mouseConnected=InputMonitoring.mouseConnected
    }

    // MARK: High-precision status
    @ViewBuilder private var status:some View {
        if appAccess != .allowed {
            StatusLine(.problem,"Needs Input Monitoring. Turn on \(Brand.name) in System Settings › Privacy & Security › Input Monitoring. Until then, the mouse works as Standard does.",
                       button:appAccess == .undecided ? "Allow…" : "Open System Settings") {
                // Undecided: macOS's own prompt, which also adds MacVolvo to the list. Decided: only the player can change it.
                if appAccess == .undecided { InputMonitoring.ask() } else { NSWorkspace.shared.open(InputMonitoring.privacySettings) }
                refresh()
            }
        } else if !mouseConnected {
            StatusLine(.warning,"\(Brand.name) has Input Monitoring, but no mouse is connected. A trackpad works as Standard does.")
        } else {
            StatusLine(.good,"High-precision mouse is working: \(Brand.name) has Input Monitoring and reads your mouse directly.")
        }
    }
    // MARK: Mouse updates
    /// Mouse › Advanced (AppModel.MouseRate), closed until opened: how many times a second the game
    /// gets the mouse's movement, with All at the slider's right end. Classic keeps its own timing.
    private var rate:some View {
        let stops=AppModel.MouseRate.stops,rate=model.mouseRate
        let classic=model.mouseInput == .classic
        let caption=rate == .all ? "Your game gets every update your mouse sends, at its full polling rate, as on a Windows PC."
            : "Your game gets up to \((rate.perSecond ?? 0).formatted()) updates a second. Movement in between is added to the next one, so none is lost."
        return VStack(alignment:.leading,spacing:10) {
            Divider().padding(.bottom,-2)
            Button { withAnimation(reduceMotion ? nil : .easeOut(duration:0.15)) { advanced.toggle() } } label: {
                HStack(spacing:6) {
                    Text("Advanced").font(LauncherStyle.font(12,.medium))
                    Image(systemName:"chevron.down").font(.system(size:10,weight:.semibold)).rotationEffect(.degrees(advanced ? 180 : 0))
                    if !advanced && !classic && rate != .all {
                        Text(verbatim:"·").font(LauncherStyle.font(12))
                        Text(label(rate)).font(LauncherStyle.font(12)).monospacedDigit()
                    }
                }.foregroundStyle(.secondary).contentShape(Rectangle())
            }.buttonStyle(.plain)
                .accessibilityHint(advanced ? "Hides the advanced mouse setting" : "Shows how often your game gets your mouse’s movement")
            if advanced {
                if classic {
                    Text("Classic keeps its own timing, so this setting doesn’t change it.")
                        .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                } else {
                    VStack(alignment:.leading,spacing:8) {
                        HStack(alignment:.center,spacing:12) {
                            VStack(alignment:.leading,spacing:2) {
                                Text("Mouse updates").font(LauncherStyle.font(13,.semibold))
                                Text("How many times a second your game gets your mouse’s movement.").font(LauncherStyle.font(12))
                                    .foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                            }
                            Spacer(minLength:12)
                            Text(label(rate)).font(LauncherStyle.font(13,.medium)).monospacedDigit()
                        }.accessibilityElement(children:.combine)
                        HStack(spacing:10) {
                            Text("Fewer").font(LauncherStyle.font(12)).foregroundStyle(.secondary).accessibilityHidden(true)
                            Slider(value:Binding(get:{ Double(stops.firstIndex(of:model.mouseRate) ?? stops.count-1) },
                                                 set:{ model.mouseRate=stops[min(stops.count-1,max(0,Int($0.rounded())))] }),
                                   in:0...Double(stops.count-1),step:1) {
                                Text("Mouse updates")
                            }.labelsHidden().tint(LauncherStyle.accent).accessibilityValue(label(rate))
                            Text("All").font(LauncherStyle.font(12)).foregroundStyle(.secondary).accessibilityHidden(true)
                        }.disabled(locked)
                        Text(caption).font(LauncherStyle.font(13)).lineSpacing(3).fixedSize(horizontal:false,vertical:true)
                        Text("Lower this only if your game runs slower while you move your mouse.")
                            .font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                    }
                }
            }
        }.padding(.top,8)
    }
    private func label(_ rate:AppModel.MouseRate) -> String {
        rate.perSecond.map { "\($0.formatted()) a second" } ?? "Every update"
    }
}

/// The High-precision mouse in NotProton's Steam, as MacVolvo's driver reported it in the latest
/// game that used it (NotProton.MouseStatus): macOS asks Steam for Input Monitoring, which started
/// the game, and only the game can tell whether Steam has it. MacVolvo never asks for itself here.
struct SteamMouseStatus:View {
    @Bindable var model:AppModel
    var body:some View {
        if let report=model.mouseStatus {
            let when="in \(report.process), \(report.date.formatted(.relative(presentation:.named)))"
            if report.access != "granted" {
                StatusLine(.problem,"Steam needs Input Monitoring (\(when)). Turn on Steam in System Settings › Privacy & Security › Input Monitoring, then quit and reopen Steam. Until then, the mouse works as Standard does.",
                           button:"Open System Settings") { NSWorkspace.shared.open(InputMonitoring.privacySettings) }
            } else if report.reporting {
                StatusLine(.good,"High-precision mouse is working: your game reads your mouse directly (\(when)).")
            } else if report.mice > 0 {
                StatusLine(.good,"Steam has Input Monitoring and your mouse is open (\(when)). Move it in the game to confirm.")
            } else {
                StatusLine(.warning,"Steam has Input Monitoring, but no mouse was connected (\(when)). A trackpad works as Standard does.")
            }
        } else {
            StatusLine(.pending,"No game has used High-precision yet. In Steam, choose MacVolvo driver and turn on MacVolvo mouse › High-precision for a game, then start it: macOS asks to allow Input Monitoring for Steam then, since Steam starts your games. Steam only appears in System Settings after that, and it works from the next launch. This shows how it went.")
        }
    }
}

/// One line of status: a coloured symbol, the text, and an optional button.
struct StatusLine:View {
    enum Kind { case good, warning, problem, pending }
    let kind:Kind
    let text:String
    var button:String?=nil
    var action:()->Void={}
    init(_ kind:Kind,_ text:String,button:String?=nil,action:@escaping ()->Void={}) {
        self.kind=kind;self.text=text;self.button=button;self.action=action
    }
    var body:some View {
        HStack(alignment:.top,spacing:10) {
            Image(systemName:symbol).font(.system(size:14,weight:.semibold)).foregroundStyle(color).frame(width:18).accessibilityHidden(true)
            VStack(alignment:.leading,spacing:8) {
                Text(text).font(LauncherStyle.font(12,.medium)).fixedSize(horizontal:false,vertical:true)
                if let button { Button(button,action:action).buttonStyle(.bordered).buttonBorderShape(.capsule).controlSize(.small) }
            }
            Spacer(minLength:0)
        }.accessibilityElement(children:.combine)
    }
    private var symbol:String {
        switch kind {
        case .good: return "checkmark.circle.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .problem: return "xmark.circle.fill"
        case .pending: return "circle.dashed"
        }
    }
    private var color:Color {
        switch kind {
        case .good: return .green
        case .warning: return .orange
        case .problem: return .red
        case .pending: return .secondary
        }
    }
}

/// One of the mouse's choices: a radio button with its name over an explanation.
struct MouseChoice:View {
    let title:String
    let detail:String
    let selected:Bool
    let disabled:Bool
    let choose:()->Void
    var body:some View {
        Button(action:choose) {
            HStack(alignment:.firstTextBaseline,spacing:10) {
                Image(systemName:selected ? "largecircle.fill.circle" : "circle").font(.system(size:14))
                    .foregroundStyle(selected ? LauncherStyle.accent : .secondary).accessibilityHidden(true)
                VStack(alignment:.leading,spacing:2) {
                    Text(title).font(LauncherStyle.font(13,.semibold))
                    Text(detail).font(LauncherStyle.font(12)).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                Spacer(minLength:0)
            }.contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(disabled).opacity(disabled ? 0.6 : 1)
            .accessibilityElement(children:.combine).accessibilityAddTraits(selected ? .isSelected : [])
    }
}
