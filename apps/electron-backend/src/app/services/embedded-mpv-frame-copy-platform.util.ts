import { app } from 'electron';
import { accessSync, constants as fsConstants } from 'fs';
import path from 'path';

/**
 * Platform gate + helper discovery for the embedded MPV frame-copy engine,
 * shared by the main-process bootstrap (main.ts), EmbeddedMpvNativeService
 * and EmbeddedMpvFrameCopyAdapter so the checks cannot drift. Everything
 * here must stay callable at module top level, before app.whenReady() —
 * main.ts uses it to decide the window sandbox.
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

function dedupeDefinedPaths(paths: Array<string | undefined>): string[] {
    return [
        ...new Set(paths.filter((value): value is string => Boolean(value))),
    ];
}

/**
 * Candidate locations of the embedded MPV native addon, most likely first.
 * The frame-copy helper and shm reader sit next to whichever addon wins.
 */
export function getEmbeddedMpvAddonCandidatePaths(): string[] {
    const localBuildAddonPath = path.resolve(
        process.cwd(),
        'apps/electron-backend/native/build/Release/embedded_mpv.node'
    );
    const distAddonPaths = [
        path.resolve(__dirname, 'native/embedded_mpv.node'),
        path.resolve(__dirname, '../../native/embedded_mpv.node'),
    ];
    const packagedAddonPaths = [
        path.resolve(
            (process as NodeJS.Process & { resourcesPath?: string })
                .resourcesPath ?? '',
            'app.asar.unpacked',
            'electron-backend',
            'native',
            'embedded_mpv.node'
        ),
        app.getAppPath()
            ? path.join(
                  path.dirname(app.getAppPath()),
                  'app.asar.unpacked',
                  'electron-backend',
                  'native',
                  'embedded_mpv.node'
              )
            : undefined,
    ];

    return dedupeDefinedPaths(
        app.isPackaged
            ? [...packagedAddonPaths, ...distAddonPaths, localBuildAddonPath]
            : [localBuildAddonPath, ...distAddonPaths, ...packagedAddonPaths]
    );
}

/**
 * First executable frame-copy helper binary next to an addon candidate, or
 * null. Requires the execute bit, not just existence: webpack's dist asset
 * copy drops file modes, and spawning a 0644 helper fails with EACCES — a
 * non-executable candidate must read as "unavailable" so the engine falls
 * back to native instead of erroring.
 */
export function resolveFrameCopyHelperPath(): string | null {
    const candidates = getEmbeddedMpvAddonCandidatePaths().map(
        (candidatePath) =>
            path.join(path.dirname(candidatePath), 'iptvnator_mpv_helper')
    );
    return (
        candidates.find((candidate) => {
            try {
                accessSync(candidate, fsConstants.X_OK);
                return true;
            } catch {
                return false;
            }
        }) ?? null
    );
}
