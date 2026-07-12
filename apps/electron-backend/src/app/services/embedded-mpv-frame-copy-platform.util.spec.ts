import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'fs';
import { tmpdir } from 'os';
import path from 'path';

jest.mock('electron', () => ({
    app: {
        isPackaged: false,
        getAppPath: () => '',
    },
}));

import {
    isFrameCopyPlatformSupported,
    resolveFrameCopyHelperPath,
} from './embedded-mpv-frame-copy-platform.util';

describe('embedded-mpv-frame-copy-platform.util', () => {
    describe('isFrameCopyPlatformSupported', () => {
        const originalPlatform = process.platform;
        const originalArch = process.arch;

        afterEach(() => {
            Object.defineProperty(process, 'platform', {
                value: originalPlatform,
            });
            Object.defineProperty(process, 'arch', { value: originalArch });
        });

        it.each<[NodeJS.Platform, string, boolean]>([
            ['darwin', 'arm64', true],
            ['darwin', 'x64', false],
            ['linux', 'x64', true],
            ['linux', 'arm64', true],
            ['win32', 'x64', true],
            ['freebsd', 'x64', false],
        ])('%s/%s -> %s', (platform, arch, expected) => {
            Object.defineProperty(process, 'platform', { value: platform });
            Object.defineProperty(process, 'arch', { value: arch });
            expect(isFrameCopyPlatformSupported()).toBe(expected);
        });
    });

    describe('resolveFrameCopyHelperPath', () => {
        let tempDir: string;
        let cwdSpy: jest.SpyInstance<string, []>;

        const releaseDir = () =>
            path.join(
                tempDir,
                'apps',
                'electron-backend',
                'native',
                'build',
                'Release'
            );
        // The resolver looks for the host platform's binary name, so the
        // fixture must follow it for the spec to stay host-agnostic.
        const helperPath = () =>
            path.join(
                releaseDir(),
                process.platform === 'win32'
                    ? 'iptvnator_mpv_helper.exe'
                    : 'iptvnator_mpv_helper'
            );

        beforeEach(() => {
            tempDir = mkdtempSync(path.join(tmpdir(), 'impv-fc-util-'));
            mkdirSync(releaseDir(), { recursive: true });
            cwdSpy = jest.spyOn(process, 'cwd').mockReturnValue(tempDir);
        });

        afterEach(() => {
            cwdSpy.mockRestore();
            rmSync(tempDir, { recursive: true, force: true });
        });

        it('returns null when no helper binary exists', () => {
            expect(resolveFrameCopyHelperPath()).toBeNull();
        });

        it('returns the helper next to the local-build addon when executable', () => {
            writeFileSync(helperPath(), '#!/bin/sh\n');
            chmodSync(helperPath(), 0o755);

            expect(resolveFrameCopyHelperPath()).toBe(helperPath());
        });

        // POSIX-only semantics: the webpack dist asset copy drops file
        // modes, and a 0644 helper must read as "unavailable" (spawn would
        // fail with EACCES). Windows has no execute bit.
        (process.platform === 'win32' ? it.skip : it)(
            'ignores a helper without the execute bit',
            () => {
                writeFileSync(helperPath(), '#!/bin/sh\n');
                chmodSync(helperPath(), 0o644);

                expect(resolveFrameCopyHelperPath()).toBeNull();
            }
        );
    });
});
