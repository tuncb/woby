const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createCiConfig } = require('./ci-config.cjs');

function inputs() {
    return {
        manifest: { name: 'woby', 'version-string': '0.7.1', dependencies: [
            'sdl3', { name: 'bgfx', features: ['tools'] },
        ] },
        revision: 'a'.repeat(40), compiler: 'MSVC 19.51;SDK 26100',
        os: 'Windows', arch: 'X64', image: 'windows-2025/20260901',
        triplet: 'woby-ci-x64-windows',
        triplets: { 'woby-ci-x64-windows.cmake': 'set(VCPKG_BUILD_TYPE release)\n' },
    };
}

test('CI manifest pins dependencies without changing the source manifest', () => {
    const input = inputs();
    const before = structuredClone(input);
    const config = createCiConfig(input);
    assert.equal(config.manifest['builtin-baseline'], input.revision);
    assert.equal(config.manifest['version-string'], '0.7.1');
    assert.deepEqual(config.manifest.dependencies, input.manifest.dependencies);
    assert.deepEqual(input, before);
});

test('application version bumps preserve the dependency cache key', () => {
    const before = inputs();
    const after = inputs();
    after.manifest['version-string'] = '99.0.0';
    after.manifest['port-version'] = 4;
    assert.equal(createCiConfig(before).key, createCiConfig(after).key);
});

test('dependency changes produce a new key while retaining the compatible restore prefix', () => {
    const before = inputs();
    const after = inputs();
    after.manifest.dependencies.push({ name: 'fmt', 'version>=': '12.0.0' });
    assert.notEqual(createCiConfig(before).key, createCiConfig(after).key);
    assert.equal(createCiConfig(before).restorePrefix, createCiConfig(after).restorePrefix);
});

test('feature and override changes invalidate the dependency cache', () => {
    const baseline = createCiConfig(inputs()).key;
    const featureChange = inputs();
    featureChange.manifest.dependencies[1].features.push('multithreaded');
    assert.notEqual(createCiConfig(featureChange).key, baseline);
    const overrideChange = inputs();
    overrideChange.manifest.overrides = [{ name: 'fmt', version: '12.0.0' }];
    assert.notEqual(createCiConfig(overrideChange).key, baseline);
});

test('compiler, SDK, vcpkg revision, image, and included triplet changes invalidate cache identity', () => {
    const prefix = createCiConfig(inputs()).restorePrefix;
    for (const change of [
        input => { input.compiler += ';SDK 27000'; },
        input => { input.revision = 'b'.repeat(40); },
        input => { input.image += '-updated'; },
        input => { input.triplets['woby-x64-linux-dynamic.cmake'] = 'changed'; },
    ]) {
        const input = inputs();
        change(input);
        assert.notEqual(createCiConfig(input).restorePrefix, prefix);
    }
});

test('JSON key order and checkout line endings do not change cache identity', () => {
    const before = inputs();
    const after = inputs();
    after.manifest.dependencies[1] = { features: ['tools'], name: 'bgfx' };
    after.triplets['woby-ci-x64-windows.cmake'] = 'set(VCPKG_BUILD_TYPE release)\r\n';
    after.revision += '\r\n';
    assert.equal(createCiConfig(before).key, createCiConfig(after).key);
});

test('invalid pin, absent compiler, and unknown triplet fail before a cache can be used', () => {
    for (const change of [
        input => { input.revision = 'main'; },
        input => { input.compiler = ''; },
        input => { input.triplet = 'missing'; },
    ]) {
        const input = inputs();
        change(input);
        assert.throws(() => createCiConfig(input));
    }
});
