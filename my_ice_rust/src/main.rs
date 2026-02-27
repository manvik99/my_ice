use std::ffi::CString;
use std::os::raw::c_char;

extern "C" {
    fn c_driver_main(argc: i32, argv: *mut *mut c_char) -> i32;
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let mut c_args = Vec::with_capacity(args.len());
    for arg in args {
        match CString::new(arg) {
            Ok(s) => c_args.push(s),
            Err(_) => {
                eprintln!("argument contains NUL byte");
                std::process::exit(2);
            }
        }
    }

    let mut argv: Vec<*mut c_char> = c_args
        .iter_mut()
        .map(|s| s.as_ptr() as *mut c_char)
        .collect();

    let rc = unsafe { c_driver_main(argv.len() as i32, argv.as_mut_ptr()) };
    std::process::exit(rc);
}
