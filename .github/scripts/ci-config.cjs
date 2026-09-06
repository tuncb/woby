const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');

function canonicalJson(value) {
    if (Array.isArray(value)) return `[${value.map(canonicalJson).join(',')}]`;
    if (value !== null && typeof value === 'object') {
        return `{${Object.keys(value).sort().map(key =>
            `${JSON.stringify(key)}:${canonicalJson(value[key])}`).join(',')}}`;
    }
    return JSON.stringify(value);
}

function createCiConfig({ manifest, revision, triplets, compiler, os, arch, triplet, image }) {
    revision = revision.trim();
    if (!/^[a-f0-9]{40}$/.test(revision)) throw new Error('Expected a full vcpkg commit SHA.');
    if (!compiler.trim()) throw new Error('Missing CI compiler identity.');
    if (!triplets[`${triplet}.cmake`]) throw new Error(`Unknown CI triplet: ${triplet}`);

    const ciManifest = { ...manifest, 'builtin-baseline': revision };
    const dependencyManifest = { ...ciManifest };
    // The root package's version does not change any dependency binaries.
    for (const key of ['version', 'version-string', 'version-semver', 'version-date', 'port-version']) {
        delete dependencyManifest[key];
    }
    const normalizedTriplets = Object.fromEntries(Object.entries(triplets).map(([name, text]) =>
        [name, text.replace(/\r\n/g, '\n')]));
    const toolchainHash = createHash('sha256').update(canonicalJson({
        revision, compiler: compiler.trim().replace(/\r\n/g, '\n'), os, arch, triplet, image,
        triplets: normalizedTriplets,
    })).digest('hex');
    const dependencyHash = createHash('sha256').update(canonicalJson(dependencyManifest)).digest('hex');
    const restorePrefix = `vcpkg-ci-v1-${os}-${arch}-${triplet}-${toolchainHash}-`;
    return { manifest: ciManifest, revision, key: restorePrefix + dependencyHash, restorePrefix };
}

function main() {
    const root = process.env.GITHUB_WORKSPACE || process.cwd();
    const tripletDirectory = path.join(root, 'cmake/vcpkg-triplets');
    const triplets = Object.fromEntries(fs.readdirSync(tripletDirectory)
        .filter(name => name.endsWith('.cmake')).map(name =>
            [name, fs.readFileSync(path.join(tripletDirectory, name), 'utf8')]));
    const config = createCiConfig({
        manifest: JSON.parse(fs.readFileSync(path.join(root, 'vcpkg.json'), 'utf8')),
        revision: fs.readFileSync(path.join(root, 'cmake/ci-vcpkg-revision.txt'), 'utf8'),
        triplets, compiler: process.env.WOBY_CI_COMPILER_ID || '',
        os: process.env.RUNNER_OS, arch: process.env.RUNNER_ARCH,
        triplet: process.env.WOBY_CI_TRIPLET,
        image: `${process.env.ImageOS || ''}/${process.env.ImageVersion || ''}`,
    });
    const manifestDirectory = path.join(root, 'build/ci-vcpkg');
    fs.mkdirSync(manifestDirectory, { recursive: true });
    fs.writeFileSync(path.join(manifestDirectory, 'vcpkg.json'), JSON.stringify(config.manifest, null, 2) + '\n');
    fs.appendFileSync(process.env.GITHUB_OUTPUT,
        `revision=${config.revision}\ncache-key=${config.key}\nrestore-prefix=${config.restorePrefix}\n`);
}

module.exports = { createCiConfig };
if (require.main === module) main();
