link /subsystem:console /entry:mainCRTStartup ^
hello.obj logger.obj pch.obj ^
libcmtd.lib libcpmtd.lib libvcruntimed.lib libucrtd.lib ^
kernel32.lib shell32.lib ^
/NODEFAULTLIB:ucrtd.lib /NODEFAULTLIB:vcruntimed.lib /NODEFAULTLIB:msvcrtd.lib /NODEFAULTLIB:msvcprtd.lib
