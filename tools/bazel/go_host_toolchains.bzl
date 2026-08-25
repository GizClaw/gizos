"""Root-registerable Go host toolchains owned by the GizOS module."""

load(
    "@io_bazel_rules_go//go/private:sdk.bzl",
    "go_download_sdk_rule",
    "go_multiple_toolchains",
)

_GO_VERSION = "1.24.12"
_SDK_ARCHIVES = {
    "darwin_amd64": [
        "go1.24.12.darwin-amd64.tar.gz",
        "4b9cc6771b56645da35a83a5424ae507f3250829b0d227e75f57b73e72da1f76",
    ],
    "darwin_arm64": [
        "go1.24.12.darwin-arm64.tar.gz",
        "098d0c039357c3652ec6c97d5451bc4dc24f7cf30ed902373ed9a8134aab2d29",
    ],
    "linux_amd64": [
        "go1.24.12.linux-amd64.tar.gz",
        "bddf8e653c82429aea7aec2520774e79925d4bb929fe20e67ecc00dd5af44c50",
    ],
    "linux_arm64": [
        "go1.24.12.linux-arm64.tar.gz",
        "4e02e2979e53b40f3666bba9f7e5ea0b99ea5156e0824b343fd054742c25498d",
    ],
    "windows_amd64": [
        "go1.24.12.windows-amd64.zip",
        "20e4bb6417117150d486181b16eaea4f1a9e7d8a2407a77da65d2b4e28dca53d",
    ],
    "windows_arm64": [
        "go1.24.12.windows-arm64.zip",
        "752e862a4479c7f5b231c2cdc7b5d33d2e7ac71fbe5d9eab3121b2f991090cbc",
    ],
}
_HOST_PLATFORMS = [
    ("darwin", "amd64"),
    ("darwin", "arm64"),
    ("linux", "amd64"),
    ("linux", "arm64"),
    ("windows", "amd64"),
    ("windows", "arm64"),
]

def _go_host_toolchains_impl(module_ctx):
    sdk_repos = []
    prefixes = []
    geese = []
    goarchs = []

    for index, (goos, goarch) in enumerate(_HOST_PLATFORMS):
        platform = "{}_{}".format(goos, goarch)
        repo_name = "gizos_go_sdk_{}".format(platform)
        go_download_sdk_rule(
            name = repo_name,
            goos = goos,
            goarch = goarch,
            sdks = _SDK_ARCHIVES,
            version = _GO_VERSION,
        )
        sdk_repos.append(repo_name)
        prefixes.append("_000{}_{}_".format(index, repo_name))
        geese.append(goos)
        goarchs.append(goarch)

    go_multiple_toolchains(
        name = "gizos_go_toolchains",
        prefixes = prefixes,
        geese = geese,
        goarchs = goarchs,
        sdk_repos = sdk_repos,
        sdk_types = ["remote"] * len(sdk_repos),
        sdk_versions = [_GO_VERSION] * len(sdk_repos),
    )

go_host_toolchains = module_extension(
    implementation = _go_host_toolchains_impl,
)
