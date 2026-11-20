#!/bin/bash
# Double-click in Finder to open the LiPo Editor in your browser. Close this window to stop it.
cd "$(dirname "$0")" && exec python3 tools/lipo_editor.py
