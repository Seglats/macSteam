"""The Apple developer folder for builds that run under Rosetta (arch -x86_64).

Wine and its native dependencies are x86_64 and configure them in Intel mode. There,
/usr/bin/clang loads the developer folder's libxcrun as x86_64; macOS 27's Command
Line Tools ship it arm64-only, while Xcode's still has the Intel slice. The SDK stays
the Command Line Tools' macOS 26.5 SDK either way.
"""
import subprocess

COMMAND_LINE_TOOLS = '/Library/Developer/CommandLineTools'
XCODE = '/Applications/Xcode.app/Contents/Developer'
SDKROOT = COMMAND_LINE_TOOLS + '/SDKs/MacOSX26.5.sdk'


def developer_dir():
    for folder in (COMMAND_LINE_TOOLS, XCODE):
        archs = subprocess.run(['/usr/bin/lipo', '-archs', folder + '/usr/lib/libxcrun.dylib'],
                               capture_output=True, text=True).stdout.split()
        if 'x86_64' in archs:
            return folder
    raise RuntimeError('No developer folder can run clang under Rosetta; install Xcode')
