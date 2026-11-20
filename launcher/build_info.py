"""Which launcher build this is. The Launcher workflow rewrites this file before it builds the
.exe from main; a build made anywhere else (tools/build-launcher.cmd, a pull request) keeps
BUILD = 0, and such a launcher never replaces itself (see ddself.py)."""
BUILD = 0
COMMIT = ""
