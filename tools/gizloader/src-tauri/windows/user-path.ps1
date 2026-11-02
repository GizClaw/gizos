param([ValidateSet('install','uninstall')][string]$Action, [string]$Directory)
$ErrorActionPreference = 'Stop'
$environment = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment')
$ownership = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\GizClaw\Gizloader')
try {
  $value = [string]$environment.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
  $entries = @($value.Split(';'))
  $present = @($entries | Where-Object { $_.TrimEnd('\') -ieq $Directory.TrimEnd('\') }).Count -gt 0
  if ($Action -eq 'install' -and -not $present) {
    $updated = if ($value) { $value + ';' + $Directory } else { $Directory }
    $environment.SetValue('Path', $updated, [Microsoft.Win32.RegistryValueKind]::ExpandString)
    $ownership.SetValue('AddedToPath', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
  } elseif ($Action -eq 'uninstall' -and $ownership.GetValue('AddedToPath', 0) -eq 1) {
    $updated = ($entries | Where-Object { $_.TrimEnd('\') -ine $Directory.TrimEnd('\') }) -join ';'
    $environment.SetValue('Path', $updated, [Microsoft.Win32.RegistryValueKind]::ExpandString)
    $ownership.DeleteValue('AddedToPath', $false)
  }
} finally {
  $environment.Dispose()
  $ownership.Dispose()
}
