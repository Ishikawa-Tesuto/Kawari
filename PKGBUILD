# Local package recipe. Run makepkg from the Kawari source directory.
pkgname=kawari
pkgver=0.5h3
pkgrel=3
pkgdesc='Live2D avatar and livestreaming app with PipeWire video output'
arch=('x86_64')
# makepkg's debug path remapping uses RUSTFLAGS, which Cargo splits on spaces.
options=('!debug')
depends=(glibc gcc-libs vulkan-icd-loader vulkan-driver pipewire libdrm mesa
         wayland libxkbcommon libx11 libxrandr libxinerama libxcursor libxi libxext
         libpng giflib glib2 curl json-c)
makedepends=(rust cmake meson ninja git python pkgconf shaderc vulkan-headers
             wayland-protocols patchelf)
optdepends=('ttf-dejavu: Cyrillic text in the studio interface'
            'xdg-utils: open Twitch authorization in a browser'
            'xdg-desktop-portal: experimental global hotkeys on Wayland')

build() {
    cd "$startdir"
    ./scripts/build.sh
    ./scripts/build-twitch.sh
    ./scripts/build-global-shortcuts.sh
}

package() {
    local runtime="$pkgdir/usr/lib/kawari"
    local renderer="$runtime/native/bin/Demo"
    local build_dir="$startdir/build"
    local release_dir="$startdir/api-rust/target/release"
    local library soname object needed size
    local app_id=ru.tesutoidclub.Kawari

    install -dm755 "$runtime/native/bin" "$pkgdir/usr/bin"
    install -m755 -t "$runtime" "$release_dir/studio" "$release_dir/studio-vts-api" \
        "$release_dir/kawari-global-shortcuts" "$build_dir/kawari-twitch"
    cp -RL -- "$build_dir/native/bin/." "$runtime/native/bin/"

    if [[ ! -r "$renderer/Resources/Haru/Haru.model3.json" &&
          -r "$build_dir/sdk/Samples/Resources/Haru/Haru.model3.json" ]]; then
        install -dm755 "$renderer/Resources"
        cp -RL -- "$build_dir/sdk/Samples/Resources/Haru" "$renderer/Resources/"
    fi
    for library in libfunnel.so libfunnel-vk.so; do
        install -m755 "$build_dir/funnel/$library" "$renderer/$library"
        soname="$(patchelf --print-soname "$renderer/$library")" || return 1
        soname="${soname##*/}"
        if [[ -n "$soname" && "$soname" != "$library" ]]; then
            ln -s "$library" "$renderer/$soname"
        fi
    done
    for object in "$renderer/Demo" "$renderer/libfunnel.so" "$renderer/libfunnel-vk.so"; do
        # A library without a SONAME can be recorded by its absolute build path.
        needed="$(patchelf --print-needed "$object")" || return 1
        while IFS= read -r library; do
            case "$library" in
                */libfunnel.so*|*/libfunnel-vk.so*)
                    patchelf --replace-needed "$library" "${library##*/}" "$object" || return 1 ;;
            esac
        done <<< "$needed"
        patchelf --set-rpath '$ORIGIN' "$object" || return 1
    done

    ln -s ../lib/kawari/studio "$pkgdir/usr/bin/kawari"
    install -Dm644 "$startdir/packaging/$app_id.desktop" "$pkgdir/usr/share/applications/$app_id.desktop"
    for size in 16 32 48 64 128 256; do
        install -Dm644 "$startdir/assets/icons/$size.png" \
            "$pkgdir/usr/share/icons/hicolor/${size}x${size}/apps/$app_id.png"
    done
}
