#!/bin/bash
exec >~/setup-desktop.log 2>&1
set -x
export DEBIAN_FRONTEND=noninteractive
sudo apt-get install -y -q --no-install-recommends kwin-wayland plasma-workspace plasma-desktop qt6-tools-dev-tools qdbus-qt6 bubblewrap dbus-user-session dbus-x11 curl python3 psmisc xwayland fonts-dejavu-core konsole || echo "SETUPD FAIL apt"
cd ~ && curl -fsSLO https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb && sudo apt-get install -y -q ./google-chrome-stable_current_amd64.deb || echo "SETUPD FAIL chrome"
which kwin_wayland plasmashell qdbus6 bwrap google-chrome
echo SETUPD DONE
