# Woby app icon

`woby.png` is the original selected D artwork (blue and orange layered surfaces),
generated with the built-in imagegen tool. Its composition and dark background
are preserved. `woby.ico` contains Windows sizes from 16 to 256 pixels;
`woby.icns` supplies the macOS bundle icon. `assets/icons/woby.bmp` is the
256-pixel SDL window icon, using BMP for compatibility with SDL 3.2 and later.

All platform icon files are stored directly in the repository and consumed by
the build. No icon generation step, Python script, or Pillow dependency is
needed. Icon packaging is checked by the C++ test suite.
