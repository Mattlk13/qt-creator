add_qtc_plugin(Docker
  DEPENDS Utils CmdBridgeClient
  PLUGIN_DEPENDS Core ProjectExplorer QtSupport
  SOURCES
    docker_global.h
    dockertr.h
    dockerapi.cpp dockerapi.h
    dockerconstants.h
    dockercontainerthread.cpp
    dockercontainerthread.h
    dockerdevice.cpp dockerdevice.h
    dockerdevicewidget.cpp dockerdevicewidget.h
    dockerdeviceenvironmentaspect.cpp dockerdeviceenvironmentaspect.h
    dockerplugin.cpp
    dockersettings.cpp dockersettings.h
)
