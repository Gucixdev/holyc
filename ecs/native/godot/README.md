# Godot binding (GDExtension)

`gdext/` wraps the C core as a Godot 4 GDExtension: `EcsWorld`, `EcsView` and the stats,
event, scheduler and streaming modules. The C sources in `../src` are compiled straight
into the shared library, so there is nothing to install besides godot-cpp.

## Build

```sh
python -m venv .venv && .venv/bin/pip install scons
git clone https://github.com/godotengine/godot-cpp godot-cpp
git -C godot-cpp checkout b051e69        # the commit the binaries were built against
cd gdext

../.venv/bin/scons platform=linux target=template_debug
../.venv/bin/scons platform=linux target=template_release
# Windows, cross-compiled from Linux (needs mingw-w64):
../.venv/bin/scons platform=windows use_mingw=yes target=template_release
```

The libraries land in `gdext/bin/`. Copy them into `../../bin/` (the tool folder), where
`ecs.gdextension` already points at them.

macOS is not set up: it needs Apple's SDK to build from Linux.
