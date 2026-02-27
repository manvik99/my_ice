use std::env;
use std::path::PathBuf;
use std::process::Command;

fn run(cmd: &mut Command, what: &str) {
    let status = cmd.status().unwrap_or_else(|e| panic!("failed to run {}: {}", what, e));
    if !status.success() {
        panic!("{} failed with status {}", what, status);
    }
}

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let project_root = manifest_dir.parent().expect("project root");
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));

    let c_src = project_root.join("main.c");
    let include_dir = project_root;
    let obj = out_dir.join("my_ice_core.o");
    let lib = out_dir.join("libmy_ice_core.a");

    run(
        Command::new("cc")
            .arg("-O2")
            .arg("-Wall")
            .arg("-Wextra")
            .arg("-std=c11")
            .arg("-Dmain=c_driver_main")
            .arg("-I")
            .arg(include_dir)
            .arg("-c")
            .arg(&c_src)
            .arg("-o")
            .arg(&obj),
        "cc",
    );

    run(
        Command::new("ar")
            .arg("rcs")
            .arg(&lib)
            .arg(&obj),
        "ar",
    );

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=my_ice_core");
    println!("cargo:rerun-if-changed={}", c_src.display());
    println!("cargo:rerun-if-changed={}", project_root.join("ice_min.h").display());
}
