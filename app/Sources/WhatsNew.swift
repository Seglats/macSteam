// Offline release notes, also available from Help.
enum WhatsNew {
    static let version="1.0"
    static let items:[(symbol:String, title:String, detail:String)]=[
        ("gamecontroller","SteamDriver for every game",
         "macSteam’s own Wine, offered in macOS Steam as a compatibility tool: Apple D3DMetal from your Game Porting Toolkit, key combo overrides and the High-precision mouse. Needs NotProton and a licensed CrossOver."),
        ("cursorarrow.motionlines","Lower input latency",
         "Measured end to end in Counter-Strike 2: mouse 35 → 20 ms and keyboard 32 → 25 ms compared with CrossOver."),
        ("slider.horizontal.3","Profiles and options in Steam",
         "Global DX11, Global DX12 and per-game profiles, plus the SteamDriver driver, renderer and mouse options on NotProton’s CrossOver."),
        ("headphones","Headphone surround (experimental)",
         "5.1 and 7.1 game sound on headphones, with a test and live settings you can tune and save."),
        ("gamecontroller.fill","More controller support (experimental)",
         "Controllers in Xbox mode, such as 8BitDo pads, reach SteamDriver games directly at their full rate."),
    ]
}
