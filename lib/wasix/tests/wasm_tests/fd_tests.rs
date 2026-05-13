wasm_test!(test_fd_allocate, "fd-allocate");
wasm_test!(test_fd_dup2_huge_min, "fd-dup2-huge-min");
wasm_test!(test_fd_append_after_truncate, "fd-append-after-truncate");
wasm_test!(test_fd_append_seek_read, "fd-append-seek-read");
wasm_test!(test_fd_open_readonly, "fd-open-readonly");

wasm_test!(
    test_fd_sparse_write_after_truncate,
    "fd-sparse-write-after-truncate"
);
wasm_test!(
    test_fd_renumber_negative_target,
    "fd-renumber-negative-target"
);
wasm_test!(test_proc_spawn2_dup2_huge_fd, "proc-spawn2-dup2-huge-fd");
wasm_test!(test_proc_spawn2_open_huge_fd, "proc-spawn2-open-huge-fd");

wasm_test!(test_fd_close, "fd-close");

wasm_test!(test_pipes, "pipes");

wasm_test!(
    test_pwrite_and_size,
    "pwrite-and-size",
    temp_dir,
    stdout = "0"
);

// Pins stdin to a non-tty capture so the assertion that stdio is *not* a
// terminal holds regardless of whether `cargo test` was launched from an
// interactive terminal. stdout/stderr are already capture buffers in the
// shared runner.
#[test]
fn test_fd_isatty_and_stdio_filetype() {
    use std::sync::{Arc, Mutex};

    let wasm = super::run_build_script(file!(), "fd-isatty").unwrap();
    let test_dir = wasm.parent().unwrap();
    let result = super::run_wasm_with_runner_config(&wasm, test_dir, |runner| {
        let buf = Arc::new(Mutex::new(Vec::new()));
        runner.with_stdin(Box::new(super::CaptureFile::new(buf)));
    })
    .unwrap();

    if result.exit_code != Some(0) {
        panic!(
            "fd-isatty test failed:\n{}",
            super::format_captured_output(&result)
        );
    }
}
