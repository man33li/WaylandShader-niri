fn main() {
    for name in [
        "WAYLANDSHADER_BRIDGE_LIB_DIR",
        "WAYLANDSHADER_RASHADER_LIB_DIR",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
        let directory = std::env::var(name).expect("build with python3 waylandshader/build.py");
        println!("cargo:rustc-link-search=native={directory}");
        let library = if name == "WAYLANDSHADER_BRIDGE_LIB_DIR" {
            "libwaylandshader-niri-bridge.a"
        } else {
            "libwaylandshader-rashader.so.2"
        };
        println!("cargo:rerun-if-changed={directory}/{library}");
    }
    println!("cargo:rustc-link-lib=static=waylandshader-niri-bridge");
    println!("cargo:rustc-link-lib=dylib=waylandshader-rashader");
    println!("cargo:rustc-link-lib=dylib=epoxy");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-arg-bin=niri=-Wl,-rpath,$ORIGIN/../lib/waylandshader");
    println!("cargo:rustc-check-cfg=cfg(have_libinput_plugin_system)");
    if pkg_config::Config::new()
        .atleast_version("1.30.0")
        .probe("libinput")
        .is_ok()
    {
        println!("cargo:rustc-cfg=have_libinput_plugin_system")
    }
}
