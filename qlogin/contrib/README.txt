contrib/ = examples only. Nothing here is installed by this project and none of it has been run on a real system.
qlogind refuses to start unless DBUS_SYSTEM_BUS_ADDRESS is set, so every service example below sets it explicitly.
Do NOT enable qlogind on a machine that already runs systemd-logind or elogind: both want the bus name org.freedesktop.login1,
and qlogind asks for it with DO_NOT_QUEUE, so it would simply fail to start - but the policy file and the PAM file are still yours to review first.
