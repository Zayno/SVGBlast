# resvg build

`resvg.lib` is resvg **0.49.0** (C API, static library) with the patches in this folder applied.

| Patch | Why |
|---|---|
| `0001-clamp-layers-to-visible-area.patch` | Layers of groups without filters (opacity, clip paths, masks, blend modes) are clamped to the visible pixmap instead of 5x5 the canvas. At deep zoom nearly every such group covers the whole view, so this avoids allocating and compositing layers 25x the view size. Output is identical apart from rare anti-aliasing differences of a few levels on edge pixels. |

## Rebuilding

Needs the Rust MSVC toolchain (`winget install Rustlang.Rustup`).

```bash
curl -LO https://github.com/linebender/resvg/releases/download/v0.49.0/resvg-0.49.0.tar.xz
tar xf resvg-0.49.0.tar.xz
cd resvg-0.49.0
patch -p1 < ../0001-clamp-layers-to-visible-area.patch
RUSTFLAGS="-C target-feature=+crt-static" cargo build --release --manifest-path crates/c-api/Cargo.toml
```

`+crt-static` matches the app's `/MT` runtime. Copy `target/release/resvg.lib` and `crates/c-api/resvg.h`
(with CRLF line endings) into `SVGBlastProj/`.
