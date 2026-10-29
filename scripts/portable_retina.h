/* Wine's Retina mode is prefix-wide and doubles every display once any is Retina:
 * a 3840x1600 monitor becomes a 7680x3200 desktop, which Unreal's windowed fullscreen
 * then renders at four times the pixels. MacVolvo needed it for Recall's DXMT canvas;
 * D3DMetal has no canvas, so MacFinals keeps it off, as CrossOver does by default.
 * Installations that have it on are switched off, only while idle. */
static NSString *retinaRegistry(void) {
    NSString *path=join(root,@"environment/user.reg");struct stat st;
    if(lstat(path.fileSystemRepresentation,&st) || !S_ISREG(st.st_mode) || st.st_size>(8<<20))
        fail(@"retina_configuration_failed");
    NSString *text=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if(!text)fail(@"retina_configuration_failed");return text;
}
static BOOL retinaEnabled(void) {
    BOOL section=NO;
    for(NSString *raw in [retinaRegistry() componentsSeparatedByString:@"\n"]) {
        NSString *line=[raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if([line hasPrefix:@"["])
            section=[line hasPrefix:@"[Software\\\\Wine\\\\Mac Driver]"];
        else if(section && [line isEqual:@"\"RetinaMode\"=\"Y\""])return YES;
    }
    return NO;
}
static void configureRetina(void) {
    if(!retinaEnabled())return;
    NSString *engine=runtime();
    // Wine caches desktop geometry in the running session. Never change the
    // registry underneath a client/game, or kill one to perform this migration.
    if(sessionProcesses().count || busy(engine))fail(@"close_game_before_setup");
    mkdirs(join(root,@"settings-backup"));
    NSString *backup=join(root,@"settings-backup/before-retina-off-user.reg");
    if(![fm fileExistsAtPath:backup] &&
       ![fm copyItemAtPath:join(root,@"environment/user.reg") toPath:backup error:nil])
        fail(@"settings_backup_failed");
    ownedPreparation=YES;
    if(waitTask(task(join(engine,@"bin/wine"),
        @[@"reg.exe",@"add",@"HKCU\\Software\\Wine\\Mac Driver",@"/v",@"RetinaMode",@"/t",@"REG_SZ",@"/d",@"N",@"/f"],
        wineEnv(engine),@"retina-configuration.log"),60))fail(@"retina_configuration_failed");
    // The registry tool itself starts Wine with the old geometry. Wait for its
    // server to exit and flush the registry before starting Steam afresh.
    if(waitTask(task(join(engine,@"bin/wineserver"),@[@"-w"],wineEnv(engine),@"retina-wait.log"),60))
        fail(@"environment_initialization_busy");
    ownedPreparation=NO;
    if(retinaEnabled())fail(@"retina_configuration_failed");
    event(@"retina_disabled",nil);
}
