/**
 * Platform gate for the embedded MPV frame-copy engine, shared by the
 * main-process bootstrap (main.ts), EmbeddedMpvNativeService and
 * EmbeddedMpvFrameCopyAdapter so the checks cannot drift. Keep this module
 * dependency-free: main.ts evaluates it at module top level, before
 * app.whenReady().
 *
 * macOS: Apple Silicon only (owner decision 2026-07-10) — Intel Macs keep
 * the docked native engine. Linux: any arch — the helper renders offscreen
 * through headless EGL and links libmpv out of process, so neither window
 * embedding nor the in-process-libmpv ban constrains it. Windows: not
 * ported yet.
 */
export function isFrameCopyPlatformSupported(): boolean {
    return (
        process.platform === 'linux' ||
        (process.platform === 'darwin' && process.arch === 'arm64')
    );
}
