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
        io::{self, Read, Write},
        os::{
            fd::AsRawFd,
            unix::{
                ffi::{OsStrExt, OsStringExt},
                fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
                process::{CommandExt, ExitStatusExt},
            },
        },
        path::PathBuf,
        process::{Child, ExitStatus, Stdio},
        sync::{
            atomic::{AtomicBool, AtomicI32, Ordering},
            mpsc::{self, Receiver, TryRecvError},
            Arc,
        },
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

    fn private_file(directory: &Path, name: &str) -> io::Result<File> {
        let file = OpenOptions::new()
            .read(true)
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
        private_file(directory, name)?
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

    fn fifo(directory: &Path, name: &str) -> io::Result<File> {
        let path = std::ffi::CString::new(directory.join(name).as_os_str().as_bytes())?;
        if unsafe { libc::mkfifo(path.as_ptr(), 0o600) } != 0 {
            return Err(io::Error::last_os_error());
        }
        private_fifo(directory, name, false)
    }

    fn private_fifo(directory: &Path, name: &str, write: bool) -> io::Result<File> {
        let file = OpenOptions::new()
            .read(!write)
            .write(write)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(directory.join(name))?;
        let metadata = file.metadata()?;
        if !metadata.file_type().is_fifo()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o077 != 0
        {
            return Err(invalid(
                "CLI streams must be private FIFOs owned by the caller",
            ));
        }
        Ok(file)
    }

    fn blocking(file: File) -> io::Result<File> {
        let flags = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GETFL) };
        if flags == -1
            || unsafe { libc::fcntl(file.as_raw_fd(), libc::F_SETFL, flags & !libc::O_NONBLOCK) }
                == -1
        {
            return Err(io::Error::last_os_error());
        }
        Ok(file)
    }

    struct Streams {
        stdout: File,
        stderr: File,
        // Only the supervising caller holds this reader. Closing it is observable even
        // when the native process is silent or a forwarding thread is backpressured.
        caller: File,
    }
    impl Streams {
        fn create(directory: &Path) -> io::Result<Self> {
            Ok(Self {
                stdout: fifo(directory, "stdout")?,
                stderr: fifo(directory, "stderr")?,
                caller: fifo(directory, "caller")?,
            })
        }
    }

    fn caller_alive(mut caller: &File) -> io::Result<bool> {
        // A nonblocking probe reliably reports EPIPE on Darwin when the last
        // reader dies. A full liveness FIFO still means the caller is connected;
        // its contents stay bounded in the kernel and never reach disk.
        match caller.write(&[0]) {
            Ok(_) => Ok(true),
            Err(error) if error.kind() == io::ErrorKind::BrokenPipe => Ok(false),
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                ) =>
            {
                Ok(true)
            }
            Err(error) => Err(error),
        }
    }

    struct Forwarding {
        finished: Arc<AtomicBool>,
        results: Receiver<io::Result<()>>,
        remaining: usize,
    }
    impl Forwarding {
        fn start(
            stdout: File,
            stderr: File,
            output: impl Write + Send + 'static,
            errors: impl Write + Send + 'static,
        ) -> Self {
            let finished = Arc::new(AtomicBool::new(false));
            let (sender, results) = mpsc::channel();
            fn forward(
                source: File,
                mut sink: impl Write,
                finished: &AtomicBool,
            ) -> io::Result<()> {
                let mut source = blocking(source)?;
                let mut buffer = [0; LIMIT as usize];
                loop {
                    match source.read(&mut buffer) {
                        Ok(0) if finished.load(Ordering::Acquire) => return sink.flush(),
                        Ok(0) => thread::sleep(POLL),
                        Ok(count) => {
                            sink.write_all(&buffer[..count])?;
                            sink.flush()?;
                        }
                        Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                        Err(error) => return Err(error),
                    }
                }
            }
            let stdout_finished = Arc::clone(&finished);
            let stdout_sender = sender.clone();
            thread::spawn(move || {
                let _ = stdout_sender.send(forward(stdout, output, &stdout_finished));
            });
            let stderr_finished = Arc::clone(&finished);
            thread::spawn(move || {
                let _ = sender.send(forward(stderr, errors, &stderr_finished));
            });
            Self {
                finished,
                results,
                remaining: 2,
            }
        }

        fn drained(&mut self) -> io::Result<bool> {
            while self.remaining != 0 {
                match self.results.try_recv() {
                    Ok(result) => {
                        self.remaining -= 1;
                        result?;
                    }
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => {
                        return Err(io::Error::other("CLI output forwarding stopped"));
                    }
                }
            }
            Ok(self.remaining == 0)
        }
    }
    impl Drop for Forwarding {
        fn drop(&mut self) {
            self.finished.store(true, Ordering::Release);
        }
    }

    const INTERRUPTS: [i32; 3] = [libc::SIGINT, libc::SIGTERM, libc::SIGHUP];
    extern "C" fn interrupted(signal: libc::c_int) {
        let _ = SIGNAL.compare_exchange(0, signal, Ordering::Relaxed, Ordering::Relaxed);
    }
    struct Signals([libc::sighandler_t; 3]);
    impl Signals {
        fn install() -> Self {
            SIGNAL.store(0, Ordering::Relaxed);
            Self(INTERRUPTS.map(|signal| unsafe {
                libc::signal(signal, interrupted as *const () as libc::sighandler_t)
            }))
        }
    }
    impl Drop for Signals {
        fn drop(&mut self) {
            for (signal, previous) in INTERRUPTS.into_iter().zip(self.0) {
                unsafe {
                    libc::signal(signal, previous);
                }
            }
        }
    }

    fn signal_child(child: &Child, signal: i32) -> io::Result<()> {
        // Both the LaunchServices helper and native child start their own process
        // groups, so cancellation also reaches native helper descendants.
        if unsafe { libc::kill(-(child.id() as libc::pid_t), signal) } == -1 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() != Some(libc::ESRCH) {
                return Err(error);
            }
        }
        Ok(())
    }

    fn wait_bounded(child: &mut Child, grace: Duration) -> io::Result<ExitStatus> {
        let deadline = Instant::now() + grace;
        loop {
            if let Some(status) = child.try_wait()? {
                return Ok(status);
            }
            if Instant::now() >= deadline {
                signal_child(child, libc::SIGKILL)?;
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
            Ok(signal) if INTERRUPTS.contains(&signal) => Ok(Some(signal)),
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
        let streams = Streams::create(&directory)?;
        let _signals = Signals::install();
        let launcher = Command::new("/usr/bin/open")
            .args(["-W", "-n"])
            .arg(bundle)
            .args(["--args", WORKER])
            .arg(&directory)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .process_group(0)
            .spawn()?;
        supervise(&directory, launcher, streams, io::stdout(), io::stderr())
    }

    fn supervise(
        directory: &Path,
        mut launcher: Child,
        streams: Streams,
        output: impl Write + Send + 'static,
        errors: impl Write + Send + 'static,
    ) -> io::Result<i32> {
        let Streams {
            stdout,
            stderr,
            caller: _caller,
        } = streams;
        // Forward separately so a blocked stdout/stderr consumer cannot prevent
        // signal handling, cancellation, or reaping. Each thread holds at most
        // one fixed-size buffer; FIFOs apply kernel backpressure to the native CLI.
        let mut forwarding = Forwarding::start(stdout, stderr, output, errors);
        let result = (|| {
            let mut cancellation = None;
            loop {
                let signal = SIGNAL.load(Ordering::Relaxed);
                if signal != 0 && cancellation.is_none() {
                    cancel(directory, signal)?;
                    cancellation = Some(Instant::now());
                }
                let drained = match forwarding.drained() {
                    Ok(drained) => drained,
                    Err(_) if signal != 0 => false,
                    Err(error) => return Err(error),
                };
                if launcher.try_wait()?.is_some() {
                    // The worker writes the result only after the native process is reaped.
                    forwarding.finished.store(true, Ordering::Release);
                    if drained {
                        let completed: Completed = read_json(directory, "result.json")?;
                        return Ok(if signal == 0 {
                            completed.code
                        } else {
                            128 + signal
                        });
                    }
                }
                if cancellation
                    .is_some_and(|start: Instant| start.elapsed() >= Duration::from_secs(5))
                {
                    wait_bounded(&mut launcher, Duration::ZERO)?;
                    // The CLI caller exits immediately after this function returns.
                    // Do not join a forwarding thread blocked on a dead terminal or
                    // stalled pipeline; process exit closes its bounded buffers.
                    return Ok(128 + signal);
                }
                thread::sleep(POLL);
            }
        })();
        if result.is_err() {
            let _ = cancel(directory, libc::SIGTERM);
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
        let caller = private_fifo(directory, "caller", true)?;
        // Keep diagnostics nonblocking on a separate open description from the
        // native stderr. Reporting an error must not delay native-child cleanup.
        let mut errors = private_fifo(directory, "stderr", true)?;
        let outcome = (|| {
            if let Some(signal) = cancellation(directory)? {
                return Ok(128 + signal);
            }
            if !caller_alive(&caller)? {
                return Ok(128 + libc::SIGTERM);
            }
            let mut child = Command::new(binary.canonicalize()?)
                .args(request.arguments.into_iter().map(OsString::from_vec))
                .envs(caller_environment(request.environment.into_iter().map(
                    |(name, value)| (OsString::from_vec(name), OsString::from_vec(value)),
                )))
                .current_dir(cwd)
                .stdin(Stdio::null())
                .stdout(blocking(private_fifo(directory, "stdout", true)?)?)
                .stderr(blocking(private_fifo(directory, "stderr", true)?)?)
                .process_group(0)
                .spawn()?;
            let result = (|| loop {
                if let Some(status) = child.try_wait()? {
                    return Ok(exit_code(status));
                }
                let signal = cancellation(directory)?.or(if caller_alive(&caller)? {
                    None
                } else {
                    Some(libc::SIGTERM)
                });
                if let Some(signal) = signal {
                    signal_child(&child, signal)?;
                    return wait_bounded(&mut child, Duration::from_secs(3)).map(exit_code);
                }
                thread::sleep(POLL);
            })();
            if result.is_err() {
                let _ = signal_child(&child, libc::SIGKILL);
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
        use std::{os::fd::FromRawFd, sync::Mutex};

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
            directory
        }

        fn shell(script: &str, cwd: &Path) -> tempfile::TempDir {
            exchange(&["-c".into(), script.into()], cwd)
        }

        fn wait_for(mut condition: impl FnMut() -> bool) {
            let deadline = Instant::now() + Duration::from_secs(10);
            while !condition() {
                assert!(Instant::now() < deadline, "CLI bridge timed out");
                thread::sleep(POLL);
            }
        }

        fn native_pid(cwd: &Path) -> libc::pid_t {
            let path = cwd.join("native.pid");
            wait_for(|| fs::read_to_string(&path).is_ok_and(|pid| pid.parse::<i32>().is_ok()));
            fs::read_to_string(path).unwrap().parse().unwrap()
        }

        fn assert_reaped(pid: libc::pid_t) {
            assert_eq!(
                unsafe { libc::kill(pid, 0) },
                -1,
                "native child still exists"
            );
            assert_eq!(io::Error::last_os_error().raw_os_error(), Some(libc::ESRCH));
        }

        // Keep a failed test from leaking the fake native process. The worker can
        // then observe its exit and reap it normally.
        struct NativeGuard(libc::pid_t);
        impl Drop for NativeGuard {
            fn drop(&mut self) {
                unsafe { libc::kill(-self.0, libc::SIGKILL) };
            }
        }

        #[derive(Clone, Default)]
        struct Capture(Arc<Mutex<Vec<u8>>>);
        impl Write for Capture {
            fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
                self.0.lock().unwrap().extend_from_slice(bytes);
                Ok(bytes.len())
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }

        const HELPER: &str = "cli::macos::tests::subprocess_helper";
        const MODE: &str = "GIZLOADER_CLI_TEST_MODE";
        const DIRECTORY: &str = "GIZLOADER_CLI_TEST_DIRECTORY";
        fn helper(mode: &str, directory: &Path) -> Command {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command
                .args(["--exact", HELPER, "--nocapture"])
                .env(MODE, mode)
                .env(DIRECTORY, directory)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            command
        }

        // Exercise the production worker/supervisor in separate processes. Signal
        // handlers never affect the test runner or parallel module tests.
        #[test]
        fn subprocess_helper() {
            let Some(mode) = std::env::var_os(MODE) else {
                return;
            };
            let directory = PathBuf::from(std::env::var_os(DIRECTORY).unwrap());
            let result = if mode == "worker" {
                worker(&directory, Path::new("/bin/sh"))
            } else {
                let _signals = Signals::install();
                let streams = Streams::create(&directory).unwrap();
                let launcher = helper("worker", &directory)
                    .process_group(0)
                    .spawn()
                    .unwrap();
                supervise(&directory, launcher, streams, io::stdout(), io::stderr())
            };
            std::process::exit(result.unwrap());
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
        fn worker_preserves_cwd_stream_bytes_raw_argv_environment_and_exit_status() {
            let cwd = tempfile::tempdir().unwrap();
            fs::write(cwd.path().join("relative"), b"\xfftail").unwrap();
            let directory = exchange(
                &[
                    "-c".into(),
                    "cat relative; printf '%s' \"$GIZLOADER_TEST_VALUE\" \"$1\"; printf '\\376diagnostic\\000' >&2; exit 7".into(),
                    "test".into(),
                    OsString::from_vec(b"\xff raw argument".to_vec()),
                ],
                cwd.path(),
            );
            let path = directory.path().canonicalize().unwrap();
            let streams = Streams::create(&path).unwrap();
            let launcher = helper("worker", &path).process_group(0).spawn().unwrap();
            let output = Capture::default();
            let errors = Capture::default();
            assert_eq!(
                supervise(&path, launcher, streams, output.clone(), errors.clone()).unwrap(),
                7
            );
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                7
            );
            assert_eq!(
                *output.0.lock().unwrap(),
                b"\xfftailrestored\xff raw argument"
            );
            assert_eq!(*errors.0.lock().unwrap(), b"\xfediagnostic\0");
        }

        #[test]
        fn sustained_output_is_exact_backpressured_and_has_no_disk_spool() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(
                "printf '%s' $$ > native.pid; printf '\\377'; /bin/dd if=/dev/zero bs=65536 count=64 2>/dev/null; printf '\\376' >&2; /bin/dd if=/dev/zero bs=65536 count=64 >&2 2>/dev/null; printf complete > completed",
                cwd.path(),
            );
            let path = directory.path().canonicalize().unwrap();
            let Streams {
                stdout,
                stderr,
                caller: _caller,
            } = Streams::create(&path).unwrap();
            let mut child = helper("worker", &path).process_group(0).spawn().unwrap();
            let native = NativeGuard(native_pid(cwd.path()));
            thread::sleep(Duration::from_millis(100));
            assert!(
                !cwd.path().join("completed").exists(),
                "writer bypassed backpressure"
            );
            for name in ["stdout", "stderr", "caller"] {
                let metadata = fs::symlink_metadata(path.join(name)).unwrap();
                assert!(metadata.file_type().is_fifo());
                assert_eq!(metadata.len(), 0);
            }
            let output = Capture::default();
            let errors = Capture::default();
            let mut forwarding = Forwarding::start(stdout, stderr, output.clone(), errors.clone());
            wait_for(|| child.try_wait().unwrap().is_some());
            assert_eq!(child.wait().unwrap().code(), Some(0));
            forwarding.finished.store(true, Ordering::Release);
            wait_for(|| forwarding.drained().unwrap());
            let output = output.0.lock().unwrap();
            let errors = errors.0.lock().unwrap();
            for (bytes, first) in [(&*output, 0xff), (&*errors, 0xfe)] {
                assert_eq!(bytes.len(), 1 + 64 * LIMIT as usize);
                assert_eq!(bytes[0], first);
                assert!(bytes[1..].iter().all(|byte| *byte == 0));
            }
            assert_eq!(fs::read(cwd.path().join("completed")).unwrap(), b"complete");
            let retained: u64 = fs::read_dir(&path)
                .unwrap()
                .map(|entry| {
                    let metadata = entry.unwrap().metadata().unwrap();
                    if metadata.is_file() {
                        metadata.len()
                    } else {
                        assert_eq!(metadata.len(), 0);
                        0
                    }
                })
                .sum();
            assert!(retained <= LIMIT, "output accumulated in bridge storage");
            assert_reaped(native.0);
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

        const MONITOR: &str = "exec 2>/dev/null; trap 'printf INT > received; exit 130' INT; trap 'printf TERM > received; exit 143' TERM; trap 'printf HUP > received; exit 129' HUP; printf '%s' $$ > native.pid; while :; do /bin/sleep 60; done";

        #[test]
        fn cancellation_marker_cooperatively_stops_and_reaps_the_native_child() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(MONITOR, cwd.path());
            let path = directory.path().canonicalize().unwrap();
            let _streams = Streams::create(&path).unwrap();
            let mut child = helper("worker", &path).process_group(0).spawn().unwrap();
            let native = NativeGuard(native_pid(cwd.path()));
            cancel(&path, libc::SIGTERM).unwrap();
            wait_for(|| child.try_wait().unwrap().is_some());
            assert_eq!(child.wait().unwrap().code(), Some(143));
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                143
            );
            assert_eq!(fs::read(cwd.path().join("received")).unwrap(), b"TERM");
            assert_reaped(native.0);
        }

        #[test]
        fn signals_cancel_and_reap_the_native_child_with_the_original_signal_code() {
            for (signal, received) in [
                (libc::SIGINT, "INT"),
                (libc::SIGTERM, "TERM"),
                (libc::SIGHUP, "HUP"),
            ] {
                let cwd = tempfile::tempdir().unwrap();
                let directory = shell(MONITOR, cwd.path());
                let path = directory.path().canonicalize().unwrap();
                let mut caller = helper("caller", &path).spawn().unwrap();
                let native = NativeGuard(native_pid(cwd.path()));
                assert_eq!(unsafe { libc::kill(caller.id() as i32, signal) }, 0);
                wait_for(|| caller.try_wait().unwrap().is_some());
                assert_eq!(caller.wait().unwrap().code(), Some(128 + signal));
                assert_eq!(read_json::<i32>(&path, "cancel").unwrap(), signal);
                assert_eq!(
                    read_json::<Completed>(&path, "result.json").unwrap().code,
                    128 + signal
                );
                assert_eq!(
                    fs::read(cwd.path().join("received")).unwrap(),
                    received.as_bytes()
                );
                assert_reaped(native.0);
            }
        }

        #[test]
        fn terminal_hangup_cancels_and_reaps_the_native_child() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(MONITOR, cwd.path());
            let path = directory.path().canonicalize().unwrap();
            let (mut master, mut slave) = (-1, -1);
            assert_eq!(
                unsafe {
                    libc::openpty(
                        &mut master,
                        &mut slave,
                        std::ptr::null_mut(),
                        std::ptr::null_mut(),
                        std::ptr::null_mut(),
                    )
                },
                0
            );
            let master = unsafe { File::from_raw_fd(master) };
            let slave = unsafe { File::from_raw_fd(slave) };
            // The child must not inherit the master, or closing our copy would not hang up.
            assert_eq!(
                unsafe { libc::fcntl(master.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) },
                0
            );
            let mut command = helper("caller", &path);
            command
                .stdin(slave.try_clone().unwrap())
                .stdout(slave.try_clone().unwrap())
                .stderr(slave);
            unsafe {
                command.pre_exec(|| {
                    if libc::setsid() == -1
                        || libc::ioctl(0, libc::TIOCSCTTY as libc::c_ulong, 0) == -1
                    {
                        return Err(io::Error::last_os_error());
                    }
                    Ok(())
                });
            }
            let mut caller = command.spawn().unwrap();
            let native = NativeGuard(native_pid(cwd.path()));
            drop(master);
            wait_for(|| caller.try_wait().unwrap().is_some());
            assert_eq!(caller.wait().unwrap().code(), Some(128 + libc::SIGHUP));
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                129
            );
            assert_eq!(fs::read(cwd.path().join("received")).unwrap(), b"HUP");
            assert_reaped(native.0);
        }

        #[test]
        fn caller_death_cancels_a_silent_native_child_and_reaps_it() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(MONITOR, cwd.path());
            let path = directory.path().canonicalize().unwrap();
            let mut caller = helper("caller", &path).spawn().unwrap();
            let native = NativeGuard(native_pid(cwd.path()));
            caller.kill().unwrap();
            caller.wait().unwrap();
            wait_for(|| path.join("result.json").exists());
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                143
            );
            assert_eq!(fs::read(cwd.path().join("received")).unwrap(), b"TERM");
            assert_reaped(native.0);
        }

        #[test]
        fn full_liveness_fifo_still_detects_caller_death_without_disk_growth() {
            let directory = tempfile::tempdir().unwrap();
            let reader = fifo(directory.path(), "caller").unwrap();
            let mut writer = private_fifo(directory.path(), "caller", true).unwrap();
            let mut retained = 0;
            loop {
                match writer.write(&[0; 4096]) {
                    Ok(count) => {
                        retained += count;
                        assert!(retained < 64 * LIMIT as usize);
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                    other => panic!("unexpected FIFO probe: {other:?}"),
                }
            }
            assert!(retained > 0);
            assert!(caller_alive(&writer).unwrap());
            assert_eq!(writer.metadata().unwrap().len(), 0);
            drop(reader);
            // Parallel subprocess tests can inherit the reader between fork and exec.
            // Wait for those close-on-exec copies, as the polling worker does.
            wait_for(|| !caller_alive(&writer).unwrap());
            assert_eq!(writer.metadata().unwrap().len(), 0);
        }

        #[test]
        fn stalled_stdout_consumer_cannot_block_signal_cancellation_or_native_reaping() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(
                "exec 2>/dev/null; trap 'exit 143' TERM; printf '%s' $$ > native.pid; /bin/dd if=/dev/zero bs=65536 count=64; printf complete > completed; while :; do /bin/sleep 60; done",
                cwd.path(),
            );
            let path = directory.path().canonicalize().unwrap();
            let mut caller = helper("caller", &path)
                .stdout(Stdio::piped())
                .spawn()
                .unwrap();
            let native = NativeGuard(native_pid(cwd.path()));
            thread::sleep(Duration::from_millis(100));
            assert!(!cwd.path().join("completed").exists());
            assert_eq!(unsafe { libc::kill(caller.id() as i32, libc::SIGTERM) }, 0);
            wait_for(|| path.join("result.json").exists());
            assert_reaped(native.0);
            wait_for(|| caller.try_wait().unwrap().is_some());
            assert_eq!(caller.wait().unwrap().code(), Some(143));
            assert_eq!(read_json::<i32>(&path, "cancel").unwrap(), libc::SIGTERM);
        }

        #[test]
        fn cancellation_kills_and_reaps_an_uncooperative_backpressured_child() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(
                "trap '' INT TERM HUP; printf '%s' $$ > native.pid; while :; do printf 'full buffer full buffer full buffer'; done",
                cwd.path(),
            );
            let path = directory.path().canonicalize().unwrap();
            let _streams = Streams::create(&path).unwrap();
            let mut child = helper("worker", &path).process_group(0).spawn().unwrap();
            let native = NativeGuard(native_pid(cwd.path()));
            cancel(&path, libc::SIGTERM).unwrap();
            wait_for(|| child.try_wait().unwrap().is_some());
            assert_eq!(child.wait().unwrap().code(), Some(128 + libc::SIGKILL));
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                137
            );
            assert_reaped(native.0);
        }

        #[test]
        fn cancellation_before_launch_does_not_spawn_a_native_child() {
            let cwd = tempfile::tempdir().unwrap();
            let directory = shell(MONITOR, cwd.path());
            let path = directory.path().canonicalize().unwrap();
            let _streams = Streams::create(&path).unwrap();
            cancel(&path, libc::SIGHUP).unwrap();
            assert_eq!(worker(&path, Path::new("/bin/sh")).unwrap(), 129);
            assert!(!cwd.path().join("native.pid").exists());
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                129
            );
        }

        #[test]
        fn private_streams_reject_regular_files_symlinks_and_public_permissions() {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().canonicalize().unwrap();
            create(&path, "regular").unwrap();
            assert!(private_fifo(&path, "regular", false).is_err());
            let _reader = fifo(&path, "stream").unwrap();
            std::os::unix::fs::symlink(path.join("stream"), path.join("symlink")).unwrap();
            assert!(private_fifo(&path, "symlink", true).is_err());
            assert!(private_fifo(&path, "symlink", false).is_err());
            fs::set_permissions(path.join("stream"), fs::Permissions::from_mode(0o644)).unwrap();
            assert!(private_fifo(&path, "stream", true).is_err());
            assert!(private_fifo(&path, "stream", false).is_err());
        }

        #[test]
        fn cancellation_and_result_remain_atomic_private_regular_files() {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().canonicalize().unwrap();
            assert_eq!(cancellation(&path).unwrap(), None);
            for signal in INTERRUPTS {
                cancel(&path, signal).unwrap();
                assert_eq!(cancellation(&path).unwrap(), Some(signal));
                let metadata = fs::symlink_metadata(path.join("cancel")).unwrap();
                assert!(metadata.is_file());
                assert_eq!(metadata.mode() & 0o077, 0);
            }
            cancel(&path, libc::SIGKILL).unwrap();
            assert!(cancellation(&path).is_err());
            fs::remove_file(path.join("cancel")).unwrap();
            let target = path.join("untouched");
            fs::write(&target, b"untouched").unwrap();
            std::os::unix::fs::symlink(&target, path.join("result.json")).unwrap();
            publish(&path, "result.json", &Completed { code: 7 }).unwrap();
            assert_eq!(fs::read(target).unwrap(), b"untouched");
            assert_eq!(
                read_json::<Completed>(&path, "result.json").unwrap().code,
                7
            );
            assert!(!fs::symlink_metadata(path.join("result.json"))
                .unwrap()
                .file_type()
                .is_symlink());
        }
    }
}
