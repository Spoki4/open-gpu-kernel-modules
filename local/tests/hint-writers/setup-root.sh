#!/bin/bash
# In the container, as root: the system bus with a stand-in logind (gnome-shell
# needs both) and the test user's runtime directory.
mkdir -p /run/dbus /run/t && chown t /run/t && chmod 700 /run/t
dbus-uuidgen --ensure
dbus-daemon --system --fork
gjs /src/tests/hint-writers/fake-logind.js >/tmp/fake-logind.log 2>&1 &
for i in $(seq 50); do
    dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 \
        org.freedesktop.login1.Manager.CanSuspend >/dev/null 2>&1 && exit 0
    sleep 0.1
done
echo "the stand-in logind did not start: $(cat /tmp/fake-logind.log)"
exit 1
