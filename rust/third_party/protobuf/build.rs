use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rustc-check-cfg=cfg(bzl)");
    println!("cargo:rustc-check-cfg=cfg(cpp_kernel)");
    println!("cargo:rustc-check-cfg=cfg(upb_kernel)");
    println!("cargo:rustc-check-cfg=cfg(lite_runtime)");
    println!("cargo:rerun-if-env-changed=CANOPY_PROTOBUF_NATIVE_LIB_DIR");
    println!("cargo:rerun-if-env-changed=CANOPY_PROTOBUF_NATIVE_LIB_NAME");

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir
        .parent()
        .and_then(Path::parent)
        .and_then(Path::parent)
        .expect("repo root");

    if let Ok(lib_dir) = env::var("CANOPY_PROTOBUF_NATIVE_LIB_DIR") {
        let lib_name =
            env::var("CANOPY_PROTOBUF_NATIVE_LIB_NAME").unwrap_or_else(|_| "upbd".to_string());
        println!("cargo:rustc-link-search=native={}", lib_dir);
        println!("cargo:rustc-link-lib=static={}", lib_name);
        return;
    }

    build_libupb(&repo_root);
}

fn build_libupb(repo_root: &Path) {
    let proto_root = repo_root.join("submodules/protobuf");
    let file_lists = proto_root.join("src/file_lists.cmake");
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let obj_dir = out_dir.join("libupb_objs");
    let archive = out_dir.join("libupb_canopy.a");

    fs::create_dir_all(&obj_dir).expect("create libupb object dir");
    println!("cargo:rerun-if-changed={}", file_lists.display());

    let mut sources = parse_libupb_sources(&file_lists, &proto_root);
    sources
        .push(proto_root.join("upb/reflection/cmake/google/protobuf/descriptor.upb_minitable.c"));
    sources.push(proto_root.join("third_party/utf8_range/utf8_range.c"));

    let include_dirs = [
        proto_root.clone(),
        proto_root.join("src"),
        proto_root.join("third_party/utf8_range"),
        proto_root.join("upb/reflection/cmake"),
    ];

    let mut objects = Vec::with_capacity(sources.len());
    for source in sources {
        println!("cargo:rerun-if-changed={}", source.display());
        let object = obj_dir.join(object_name_for(&source, &proto_root));
        compile_c(&source, &object, &include_dirs, false);
        objects.push(object);
    }

    let wrapper_source = write_rust_ffi_wrapper(&out_dir, &proto_root);
    println!("cargo:rerun-if-changed={}", wrapper_source.display());
    let wrapper_object = obj_dir.join("rust_upb_api_wrapper.o");
    compile_c(&wrapper_source, &wrapper_object, &include_dirs, true);
    objects.push(wrapper_object);

    archive_objects(&archive, &objects);
    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=upb_canopy");
}

fn parse_libupb_sources(file_lists: &Path, proto_root: &Path) -> Vec<PathBuf> {
    let contents = fs::read_to_string(file_lists).expect("read file_lists.cmake");
    let mut in_list = false;
    let mut sources = Vec::new();

    for line in contents.lines() {
        let trimmed = line.trim();
        if trimmed == "set(libupb_srcs" {
            in_list = true;
            continue;
        }

        if in_list {
            if trimmed == ")" {
                break;
            }

            if trimmed.is_empty() {
                continue;
            }

            let path = trimmed.replace("${protobuf_SOURCE_DIR}/", "");
            sources.push(proto_root.join(path));
        }
    }

    sources
}

fn object_name_for(source: &Path, proto_root: &Path) -> String {
    let relative = source
        .strip_prefix(proto_root)
        .expect("source under protobuf root");
    let name = relative
        .to_string_lossy()
        .replace('/', "_")
        .replace('.', "_");
    format!("{name}.o")
}

fn compile_c(source: &Path, object: &Path, include_dirs: &[PathBuf], export_api_inlines: bool) {
    let compiler = env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let mut command = Command::new(&compiler);
    command
        .arg("-std=c99")
        .arg("-fPIC")
        .arg("-DGOOGLE_PROTOBUF_CMAKE_BUILD")
        .arg("-DHAVE_ZLIB");

    // The official protobuf Cargo package builds a single amalgamated upb.c
    // translation unit with UPB_BUILD_API enabled. In this repository checkout
    // we only have the split libupb source list, so enabling UPB_BUILD_API
    // globally would cause UPB_API_INLINE functions to become exported
    // definitions in every translation unit. That produces duplicate upb_*
    // symbols at link time. For the split-source build we keep normal sources
    // on the non-exporting path and emit the Rust FFI inline entrypoints from
    // one dedicated wrapper translation unit only.
    if export_api_inlines {
        command.arg("-DUPB_BUILD_API=1");
    }

    for include_dir in include_dirs {
        command.arg("-I").arg(include_dir);
    }

    let status = command
        .arg("-c")
        .arg(source)
        .arg("-o")
        .arg(object)
        .status()
        .expect("compile libupb source");

    assert!(status.success(), "failed to compile {}", source.display());
}

fn archive_objects(archive: &Path, objects: &[PathBuf]) {
    let ar = env::var("AR").unwrap_or_else(|_| "ar".to_string());
    let mut command = Command::new(&ar);
    command.arg("crus").arg(archive);
    for object in objects {
        command.arg(object);
    }

    let status = command.status().expect("archive libupb");
    assert!(status.success(), "failed to archive {}", archive.display());
}

fn write_rust_ffi_wrapper(out_dir: &Path, proto_root: &Path) -> PathBuf {
    let wrapper = out_dir.join("rust_upb_api_wrapper.c");
    let contents = format!(
        "#include \"{0}\"\n#include \"{1}\"\n#include \"{2}\"\n#include \"{3}\"\n#include \"{4}\"\n#include \"{5}\"\n#include \"{6}\"\n",
        proto_root.join("upb/base/string_view.h").display(),
        proto_root.join("upb/mem/arena.h").display(),
        proto_root.join("upb/mini_table/message.h").display(),
        proto_root.join("upb/mini_descriptor/decode.h").display(),
        proto_root.join("upb/message/message.h").display(),
        proto_root.join("upb/message/accessors.h").display(),
        proto_root.join("upb/message/internal/accessors.h").display(),
    );
    fs::write(&wrapper, contents).expect("write rust upb api wrapper");
    wrapper
}
