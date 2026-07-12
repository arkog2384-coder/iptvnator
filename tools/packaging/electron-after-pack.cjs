const linuxAfterPack = require('./linux-after-pack.cjs');
const {
    isForeignLinuxEmbeddedMpvArch,
    resolveElectronBuilderArchName,
    validatePackagedEmbeddedMpv,
} = require('./embedded-mpv-packaging.cjs');
const fs = require('fs');
const path = require('path');

function log(message) {
    console.log(`  - ${message}`);
}

function isTruthy(value) {
    return ['1', 'true', 'yes', 'on'].includes(
        String(value ?? '')
            .trim()
            .toLowerCase()
    );
}

function copyEmbeddedMpvNativeOutput(resourceDir, projectDir, platform) {
    const sourceDir = path.join(
        projectDir,
        'dist',
        'apps',
        'electron-backend',
        'native'
    );

    if (!fs.existsSync(sourceDir)) {
        return;
    }

    const destinationDir = path.join(
        resourceDir,
        'app.asar.unpacked',
        'electron-backend',
        'native'
    );

    fs.rmSync(destinationDir, { recursive: true, force: true });
    fs.cpSync(sourceDir, destinationDir, { recursive: true });

    const frameCopyHelperFile = path.join(
        destinationDir,
        platform === 'win32'
            ? 'iptvnator_mpv_helper.exe'
            : 'iptvnator_mpv_helper'
    );
    if (platform === 'linux') {
        // Dev-mode-only for now: the Linux frame-copy helper links the
        // build host's system libmpv, which packaged apps cannot assume is
        // installed. Ship it only once the Linux bundled-libmpv runtime
        // staging lands (spikes/mpv-frame-copy/PORTING.md milestone 4); the
        // support probe treats a missing helper as frame-copy-unavailable.
        fs.rmSync(frameCopyHelperFile, { force: true });
    } else if (platform !== 'win32' && fs.existsSync(frameCopyHelperFile)) {
        // The webpack dist asset copy drops file modes, so the helper
        // arrives here as 0644 — restore the execute bit or the packaged
        // engine cannot spawn it. (Windows has no execute bit.)
        fs.chmodSync(frameCopyHelperFile, 0o755);
    }
}

function writeEmbeddedMpvUnavailableMarker(resourceDir, targetArch) {
    const destinationDir = path.join(
        resourceDir,
        'app.asar.unpacked',
        'electron-backend',
        'native'
    );

    fs.rmSync(destinationDir, { recursive: true, force: true });
    fs.mkdirSync(destinationDir, { recursive: true });
    fs.writeFileSync(
        path.join(destinationDir, 'embedded-mpv-unavailable.txt'),
        `Embedded MPV is not bundled for ${targetArch} Linux builds yet. The built-in player and external MPV/VLC remain available.\n`
    );
}

async function afterPackHook(params) {
    await linuxAfterPack(params);

    const requireEmbeddedMpv = isTruthy(
        process.env.IPTVNATOR_REQUIRE_EMBEDDED_MPV
    );
    log(
        requireEmbeddedMpv
            ? `validating required embedded MPV ${params.electronPlatformName} runtime`
            : `validating optional embedded MPV ${params.electronPlatformName} runtime`
    );
    const resourceDir = getResourceDir(params);

    const foreignArch = isForeignLinuxEmbeddedMpvArch(
        params.electronPlatformName,
        params.arch
    );
    if (foreignArch) {
        const targetArch = resolveElectronBuilderArchName(params.arch);
        log(
            `embedded MPV addon is not built for ${targetArch}; packaging an unavailable marker instead`
        );
        writeEmbeddedMpvUnavailableMarker(resourceDir, targetArch);
    } else {
        copyEmbeddedMpvNativeOutput(
            resourceDir,
            params.packager.projectDir ?? process.cwd(),
            params.electronPlatformName
        );
    }

    const errors = validatePackagedEmbeddedMpv(resourceDir, {
        platform: params.electronPlatformName,
        required: requireEmbeddedMpv,
        foreignArch,
    });

    if (errors.length > 0) {
        throw new Error(
            [
                `Embedded MPV ${params.electronPlatformName} package validation failed.`,
                ...errors.map((error) => `- ${error}`),
            ].join('\n')
        );
    }

    log(`embedded MPV ${params.electronPlatformName} runtime validated`);
}

function getResourceDir(params) {
    if (params.electronPlatformName !== 'darwin') {
        return path.join(params.appOutDir, 'resources');
    }

    const appPath = params.appOutDir.endsWith('.app')
        ? params.appOutDir
        : fs
              .readdirSync(params.appOutDir)
              .find((entry) => entry.endsWith('.app'));

    return appPath
        ? path.join(
              params.appOutDir.endsWith('.app')
                  ? params.appOutDir
                  : path.join(params.appOutDir, appPath),
              'Contents',
              'Resources'
          )
        : params.appOutDir;
}

module.exports = afterPackHook;
