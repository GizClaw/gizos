// Tauri environment variables override bundle configuration, including empty ones.
export function tauriEnvironment(environment, platform = process.platform) {
  const child = { ...environment };
  if (platform === 'darwin') {
    child.APPLE_SIGNING_IDENTITY = environment.APPLE_SIGNING_IDENTITY || '-';
    if (environment.APPLE_API_KEY) {
      child.APPLE_API_KEY_PATH = environment.APPLE_API_KEY;
      child.APPLE_API_KEY = environment.APPLE_API_KEY_ID;
    }
  }
  return child;
}
