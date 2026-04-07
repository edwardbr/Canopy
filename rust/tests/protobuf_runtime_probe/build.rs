use protobuf_codegen::{CodeGen, Dependency};
use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() -> Result<(), String> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir
        .parent()
        .and_then(Path::parent)
        .and_then(Path::parent)
        .expect("repo root")
        .to_path_buf();

    println!("cargo:rerun-if-env-changed=CANOPY_GENERATOR");
    println!("cargo:rerun-if-env-changed=CANOPY_PROTOC");
    let generator = env::var("CANOPY_GENERATOR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root.join("build_debug/output/generator"));
    let protoc = env::var("CANOPY_PROTOC")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root.join("build_debug/output/protoc"));
    let rpc_idl = repo_root.join("interfaces/rpc/rpc_types.idl");
    let probe_idl = manifest_dir.join("basic_rpc_probe.idl");
    let nested_layout_idl = manifest_dir.join("nested_layout_probe.idl");
    let fuzz_test_idl = repo_root.join("integration_tests/interfaces/fuzz_test/fuzz_test.idl");
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let canopy_generated = out_dir.join("canopy_generated");
    let stage_root = out_dir.join("proto_stage");
    let protobuf_generated = out_dir.join("protobuf_generated");

    println!("cargo:rerun-if-changed={}", probe_idl.display());
    println!("cargo:rerun-if-changed={}", nested_layout_idl.display());
    println!("cargo:rerun-if-changed={}", fuzz_test_idl.display());
    println!("cargo:rerun-if-changed={}", rpc_idl.display());
    println!("cargo:rerun-if-changed={}", generator.display());
    println!("cargo:rerun-if-changed={}", protoc.display());

    run_generator(
        &generator,
        &repo_root,
        &rpc_idl,
        &canopy_generated,
        "rpc_types",
        &["-P", "interfaces"],
        true,
    )?;
    run_generator(
        &generator,
        &repo_root,
        &probe_idl,
        &canopy_generated,
        "basic_rpc_probe",
        &[],
        true,
    )?;
    run_generator(
        &generator,
        &repo_root,
        &nested_layout_idl,
        &canopy_generated,
        "nested_layout_probe",
        &[],
        false,
    )?;
    run_generator(
        &generator,
        &repo_root,
        &fuzz_test_idl,
        &canopy_generated,
        "fuzz_test",
        &[],
        true,
    )?;

    fs::create_dir_all(&stage_root).map_err(display_io)?;
    copy_tree(&canopy_generated.join("src"), &stage_root).map_err(display_io)?;

    let original_dir = env::current_dir().map_err(display_io)?;
    env::set_current_dir(&stage_root).map_err(display_io)?;

    let result = (|| {
        let rpc_target = Target::new("crate::rpc", "rpc", "rpc/protobuf/manifest.txt")?;
        let probe_target = Target::new(
            "crate::protobuf_runtime_probe",
            "protobuf_runtime_probe",
            "protobuf_runtime_probe/protobuf/manifest.txt",
        )?;
        let fuzz_target = Target::new(
            "crate::fuzz_test",
            "fuzz_test",
            "fuzz_test/protobuf/manifest.txt",
        )?;

        CodeGen::new()
            .output_dir(protobuf_generated.join(&rpc_target.output_subdir))
            .includes(std::iter::once(PathBuf::from(".")))
            .inputs(rpc_target.proto_files.iter().map(PathBuf::from))
            .protoc_path(&protoc)
            .generate_and_compile()?;

        CodeGen::new()
            .output_dir(protobuf_generated.join(&probe_target.output_subdir))
            .includes(std::iter::once(PathBuf::from(".")))
            .inputs(probe_target.proto_files.iter().map(PathBuf::from))
            .dependency(vec![rpc_target.dependency()])
            .protoc_path(&protoc)
            .generate_and_compile()?;

        CodeGen::new()
            .output_dir(protobuf_generated.join(&fuzz_target.output_subdir))
            .includes(std::iter::once(PathBuf::from(".")))
            .inputs(fuzz_target.proto_files.iter().map(PathBuf::from))
            .dependency(vec![rpc_target.dependency()])
            .protoc_path(&protoc)
            .generate_and_compile()?;

        Ok::<(), String>(())
    })();

    env::set_current_dir(&original_dir).map_err(display_io)?;
    result?;

    let rpc_proto_messages =
        find_generated_rs(&protobuf_generated.join("rpc")).ok_or("missing rpc generated.rs")?;
    let probe_proto_messages =
        find_generated_rs(&protobuf_generated.join("protobuf_runtime_probe"))
            .ok_or("missing basic rpc probe generated.rs")?;
    let fuzz_proto_messages = find_generated_rs(&protobuf_generated.join("fuzz_test"))
        .ok_or("missing fuzz test generated.rs")?;

    println!(
        "cargo:rustc-env=CANOPY_RPC_PROTO_MESSAGES_RS={}",
        rpc_proto_messages.display()
    );
    println!(
        "cargo:rustc-env=CANOPY_BASIC_RPC_PROTO_MESSAGES_RS={}",
        probe_proto_messages.display()
    );
    println!(
        "cargo:rustc-env=CANOPY_FUZZ_TEST_PROTO_MESSAGES_RS={}",
        fuzz_proto_messages.display()
    );
    println!(
        "cargo:rustc-env=CANOPY_BASIC_RPC_BINDINGS_RS={}",
        canopy_generated
            .join("rust/protobuf_runtime_probe/basic_rpc_probe.rs")
            .display()
    );
    println!(
        "cargo:rustc-env=CANOPY_BASIC_RPC_PROTOBUF_BINDINGS_RS={}",
        canopy_generated
            .join("rust/protobuf_runtime_probe/basic_rpc_probe_protobuf.rs")
            .display()
    );
    println!(
        "cargo:rustc-env=CANOPY_NESTED_LAYOUT_BINDINGS_RS={}",
        canopy_generated
            .join("rust/protobuf_runtime_probe/nested_layout_probe.rs")
            .display()
    );
    println!(
        "cargo:rustc-env=CANOPY_FUZZ_TEST_BINDINGS_RS={}",
        canopy_generated
            .join("rust/fuzz_test/fuzz_test.rs")
            .display()
    );
    println!(
        "cargo:rustc-env=CANOPY_FUZZ_TEST_PROTOBUF_BINDINGS_RS={}",
        canopy_generated
            .join("rust/fuzz_test/fuzz_test_protobuf.rs")
            .display()
    );

    Ok(())
}

#[derive(Clone)]
struct Target {
    crate_name: String,
    output_subdir: String,
    proto_files: Vec<String>,
}

impl Target {
    fn new(crate_name: &str, output_subdir: &str, manifest_path: &str) -> Result<Self, String> {
        Ok(Self {
            crate_name: crate_name.to_owned(),
            output_subdir: output_subdir.to_owned(),
            proto_files: read_manifest(manifest_path)?,
        })
    }

    fn dependency(&self) -> Dependency {
        Dependency {
            crate_name: self.crate_name.clone(),
            proto_import_paths: vec![PathBuf::from(".")],
            proto_files: self.proto_files.clone(),
        }
    }
}

fn run_generator(
    generator: &Path,
    repo_root: &Path,
    idl_path: &Path,
    output_path: &Path,
    name: &str,
    extra_args: &[&str],
    protobuf: bool,
) -> Result<(), String> {
    let mut command = Command::new(generator);
    command.current_dir(repo_root);
    command.arg("--idl");
    command.arg(idl_path);
    command.arg("--output_path");
    command.arg(output_path);
    command.arg("--name");
    command.arg(name);
    command.arg("--rust");
    if protobuf {
        command.arg("--protobuf");
    }
    command.args(extra_args);

    let output = command.output().map_err(display_io)?;
    if output.status.success() {
        return Ok(());
    }

    Err(format!(
        "generator failed for {}: {}",
        idl_path.display(),
        String::from_utf8_lossy(&output.stderr)
    ))
}

fn display_io(err: io::Error) -> String {
    err.to_string()
}

fn copy_tree(src: &Path, dst: &Path) -> io::Result<()> {
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let path = entry.path();
        let dest = dst.join(entry.file_name());

        if entry.file_type()?.is_dir() {
            fs::create_dir_all(&dest)?;
            copy_tree(&path, &dest)?;
        } else {
            fs::copy(&path, &dest)?;
        }
    }

    Ok(())
}

fn read_manifest(path: &str) -> Result<Vec<String>, String> {
    let contents = fs::read_to_string(path).map_err(display_io)?;
    Ok(contents
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(ToOwned::to_owned)
        .collect())
}

fn find_generated_rs(root: &Path) -> Option<PathBuf> {
    for entry in fs::read_dir(root).ok()? {
        let entry = entry.ok()?;
        let path = entry.path();
        if entry.file_type().ok()?.is_dir() {
            if let Some(found) = find_generated_rs(&path) {
                return Some(found);
            }
        } else if entry.file_name() == "generated.rs" {
            return Some(path);
        }
    }

    None
}
