// A stand-in for systemd-logind on the system bus of hint-writers-test.sh's
// container: gnome-shell does not start without the name. Answers what the
// shell asks at startup; everything else fails, which it only logs.
const {Gio, GLib} = imports.gi;
const xml = `<node>
<interface name="org.freedesktop.login1.Manager">
 <method name="GetSession"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
 <method name="GetSessionByPID"><arg type="u" direction="in"/><arg type="o" direction="out"/></method>
 <method name="GetUser"><arg type="u" direction="in"/><arg type="o" direction="out"/></method>
 <method name="ListSessions"><arg type="a(susso)" direction="out"/></method>
 <method name="CanSuspend"><arg type="s" direction="out"/></method>
 <method name="CanHibernate"><arg type="s" direction="out"/></method>
 <method name="CanPowerOff"><arg type="s" direction="out"/></method>
 <method name="CanReboot"><arg type="s" direction="out"/></method>
 <method name="CanRebootToBootLoaderMenu"><arg type="s" direction="out"/></method>
 <signal name="PrepareForSleep"><arg type="b"/></signal>
 <property name="IdleHint" type="b" access="read"/>
</interface>
<interface name="org.freedesktop.login1.Session">
 <property name="Active" type="b" access="read"/>
 <property name="Id" type="s" access="read"/>
 <property name="Class" type="s" access="read"/>
 <property name="Type" type="s" access="read"/>
 <property name="Remote" type="b" access="read"/>
 <property name="LockedHint" type="b" access="read"/>
 <signal name="Lock"/><signal name="Unlock"/>
 <method name="SetLockedHint"><arg type="b" direction="in"/></method>
</interface>
<interface name="org.freedesktop.login1.User">
 <property name="Display" type="(so)" access="read"/>
 <property name="Sessions" type="a(so)" access="read"/>
</interface></node>`;
const node = Gio.DBusNodeInfo.new_for_xml(xml);
const S = '/org/freedesktop/login1/session/c1';
const mgr = {
    GetSession: () => S, GetSessionByPID: () => S, GetUser: () => '/org/freedesktop/login1/user/_1000',
    ListSessions: () => [['c1', 1000, 't', 'seat0', S]],
    CanSuspend: () => 'no', CanHibernate: () => 'no', CanPowerOff: () => 'no', CanReboot: () => 'no',
    CanRebootToBootLoaderMenu: () => 'no', IdleHint: false,
};
const ses = {Active: true, Id: 'c1', Class: 'user', Type: 'wayland', Remote: false, LockedHint: false, SetLockedHint() {}};
const usr = {Display: ['c1', S], Sessions: [['c1', S]]};
const bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, null);
const objs = [];
for (const [o, iface, path] of [[mgr, 'Manager', '/org/freedesktop/login1'], [ses, 'Session', S],
                                [ses, 'Session', '/org/freedesktop/login1/session/auto'],
                                [usr, 'User', '/org/freedesktop/login1/user/_1000'],
                                [usr, 'User', '/org/freedesktop/login1/user/self']]) {
    const e = Gio.DBusExportedObject.wrapJSObject(node.lookup_interface('org.freedesktop.login1.' + iface), o);
    e.export(bus, path);
    objs.push(e);
}
Gio.bus_own_name_on_connection(bus, 'org.freedesktop.login1', 0, null, null);
new GLib.MainLoop(null, false).run();
