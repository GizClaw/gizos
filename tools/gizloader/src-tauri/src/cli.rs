use std::{ffi::OsString, path::Path, process::Command};

pub fn run(binary: &Path, arguments: &[OsString]) -> Result<i32, String> {
    #[cfg(target_os = "macos")]
    if let Some(bundle) = macos::app_bundle(&std::env::current_exe().map_err(|e| e.to_string())?) {
        return macos::launch(&bundle, arguments).map_err(|e| e.to_string());
    }
    let mut command = Command::new(binary);
    command.args(arguments);
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        Err(command.exec().to_string())
    }
    #[cfg(not(unix))]
    command
        .status()
        .map_err(|e| e.to_string())
        .map(|status| status.code().unwrap_or(1))
}

pub fn dispatch_worker(_arguments: &[OsString]) {
    #[cfg(target_os = "macos")]
    if _arguments.first().is_some_and(|arg| arg == macos::WORKER) {
        let result = if _arguments.len() == 2 {
            crate::native::binary().and_then(|binary| {
                macos::worker(Path::new(&_arguments[1]), &binary).map_err(|e| e.to_string())
            })
        } else {
            Err("CLI worker requires one private request directory".into())
        };
        match result {
            Ok(code) => std::process::exit(code),
            Err(error) => {
                eprintln!("Gizloader: {error}");
                std::process::exit(1);
            }
        }
    }
}

#[cfg(target_os = "macos")]
mod macos {
    use super::*;
    use serde::{de::DeserializeOwned, Deserialize, Serialize};
    use std::{
        fs::{self, File, OpenOptions},
        io::{self, Read, Seek, Write},
        os::unix::{
            ffi::{OsStrExt, OsStringExt},
            fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
            process::{CommandExt, ExitStatusExt},
        },
        path::PathBuf,
        process::{Child, ExitStatus, Stdio},
        sync::atomic::{AtomicI32, Ordering},
        thread,
        time::{Duration, Instant},
    };

    pub const WORKER: &str = "--gizloader-cli-worker";
    const LIMIT: u64 = 64 * 1024;
    const POLL: Duration = Duration::from_millis(20);
    static SIGNAL: AtomicI32 = AtomicI32::new(0);

    #[derive(Serialize, Deserialize)]
    struct Request {
        arguments: Vec<Vec<u8>>,
        cwd: Vec<u8>,
        environment: Vec<(Vec<u8>, Vec<u8>)>,
    }
    #[derive(Serialize, Deserialize)]
    struct Completed {
        code: i32,
    }

    pub fn app_bundle(executable: &Path) -> Option<PathBuf> {
        let macos = executable.parent()?;
        let contents = macos.parent()?;
        let app = contents.parent()?;
        (macos.file_name()? == "MacOS"
            && contents.file_name()? == "Contents"
            && app.extension()? == "app")
            .then(|| app.to_path_buf())
    }

    fn invalid(message: &str) -> io::Error {
        io::Error::new(io::ErrorKind::InvalidInput, message)
    }

    fn validate_directory(directory: &Path) -> io::Result<()> {
        let metadata = fs::symlink_metadata(directory)?;
        if !directory.is_absolute()
            || directory.canonicalize()? != directory
            || !metadata.is_dir()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o077 != 0
        {
            return Err(invalid(
                "CLI worker directory must be absolute, owned, private and without symlinks",
            ));
        }
        Ok(())
    }

    fn create(directory: &Path, name: &str) -> io::Result<File> {
        OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(directory.join(name))
    }

    fn private_file(directory: &Path, name: &str, append: bool) -> io::Result<File> {
        let file = OpenOptions::new()
            .read(!append)
            .append(append)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(directory.join(name))?;
        let metadata = file.metadata()?;
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o077 != 0
        {
            return Err(invalid(
                "CLI worker files must be private regular files owned by the caller",
            ));
        }
        Ok(file)
    }

    fn read_json<T: DeserializeOwned>(directory: &Path, name: &str) -> io::Result<T> {
        let mut bytes = Vec::new();
        private_file(directory, name, false)?
            .take(LIMIT + 1)
            .read_to_end(&mut bytes)?;
        if bytes.len() as u64 > LIMIT {
            return Err(invalid("CLI request exceeds 64 KiB"));
        }
        serde_json::from_slice(&bytes).map_err(io::Error::other)
    }

    fn caller_environment(
        variables: impl IntoIterator<Item = (OsString, OsString)>,
    ) -> impl Iterator<Item = (OsString, OsString)> {
        variables.into_iter().filter(|(name, _)| {
            !matches!(
                name.as_bytes(),
                b"__CFBundleIdentifier" | b"__CF_USER_TEXT_ENCODING" | b"XPC_SERVICE_NAME"
            )
        })
    }

    fn request(
        arguments: &[OsString],
        cwd: &Path,
        environment: impl IntoIterator<Item = (OsString, OsString)>,
    ) -> io::Result<Vec<u8>> {
        let value = Request {
            arguments: arguments
                .iter()
                .map(|arg| arg.as_bytes().to_vec())
                .collect(),
            cwd: cwd.canonicalize()?.as_os_str().as_bytes().to_vec(),
            environment: caller_environment(environment)
                .map(|(name, value)| (name.as_bytes().to_vec(), value.as_bytes().to_vec()))
                .collect(),
        };
        let bytes = serde_json::to_vec(&value)?;
        if bytes.len() as u64 > LIMIT {
            return Err(invalid("CLI request exceeds 64 KiB"));
        }
        Ok(bytes)
    }

    // File offsets retain partial lines and arbitrary bytes without buffering a full log.
    fn tail(file: &mut File, output: &mut impl Write) -> io::Result<()> {
        io::copy(&mut file.take(LIMIT), output)?;
        output.flush()
    }

    extern "C" fn interrupted(signal: libc::c_int) {
        let _ = SIGNAL.compare_exchange(0, signal, Ordering::Relaxed, Ordering::Relaxed);
    }
    struct Signals([libc::sighandler_t; 2]);
    impl Signals {
        fn install() -> Self {
            SIGNAL.store(0, Ordering::Relaxed);
            Self([libc::SIGINT, libc::SIGTERM].map(|signal| unsafe {
                libc::signal(signal, interrupted as *const () as libc::sighandler_t)
            }))
        }
    }
    impl Drop for Signals {
        fn drop(&mut self) {
            for (signal, previous) in [libc::SIGINT, libc::SIGTERM].into_iter().zip(self.0) {
                unsafe {
                    libc::signal(signal, previous);
                }
            }
        }
    }

    fn wait_bounded(child: &mut Child, grace: Duration) -> io::Result<ExitStatus> {
        let deadline = Instant::now() + grace;
        loop {
            if let Some(status) = child.try_wait()? {
                return Ok(status);
            }
            if Instant::now() >= deadline {
                child.kill()?;
                return child.wait();
            }
            thread::sleep(POLL);
        }
    }
    fn exit_code(status: ExitStatus) -> i32 {
        status
            .code()
            .unwrap_or_else(|| 128 + status.signal().unwrap_or(1))
    }
    fn publish(directory: &Path, name: &str, value: &impl Serialize) -> io::Result<()> {
        let mut file = tempfile::NamedTempFile::new_in(directory)?;
        serde_json::to_writer(&mut file, value)?;
        file.persist(directory.join(name))
            .map_err(io::Error::other)?;
        Ok(())
    }
    fn cancel(directory: &Path, signal: i32) -> io::Result<()> {
        publish(directory, "cancel", &signal)
    }
    fn cancellation(directory: &Path) -> io::Result<Option<i32>> {
        match read_json::<i32>(directory, "cancel") {
            Ok(signal) if [libc::SIGINT, libc::SIGTERM].contains(&signal) => Ok(Some(signal)),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
            _ => Err(invalid("Invalid CLI cancellation request")),
        }
    }

    pub fn launch(bundle: &Path, arguments: &[OsString]) -> io::Result<i32> {
        let bytes = request(arguments, &std::env::current_dir()?, std::env::vars_os())?;
        let temporary = tempfile::Builder::new()
            .prefix("gizloader-cli-")
            .tempdir()?;
        fs::set_permissions(temporary.path(), fs::Permissions::from_mode(0o700))?;
        let directory = temporary.path().canonicalize()?;
        create(&directory, "request.json")?.write_all(&bytes)?;
        let mut out = create(&directory, "stdout")?;
        let mut err = create(&directory, "stderr")?;
        let _signals = Signals::install();
        let mut launcher = Command::new("/usr/bin/open")
            .args(["-W", "-n"])
            .arg(bundle)
            .args(["--args", WORKER])
            .arg(&directory)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .process_group(0)
            .spawn()?;
        let result = (|| {
            let mut cancellation = None;
            loop {
                tail(&mut out, &mut io::stdout().lock())?;
                tail(&mut err, &mut io::stderr().lock())?;
                let signal = SIGNAL.load(Ordering::Relaxed);
                if signal != 0 && cancellation.is_none() {
                    cancel(&directory, signal)?;
                    cancellation = Some(Instant::now());
                }
                if launcher.try_wait()?.is_some() {
                    // The worker writes the result only after the native process is reaped.
                    while out.metadata()?.len() > out.stream_position()? {
                        tail(&mut out, &mut io::stdout().lock())?;
                    }
                    while err.metadata()?.len() > err.stream_position()? {
                        tail(&mut err, &mut io::stderr().lock())?;
                    }
                    let completed: Completed = read_json(&directory, "result.json")?;
                    return Ok(if signal == 0 {
                        completed.code
                    } else {
                        128 + signal
                    });
                }
                if cancellation
                    .is_some_and(|start: Instant| start.elapsed() >= Duration::from_secs(5))
                {
                    wait_bounded(&mut launcher, Duration::ZERO)?;
                    return Ok(128 + signal);
                }
                thread::sleep(POLL);
            }
        })();
        if result.is_err() {
            let _ = cancel(&directory, libc::SIGTERM);
            let _ = wait_bounded(&mut launcher, Duration::from_secs(5));
        }
        result
    }

    pub fn worker(directory: &Path, binary: &Path) -> io::Result<i32> {
        validate_directory(directory)?;
        let request: Request = read_json(directory, "request.json")?;
        let cwd = PathBuf::from(OsString::from_vec(request.cwd));
        if !cwd.is_absolute() || cwd.canonicalize()? != cwd || !cwd.is_dir() {
            return Err(invalid(
                "CLI working directory must be canonical and absolute",
            ));
        }
        let mut errors = private_file(directory, "stderr", true)?;
        let outcome = (|| {
            if let Some(signal) = cancellation(directory)? {
                return Ok(128 + signal);
            }
            let mut child = Command::new(binary.canonicalize()?)
                .args(request.arguments.into_iter().map(OsString::from_vec))
                .envs(caller_environment(request.environment.into_iter().map(
                    |(name, value)| (OsString::from_vec(name), OsString::from_vec(value)),
                )))
                .current_dir(cwd)
                .stdin(Stdio::null())
                .stdout(private_file(directory, "stdout", true)?)
                .stderr(errors.try_clone()?)
                .spawn()?;
            let result = (|| loop {
                if let Some(status) = child.try_wait()? {
                    return Ok(exit_code(status));
                }
                if let Some(signal) = cancellation(directory)? {
                    unsafe {
                        libc::kill(child.id() as libc::pid_t, signal);
                    }
                    return wait_bounded(&mut child, Duration::from_secs(3)).map(exit_code);
                }
                thread::sleep(POLL);
            })();
            if result.is_err() {
                let _ = child.kill();
                let _ = child.wait();
            }
            result
        })();
        let code = outcome.unwrap_or_else(|error: io::Error| {
            let _ = writeln!(errors, "Gizloader: {error}");
            1
        });
        publish(directory, "result.json", &Completed { code })?;
        Ok(code)
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use std::io::Seek;

        fn exchange(arguments: &[OsString], cwd: &Path) -> tempfile::TempDir {
            let directory = tempfile::tempdir().unwrap();
            fs::set_permissions(directory.path(), fs::Permissions::from_mode(0o700)).unwrap();
            create(directory.path(), "request.json")
                .unwrap()
                .write_all(
                    &request(
                        arguments,
                        cwd,
                        [("GIZLOADER_TEST_VALUE".into(), "restored".into())],
                    )
                    .unwrap(),
                )
                .unwrap();
            create(directory.path(), "stdout").unwrap();
            create(directory.path(), "stderr").unwrap();
            directory
        }

        #[test]
        fn request_preserves_raw_arguments_and_canonical_working_directory() {
            let cwd = tempfile::tempdir().unwrap();
            let arguments = vec![OsString::from("固件 name"), OsString::from_vec(vec![0xff])];
            let encoded = request(&arguments, cwd.path(), []).unwrap();
            let decoded: Request = serde_json::from_slice(&encoded).unwrap();
            assert_eq!(
                decoded
                    .arguments
                    .into_iter()
                    .map(OsString::from_vec)
                    .collect::<Vec<_>>(),
                arguments
            );
            assert_eq!(
                PathBuf::from(OsString::from_vec(decoded.cwd)),
                cwd.path().canonicalize().unwrap()
            );
            assert!(request(
                &[OsString::from("x".repeat(LIMIT as usize))],
                cwd.path(),
                []
            )
            .is_err());
            assert_eq!(
                app_bundle(Path::new(
                    "/Applications/Gizloader.app/Contents/MacOS/gizloader"
                )),
                Some(PathBuf::from("/Applications/Gizloader.app"))
            );
            assert!(app_bundle(Path::new("/tmp/gizloader")).is_none());
        }

        #[test]
        fn caller_environment_preserves_raw_values_without_overriding_launchservices() {
            let cwd = tempfile::tempdir().unwrap();
            let reserved = [
                "__CFBundleIdentifier",
                "__CF_USER_TEXT_ENCODING",
                "XPC_SERVICE_NAME",
            ];
            let raw = (
                OsString::from_vec(b"GIZ_\xff".to_vec()),
                OsString::from_vec(vec![0xff]),
            );
            let mut variables: Vec<_> = reserved
                .iter()
                .map(|name| (OsString::from(name), "caller".into()))
                .collect();
            variables.extend([
                ("PATH".into(), "/sdk/bin".into()),
                ("HOME".into(), "/caller/home".into()),
                raw.clone(),
            ]);
            let encoded = request(&[], cwd.path(), variables.clone()).unwrap();
            let decoded: Request = serde_json::from_slice(&encoded).unwrap();
            assert_eq!(decoded.environment.len(), 3);
            assert!(decoded
                .environment
                .contains(&(raw.0.as_bytes().to_vec(), raw.1.as_bytes().to_vec())));
            let mut command = Command::new("/usr/bin/true");
            for name in reserved {
                command.env(name, "app-owned");
            }
            command.envs(caller_environment(variables));
            for name in reserved {
                assert_eq!(
                    command.get_envs().find(|(key, _)| *key == name).unwrap().1,
                    Some(std::ffi::OsStr::new("app-owned"))
                );
            }
            assert_eq!(
                command
                    .get_envs()
                    .find(|(key, _)| *key == "PATH")
                    .unwrap()
                    .1,
                Some(std::ffi::OsStr::new("/sdk/bin"))
            );
            assert!(request(
                &[],
                cwd.path(),
                [("LARGE".into(), OsString::from("x".repeat(LIMIT as usize)))]
            )
            .is_err());
        }

        #[test]
        fn worker_preserves_cwd_stream_bytes_and_exit_status() {
            let cwd = tempfile::tempdir().unwrap();
            fs::write(cwd.path().join("relative"), b"\xfftail").unwrap();
            let directory = exchange(
                &[
                    "-c".into(),
                    "cat relative; printf '%s' \"$GIZLOADER_TEST_VALUE\"; printf diagnostic >&2; exit 7".into(),
                ],
                cwd.path(),
            );
            let path = directory.path().canonicalize().unwrap();
            assert_eq!(worker(&path, Path::new("/bin/sh")).unwrap(), 7);
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                7
            );
            let mut output = private_file(&path, "stdout", false).unwrap();
            let mut bytes = Vec::new();
            tail(&mut output, &mut bytes).unwrap();
            private_file(&path, "stdout", true)
                .unwrap()
                .write_all(b"next")
                .unwrap();
            tail(&mut output, &mut bytes).unwrap();
            assert_eq!(bytes, b"\xfftailrestorednext");
            assert_eq!(output.stream_position().unwrap(), 17);
            assert_eq!(fs::read(path.join("stderr")).unwrap(), b"diagnostic");
        }

        #[test]
        fn private_request_rejects_symlinks_permissions_and_oversize() {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().canonicalize().unwrap();
            create(&path, "request.json")
                .unwrap()
                .write_all(&vec![b'x'; LIMIT as usize + 1])
                .unwrap();
            assert!(read_json::<Request>(&path, "request.json").is_err());
            fs::remove_file(path.join("request.json")).unwrap();
            std::os::unix::fs::symlink("/etc/passwd", path.join("request.json")).unwrap();
            assert!(read_json::<Request>(&path, "request.json").is_err());
            fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
            assert!(validate_directory(&path).is_err());
        }

        #[test]
        fn cancellation_marker_cooperatively_stops_and_reaps_the_native_child() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = exchange(
                &[
                    "-c".into(),
                    "trap 'exit 130' INT TERM; printf ready; while :; do :; done".into(),
                ],
                cwd.path(),
            );
            let path = directory.path().canonicalize().unwrap();
            let worker_path = path.clone();
            let worker = thread::spawn(move || worker(&worker_path, Path::new("/bin/sh")));
            let deadline = Instant::now() + Duration::from_secs(3);
            while fs::metadata(path.join("stdout")).unwrap().len() == 0 && Instant::now() < deadline
            {
                thread::sleep(POLL);
            }
            cancel(&path, libc::SIGTERM).unwrap();
            assert_eq!(worker.join().unwrap().unwrap(), 130);
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                130
            );
        }
    }
}
