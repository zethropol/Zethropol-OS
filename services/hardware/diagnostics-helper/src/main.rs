use std::process::{Command, ExitCode};

fn main() -> ExitCode {
    let output = match Command::new("/usr/bin/smartctl")
        .args(["-a", "--json", "/dev/nvme0n1"])
        .output()
    {
        Ok(output) => output,
        Err(error) => {
            eprintln!("diagnostics helper: {error}");
            return ExitCode::from(1);
        }
    };

    print!("{}", String::from_utf8_lossy(&output.stdout));
    eprint!("{}", String::from_utf8_lossy(&output.stderr));

    ExitCode::from(output.status.code().unwrap_or(1) as u8)
}
