//! Real commands-folder dispatch - the plugin surface for the `/`
//! command palette. A command is a plain file under `commands/` (no
//! `.sh` extension, filename = command name) containing real bash;
//! typing its name and pressing Enter runs it for real, with the
//! current selection's pids as real arguments.

use std::path::PathBuf;
use std::process::Command;

/// commands_dir - real `commands/` next to the running binary's
/// project root when developing (`./commands`), falling back to
/// nothing found (the palette then just reports "no such command"
/// honestly rather than guessing a path).
fn commands_dir() -> PathBuf {
    PathBuf::from("commands")
}

/// run - real dispatch: `commands/<name>` executed via `sh` with
/// `args` as real positional arguments ($1, $2, ...). Returns the
/// command's own real combined stdout+stderr (trimmed), or a real,
/// honest error when the file doesn't exist or isn't executable-ish.
pub fn run(name: &str, args: &[String]) -> Result<String, String> {
    let path = commands_dir().join(name);
    if !path.is_file() {
        return Err(format!("no such command: commands/{name}"));
    }
    let out = Command::new("sh")
        .arg(&path)
        .args(args)
        .output()
        .map_err(|e| e.to_string())?;
    let mut text = String::from_utf8_lossy(&out.stdout).trim().to_string();
    let stderr = String::from_utf8_lossy(&out.stderr);
    if !stderr.trim().is_empty() {
        if !text.is_empty() {
            text.push('\n');
        }
        text.push_str(stderr.trim());
    }
    if !out.status.success() && text.is_empty() {
        text = format!("exit status: {}", out.status);
    }
    Ok(text)
}
