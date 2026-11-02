fn main() {
    println!(
        "cargo:rustc-env=NATIVE_TARGET={}",
        std::env::var("TARGET").unwrap()
    );
    tauri_build::build();
}
