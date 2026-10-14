/* Exercise the actual script extracted from the compiled firmware page. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const script = fs.readFileSync(process.argv[2], 'utf8');
const ANIMATION_COUNT = 33;

function element() {
  return {
    disabled: false,
    checked: false,
    textContent: '',
    value: '',
    files: [],
    dataset: {},
    listeners: {},
    children: [],
    addEventListener(name, callback) { this.listeners[name] = callback; },
    replaceChildren(...children) { this.children = children; },
  };
}

function response(enabled, figure = true) {
  return { ok: true, async json() { return { animation_enabled: enabled, stick_figure_enabled: figure }; } };
}

function lightingResponse(gpios = [45, 46], available = [1, 2, 3, 45, 46]) {
  return { ok: true, async json() { return { gpios, available_gpios: available }; } };
}

function statusResponse(id = 0, pending = false, name = null) {
  return { ok: true, async json() {
    return { animation_id: id, running: id !== 0, pending, name: id ? name || 'Animation ' + id : null };
  } };
}

function animationResponse(count = ANIMATION_COUNT) {
  return { ok: true, async json() {
    return Array.from({ length: count }, (_, index) => ({ id: index + 1, name: 'Animation ' + (index + 1) }));
  } };
}

function textResponse(text, ok = true) {
  return { ok, async text() { return text; } };
}


function urlPage(uploadsEnabled, initialResponse, initialUrl) {
  return page(uploadsEnabled, initialResponse, animationResponse(), initialUrl);
}

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function page(uploadsEnabled, initialResponse, initialCatalog = animationResponse(), initialUrl = textResponse(''), initialLighting = lightingResponse()) {
  const elements = Object.fromEntries(
    ['animation', 'stick-figure', 'settings-status', 'upload', 'submit', 'status', 'firmware', 'password',
      'change-password', 'current-password', 'new-password', 'confirm-password',
      'save-password', 'password-status', 'test-price-increase', 'test-price-decrease',
      'price-test-status', 'animation-choice', 'run-animation', 'animation-test-status',
      'price-url-form', 'price-url', 'price-url-password', 'save-price-url', 'price-url-status',
      'show-password', 'show-password-status', 'lighting-gpios-form', 'lighting-gpios',
      'save-lighting-gpios', 'lighting-gpios-status', 'lighting-gpios-options', 'lighting-gpios-password']
      .map(name => [name, element()]),
  );
  elements.animation.disabled = true;
  elements['stick-figure'].disabled = true;
  elements['settings-status'].textContent = 'Loading settings…';
  elements['animation-choice'].disabled = elements['run-animation'].disabled = true;
  elements.upload.dataset.enabled = uploadsEnabled ? '1' : '0';
  for (const id of ['price-url', 'price-url-password', 'save-price-url']) elements[id].disabled = true;
  for (const control of Object.values(elements)) {
    Object.defineProperty(control, 'innerHTML', {
      set() { throw new Error('Device data must not be inserted as HTML'); },
    });
  }
  elements['lighting-gpios'].disabled = elements['lighting-gpios-password'].disabled = elements['save-lighting-gpios'].disabled = true;
  const queue = [initialResponse, initialCatalog, initialUrl, initialLighting];
  const calls = [];
  const timers = new Map();
  let nextTimer = 1;
  const context = vm.createContext({
    document: {
      getElementById(name) { assert(elements[name]); return elements[name]; },
      createElement(name) { assert.equal(name, 'option'); return element(); },
    },
    fetch(url, options = {}) {
      calls.push({ url, options });
      assert(queue.length, `Unexpected request: ${url}`);
      return Promise.resolve(queue.shift());
    },
    setTimeout(callback, delay) { const id = nextTimer++; timers.set(id, { callback, delay }); return id; },
    clearTimeout(id) { timers.delete(id); },
  });
  vm.runInContext(script, context);
  return { elements, queue, calls, timers, poll() {
    assert.equal(timers.size, 1, 'Only one animation status timer may be scheduled');
    const [id, timer] = timers.entries().next().value;
    timers.delete(id);
    return timer.callback();
  } };
}

async function settled() {
  await new Promise(setImmediate);
}

function assertFirmwareControls(elements, disabled) {
  for (const id of ['submit', 'firmware', 'password', 'current-password',
    'new-password', 'confirm-password', 'save-password', 'price-url',
    'price-url-password', 'save-price-url', 'show-password']) {
    assert.equal(elements[id].disabled, disabled, `${id} disabled state`);
  }
}

function assertPriceTestControls(elements, disabled) {
  for (const id of ['test-price-increase', 'test-price-decrease']) {
    assert.equal(elements[id].disabled, disabled, `${id} disabled state`);
  }
}

function assertAnimationTestControls(elements, disabled) {
  for (const id of ['animation-choice', 'run-animation']) {
    assert.equal(elements[id].disabled, disabled, `${id} disabled state`);
  }
}

function assertSettingsControls(elements, disabled) {
  assert.equal(elements.animation.disabled, disabled);
  assert.equal(elements['stick-figure'].disabled, disabled || !elements.animation.checked);
}

async function testAnimationPreferences() {
  for (const master of [false, true]) {
    for (const figure of [false, true]) {
      const controls = page(false, response(master, figure));
      await settled();
      const { elements, queue, calls } = controls;
      assertSettingsControls(elements, false);
      assert.equal(elements.animation.checked, master);
      assert.equal(elements['stick-figure'].checked, figure);
      assertAnimationTestControls(elements, !(master && figure));
      assertPriceTestControls(elements, false);
      if (!(master && figure)) {
        assert.match(elements['animation-test-status'].textContent, /Enable Animation and Stick figure animation/);
        const before = calls.length;
        await elements['run-animation'].listeners.click();
        await controls.poll();
        assert.equal(calls.length, before, 'Disabled scenes must neither run nor poll');
      }

      // Turning off the master grays the figure control without forgetting its preference.
      if (!master) {
        const before = calls.length;
        elements['stick-figure'].checked = !figure;
        await elements['stick-figure'].listeners.change();
        assert.equal(calls.length, before, 'A grayed figure toggle must reject synthetic change events');
        assert.equal(elements['stick-figure'].checked, figure);
        queue.push(response(true, figure));
        elements.animation.checked = true;
        await elements.animation.listeners.change();
        assert.equal(calls.at(-1).options.body, '1');
        assert.equal(elements['stick-figure'].checked, figure, 'Enabling the master must restore the saved figure preference');
        assertSettingsControls(elements, false);
      }
      queue.push(response(true, !figure));
      elements['stick-figure'].checked = !figure;
      await elements['stick-figure'].listeners.change();
      assert.equal(calls.at(-1).options.body, figure ? 'figure=0' : 'figure=1');
      assert.equal(elements.animation.checked, true);
      assert.equal(elements['stick-figure'].checked, !figure);
      assertSettingsControls(elements, false);
      assertAnimationTestControls(elements, figure);
      queue.push(response(false, !figure));
      elements.animation.checked = false;
      await elements.animation.listeners.change();
      assert.equal(calls.at(-1).options.body, '0');
      assert.equal(elements['stick-figure'].checked, !figure, 'Master saves must preserve the figure preference');
      assert.equal(elements['stick-figure'].disabled, true);
      assertAnimationTestControls(elements, true);
    }
  }

  const saving = page(true, response(true, true));
  await settled();
  enterPassword(saving.elements, 'new-password');
  saving.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  const slowSave = deferred();
  saving.queue.push(slowSave.promise);
  saving.elements.animation.checked = false;
  const pending = saving.elements.animation.listeners.change();
  assertSettingsControls(saving.elements, true);
  assertFirmwareControls(saving.elements, true);
  assertAnimationTestControls(saving.elements, true);
  const before = saving.calls.length;
  saving.elements['stick-figure'].checked = false;
  await saving.elements['stick-figure'].listeners.change();
  await saving.elements.animation.listeners.change();
  await saving.elements.upload.listeners.submit({ preventDefault() {} });
  await saving.elements['change-password'].listeners.submit({ preventDefault() {} });
  await saving.elements['run-animation'].listeners.click();
  assert.equal(saving.calls.length, before, 'A pending settings save must serialize both toggles and firmware operations');
  slowSave.resolve(response(false, true));
  await pending;
  assert.equal(saving.elements.animation.checked, false);
  assert.equal(saving.elements['stick-figure'].checked, true);
  assertSettingsControls(saving.elements, false);
  assertFirmwareControls(saving.elements, false);
  assertAnimationTestControls(saving.elements, true);

  const failures = [
    { ok: false, async text() { return 'Commit failed.'; } },
    { ok: true, async json() { return { animation_enabled: true }; } },
    response(true, 'invalid'),
    { ok: true, async json() { return null; } },
  ];
  for (const failure of failures) {
    saving.queue.push(failure);
    saving.elements.animation.checked = true;
    await saving.elements.animation.listeners.change();
    assert.equal(saving.elements.animation.checked, false);
    assert.equal(saving.elements['stick-figure'].checked, true);
    assertSettingsControls(saving.elements, false);
    assertAnimationTestControls(saving.elements, true);
  }

  const failedMasterOff = page(true, response(true, true));
  await settled();
  failedMasterOff.queue.push({ ok: false, async text() { return 'Commit failed.'; } });
  failedMasterOff.elements.animation.checked = false;
  await failedMasterOff.elements.animation.listeners.change();
  assert.equal(failedMasterOff.elements.animation.checked, true);
  assert.equal(failedMasterOff.elements['stick-figure'].checked, true);
  assertSettingsControls(failedMasterOff.elements, false);
  assertAnimationTestControls(failedMasterOff.elements, false);
  for (const failure of failures) {
    failedMasterOff.queue.push(failure);
    failedMasterOff.elements['stick-figure'].checked = false;
    await failedMasterOff.elements['stick-figure'].listeners.change();
    assert.equal(failedMasterOff.elements.animation.checked, true);
    assert.equal(failedMasterOff.elements['stick-figure'].checked, true);
    assertSettingsControls(failedMasterOff.elements, false);
  }

  for (const invalidSettings of [
    { animation_enabled: true },
    { animation_enabled: true, stick_figure_enabled: 1 },
    null,
  ]) {
    const invalid = page(false, { ok: true, async json() { return invalidSettings; } });
    await settled();
    assertSettingsControls(invalid.elements, true);
    assertAnimationTestControls(invalid.elements, true);
    assertPriceTestControls(invalid.elements, false);
    assert.match(invalid.elements['settings-status'].textContent, /Reload/);
  }

  // A settings response cannot enable either switch during a firmware operation.
  const slowSettings = deferred(), slowUpload = deferred();
  const loading = page(true, slowSettings.promise);
  loading.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  loading.queue.push(slowUpload.promise);
  const uploading = loading.elements.upload.listeners.submit({ preventDefault() {} });
  slowSettings.resolve(response(true, true));
  await settled();
  assertSettingsControls(loading.elements, true);
  const beforeDisabledSave = loading.calls.length;
  loading.elements.animation.checked = false;
  await loading.elements.animation.listeners.change();
  assert.equal(loading.calls.length, beforeDisabledSave);
  assert.equal(loading.elements.animation.checked, true);
  slowUpload.resolve({ ok: false, async text() { return 'Firmware rejected.'; } });
  await uploading;
  assertSettingsControls(loading.elements, false);
  assertAnimationTestControls(loading.elements, false);
}

async function testAnimationStatus() {
  const controls = page(true, response(true, true));
  await settled();
  const { elements, queue, calls } = controls;
  for (const [state, expected] of [
    [statusResponse(32), 'Playing: Animation 32.'],
    [statusResponse(33, false, 'Dig for treasure'), 'Playing: Dig for treasure.'],
    [statusResponse(), 'The figure is roaming.'],
    [statusResponse(1), 'Playing: Animation 1.'],
    [statusResponse(2), 'Playing: Animation 2.'],
    [statusResponse(0, true), 'Starting animation…'],
  ]) {
    queue.push(state);
    await controls.poll();
    assert.equal(calls.at(-1).url, '/api/animation');
    assert.equal(calls.at(-1).options.cache, 'no-store');
    assert.equal(calls.at(-1).options.method, undefined);
    assert.equal(elements['animation-test-status'].textContent, expected);
    assert.equal(controls.timers.values().next().value.delay, 1000);
  }

  // A poll started before a manual request cannot replace its latest status.
  const oldStatus = deferred();
  queue.push(oldStatus.promise);
  const checking = controls.poll();
  queue.push({ ok: true, async text() { return 'Read a newspaper'; } });
  await elements['run-animation'].listeners.click();
  assert.equal(elements['animation-test-status'].textContent, 'Starting: Read a newspaper.');
  const beforeDuplicatePoll = calls.length;
  await controls.poll();
  assert.equal(calls.length, beforeDuplicatePoll, 'Status polls must not overlap');
  oldStatus.resolve(statusResponse(1));
  await checking;
  assert.equal(elements['animation-test-status'].textContent, 'Starting: Read a newspaper.');
  queue.push(statusResponse(32));
  await controls.poll();
  assert.equal(elements['animation-test-status'].textContent, 'Playing: Animation 32.');
  queue.push(statusResponse());
  await controls.poll();
  assert.equal(elements['animation-test-status'].textContent, 'The figure is roaming.');

  const beforeFailedPost = deferred();
  queue.push(beforeFailedPost.promise);
  const checkingFailure = controls.poll();
  queue.push({ ok: false, async text() { return 'The display is busy.'; } });
  await elements['run-animation'].listeners.click();
  beforeFailedPost.resolve(statusResponse(3));
  await checkingFailure;
  assert.equal(elements['animation-test-status'].textContent, 'The display is busy.');

  // Failed polls clear a previously playing scene and retry on the next interval.
  for (const failure of [
    { ok: false },
    { ok: true, async json() { return { animation_id: 0, running: true, pending: false, name: null }; } },
    { ok: true, async json() { return { animation_id: ANIMATION_COUNT + 1, running: true, pending: false, name: 'Invalid' }; } },
    { ok: true, async json() { throw new Error('Disconnected'); } },
  ]) {
    queue.push(statusResponse(4)); await controls.poll();
    assert.match(elements['animation-test-status'].textContent, /Playing/);
    queue.push(failure); await controls.poll();
    assert.match(elements['animation-test-status'].textContent, /Could not check.*Retrying/);
    assert.doesNotMatch(elements['animation-test-status'].textContent, /Playing/);
  }

  const beforeOff = deferred();
  queue.push(beforeOff.promise);
  const checkingOff = controls.poll();
  queue.push(response(false, true));
  elements.animation.checked = false;
  await elements.animation.listeners.change();
  assert.match(elements['animation-test-status'].textContent, /Enable Animation/);
  beforeOff.resolve(statusResponse(5));
  await checkingOff;
  assert.match(elements['animation-test-status'].textContent, /Enable Animation/);
  const beforeDisabledPoll = calls.length;
  await controls.poll();
  assert.equal(calls.length, beforeDisabledPoll);
  queue.push(response(true, true));
  elements.animation.checked = true;
  await elements.animation.listeners.change();
  queue.push(statusResponse());
  await controls.poll();
  assert.equal(elements['animation-test-status'].textContent, 'The figure is roaming.');

  const beforeUpload = deferred(), slowUpload = deferred();
  queue.push(beforeUpload.promise);
  const checkingUpload = controls.poll();
  elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  queue.push(slowUpload.promise);
  const uploading = elements.upload.listeners.submit({ preventDefault() {} });
  beforeUpload.resolve(statusResponse(6));
  await checkingUpload;
  assert.equal(elements['animation-test-status'].textContent, 'The figure is roaming.');
  const beforeBusyPoll = calls.length;
  await controls.poll();
  assert.equal(calls.length, beforeBusyPoll);
  slowUpload.resolve({ ok: false, async text() { return 'Firmware rejected.'; } });
  await uploading;
  queue.push(statusResponse(7));
  await controls.poll();
  assert.equal(elements['animation-test-status'].textContent, 'Playing: Animation 7.');
}

function enterPassword(elements, replacement, confirmation = replacement) {
  elements['current-password'].value = 'current-test-password';
  elements['new-password'].value = replacement;
  elements['confirm-password'].value = confirmation;
}

async function testShowPassword() {
  const loading = deferred();
  const current = urlPage(true, response(true), loading.promise);
  await settled();
  assert.equal(current.elements['show-password'].disabled, false,
    'Physical password viewing must remain available while the price URL loads');
  assert(current.elements['price-url'].disabled);
  const revealing = deferred();
  current.queue.push(revealing.promise);
  const pending = current.elements['show-password'].listeners.click();
  assert.equal(current.elements['show-password-status'].textContent, 'Requesting…');
  assertFirmwareControls(current.elements, true);
  assertSettingsControls(current.elements, true);
  assertPriceTestControls(current.elements, true);
  assertAnimationTestControls(current.elements, true);
  const request = current.calls.at(-1);
  assert.equal(request.url, '/show-password');
  assert.equal(request.options.method, 'POST');
  assert.equal(request.options.headers['X-Display-Password'], '1');
  assert.equal(request.options.headers['X-OTA-Password'], undefined);
  assert.equal(request.options.body, undefined, 'The browser does not send or receive the password');
  const count = current.calls.length;
  await current.elements['show-password'].listeners.click();
  await current.elements.upload.listeners.submit({ preventDefault() {} });
  await current.elements['change-password'].listeners.submit({ preventDefault() {} });
  await current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assert.equal(current.calls.length, count, 'A reveal request must serialize all authenticated forms');
  loading.resolve(textResponse(''));
  await settled();
  assertFirmwareControls(current.elements, true);
  revealing.resolve(textResponse('Password is showing on the display.'));
  await pending;
  assert.equal(current.elements['show-password-status'].textContent, 'Password is showing on the display.');
  assertFirmwareControls(current.elements, false);
  assert.equal(current.elements.password.value, '');
  for (const message of ['The display is busy. Please try again shortly.', '<b>Could not show the password.</b>']) {
    current.queue.push(textResponse(message, false));
    await current.elements['show-password'].listeners.click();
    assert.equal(current.elements['show-password-status'].textContent, message);
    assertFirmwareControls(current.elements, false);
  }
  const lost = deferred();
  current.queue.push(lost.promise);
  const interrupted = current.elements['show-password'].listeners.click();
  lost.reject(new Error('connection lost'));
  await interrupted;
  assert.match(current.elements['show-password-status'].textContent, /Check the display or try again/);
  assertFirmwareControls(current.elements, false);

  const failedLoad = urlPage(true, response(true), textResponse('', false));
  await settled();
  assert(failedLoad.elements['price-url'].disabled);
  assert.equal(failedLoad.elements['show-password'].disabled, false);
  failedLoad.queue.push(textResponse('Password is showing on the display.'));
  await failedLoad.elements['show-password'].listeners.click();
  assert.equal(failedLoad.calls.at(-1).url, '/show-password');
  assert.equal(failedLoad.elements['show-password'].disabled, false);
  assert(failedLoad.elements['price-url'].disabled);
}

async function testPriceUrl() {
  const saved = 'https://example.test/price?pair=USD%2FILS&label="<script>alert(1)</script>';
  const loading = deferred();
  const current = urlPage(true, response(true), loading.promise);
  await settled();
  assert.equal(current.elements.animation.disabled, false, 'Display settings load independently');
  for (const id of ['price-url', 'price-url-password', 'save-price-url']) {
    assert(current.elements[id].disabled, 'Do not let loading race with saving an empty URL');
  }
  const initialCalls = current.calls.length;
  await current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assert.equal(current.calls.length, initialCalls);
  loading.resolve(textResponse(saved));
  await settled();
  assert.equal(current.elements['price-url'].value, saved, 'Stored URL punctuation must survive loading');
  assert.equal(current.elements['price-url-status'].textContent, '');
  assertFirmwareControls(current.elements, false);

  const requested = 'https://new.test/prices?pair=USD%2FILS&format=json';
  current.elements['price-url'].value = requested;
  current.elements['price-url-password'].value = 'same-firmware-password';
  const saving = deferred();
  current.queue.push(saving.promise);
  const pending = current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assertFirmwareControls(current.elements, true);
  assertSettingsControls(current.elements, true);
  assertPriceTestControls(current.elements, true);
  assertAnimationTestControls(current.elements, true);
  assert.equal(current.elements['price-url-status'].textContent, 'Saving…');
  const post = current.calls.at(-1);
  assert.equal(post.url, '/price-url');
  assert.equal(post.options.method, 'POST');
  assert.equal(post.options.body, requested);
  assert.equal(post.options.headers['Content-Type'], 'text/plain');
  assert.equal(post.options.headers['X-OTA-Password'], 'same-firmware-password');
  assert.equal(post.options.headers['X-Display-Settings'], undefined);
  const pendingCalls = current.calls.length;
  await current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  await current.elements.upload.listeners.submit({ preventDefault() {} });
  await current.elements['change-password'].listeners.submit({ preventDefault() {} });
  await current.elements['show-password'].listeners.click();
  assert.equal(current.calls.length, pendingCalls, 'URL writes serialize with all authenticated forms');
  saving.resolve(textResponse('Price URL saved.'));
  await pending;
  assert.equal(current.elements['price-url-status'].textContent, 'Saved.');
  assert.equal(current.elements['price-url-password'].value, '');
  assert.equal(current.elements['price-url'].value, requested);
  assertFirmwareControls(current.elements, false);

  const reloaded = urlPage(true, response(true), textResponse(requested));
  await settled();
  assert.equal(reloaded.elements['price-url'].value, requested);
  current.elements['price-url'].value = '';
  current.elements['price-url-password'].value = 'same-firmware-password';
  current.queue.push(textResponse('Price fetching disabled.'));
  await current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assert.equal(current.calls.at(-1).options.body, '', 'An empty URL disables fetching');
  assert.equal(current.elements['price-url'].value, '');
  assert.equal(current.elements['price-url-password'].value, '');
  assert.equal(current.elements['price-url-status'].textContent, 'Saved.');
  assertFirmwareControls(current.elements, false);

  const untrimmed = ' https://new.test/prices ';
  current.elements['price-url'].value = untrimmed;
  current.elements['price-url-password'].value = 'same-firmware-password';
  for (const message of ['Incorrect password.', 'The device could not save the URL.', '<b>Invalid URL.</b>']) {
    current.queue.push(textResponse(message, false));
    await current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
    assert.equal(current.calls.at(-1).options.body, untrimmed, 'The URL must not be silently rewritten');
    assert.equal(current.elements['price-url-status'].textContent, message);
    assert.equal(current.elements['price-url'].value, untrimmed);
    assert.equal(current.elements['price-url-password'].value, 'same-firmware-password');
    assertFirmwareControls(current.elements, false);
  }
  const disconnected = deferred();
  current.queue.push(disconnected.promise);
  const uncertain = current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assertFirmwareControls(current.elements, true);
  disconnected.reject(new Error('connection lost'));
  await uncertain;
  assert.match(current.elements['price-url-status'].textContent, /Could not confirm.*Reload/);
  assert.equal(current.elements['price-url'].value, untrimmed);
  assert.equal(current.elements['price-url-password'].value, 'same-firmware-password');
  assertFirmwareControls(current.elements, false);
  current.elements['price-url'].value = requested;
  current.queue.push(textResponse('Price URL saved.'));
  await current.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assert.equal(current.elements['price-url-status'].textContent, 'Saved.');
  assert.equal(current.elements['price-url-password'].value, '');

  const failedLoad = urlPage(true, response(false), textResponse('Cannot read price URL.', false));
  await settled();
  assert.equal(failedLoad.elements.animation.disabled, false);
  assert.match(failedLoad.elements['price-url-status'].textContent, /Reload/);
  for (const id of ['price-url', 'price-url-password', 'save-price-url']) assert(failedLoad.elements[id].disabled);
  const failedCalls = failedLoad.calls.length;
  await failedLoad.elements['price-url-form'].listeners.submit({ preventDefault() {} });
  assert.equal(failedLoad.calls.length, failedCalls);

  /* Finishing the initial URL load cannot unlock an ongoing password change. */
  const lateLoad = deferred();
  const busy = urlPage(true, response(false), lateLoad.promise);
  await settled();
  enterPassword(busy.elements, 'replacement-password');
  const changing = deferred();
  busy.queue.push(changing.promise);
  const passwordPending = busy.elements['change-password'].listeners.submit({ preventDefault() {} });
  lateLoad.resolve(textResponse(requested));
  await settled();
  assertFirmwareControls(busy.elements, true);
  changing.resolve(textResponse('Password changed.'));
  await passwordPending;
  assertFirmwareControls(busy.elements, false);
}

function assertLightingControls(elements, disabled) {
  assert.equal(elements['lighting-gpios'].disabled, disabled, 'GPIO input disabled state');
  assert.equal(elements['save-lighting-gpios'].disabled, disabled, 'GPIO save disabled state');
  assert.equal(elements['lighting-gpios-password'].disabled, disabled, 'GPIO password disabled state');
}

async function testLightingGpios() {
  for (const pins of [[45], [45, 46], [3, 45, 46], [1, 2, 3, 45, 46]]) {
    const current = page(true, response(true));
    await settled();
    const { elements, calls, queue } = current;
    assertLightingControls(elements, false);
    assert.equal(elements['lighting-gpios'].value, '45,46');
    assert.equal(elements['lighting-gpios-options'].textContent, 'Available GPIOs: 1, 2, 3, 45, 46.');
    assert.equal(calls[3].url, '/lighting-gpios');
    assert.equal(calls[3].options.cache, 'no-store');
    elements['lighting-gpios'].value = [...pins].reverse().join(', ');
    elements['lighting-gpios-password'].value = 'same-ui-password';
    queue.push(lightingResponse(pins));
    await elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
    assert.equal(calls.at(-1).url, '/lighting-gpios');
    assert.equal(calls.at(-1).options.method, 'POST');
    assert.equal(calls.at(-1).options.headers['Content-Type'], 'text/plain');
    assert.equal(calls.at(-1).options.headers['X-Display-Settings'], undefined);
    assert.equal(calls.at(-1).options.headers['X-OTA-Password'], 'same-ui-password');
    assert.equal(calls.at(-1).options.body, [...pins].reverse().join(', '));
    assert.equal(elements['lighting-gpios'].value, pins.join(','));
    assert.equal(elements['lighting-gpios-status'].textContent, 'Saved.');
    assertLightingControls(elements, false);
    assert.equal(elements['lighting-gpios-password'].value, '', 'Password clears after an attempted GPIO save');
    assertFirmwareControls(elements, false);
  }

  const slow = deferred();
  const current = page(true, response(true));
  await settled();
  const { elements, queue, calls } = current;
  elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  enterPassword(elements, 'replacement-password');
  elements['lighting-gpios'].value = '45';
  const blankCount = calls.length;
  await elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
  assert.equal(calls.length, blankCount, 'Blank GPIO password must be rejected before HTTP');
  assert.equal(elements['lighting-gpios-status'].textContent, 'Enter the password.');
  assertLightingControls(elements, false);
  elements['lighting-gpios-password'].value = 'same-ui-password';
  queue.push(slow.promise);
  const saving = elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
  assertLightingControls(elements, true);
  assertSettingsControls(elements, true);
  assertFirmwareControls(elements, true);
  assertAnimationTestControls(elements, true);
  assert.equal(elements['lighting-gpios-status'].textContent, 'Saving…');
  const count = calls.length;
  await elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
  elements.animation.checked = false;
  await elements.animation.listeners.change();
  await elements.upload.listeners.submit({ preventDefault() {} });
  await elements['change-password'].listeners.submit({ preventDefault() {} });
  await elements['show-password'].listeners.click();
  await elements['price-url-form'].listeners.submit({ preventDefault() {} });
  await elements['run-animation'].listeners.click();
  assert.equal(calls.length, count, 'GPIO save serializes all settings and firmware actions');
  slow.resolve(lightingResponse([45]));
  await saving;
  assert.equal(elements['lighting-gpios'].value, '45');
  assert.equal(elements['lighting-gpios-password'].value, '');
  assertLightingControls(elements, false);
  assertSettingsControls(elements, false);
  assertFirmwareControls(elements, false);

  for (const failure of [textResponse('Incorrect password.', false), textResponse('Use distinct available GPIOs.', false),
    textResponse('The device could not save the GPIOs.', false),
    { ok: true, async json() { return { gpios: [46, 45], available_gpios: [45, 46] }; } },
    { ok: true, async json() { return { gpios: [45, 45], available_gpios: [45, 46] }; } },
    { ok: true, async json() { return { gpios: [19], available_gpios: [45, 46] }; } },
    { ok: true, async json() { throw new Error('Invalid JSON'); } }]) {
    elements['lighting-gpios'].value = '19,45,45';
    elements['lighting-gpios-password'].value = 'attempt-password';
    queue.push(failure);
    await elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
    assert.notEqual(elements['lighting-gpios-status'].textContent, 'Saved.');
    assert.equal(elements['lighting-gpios'].value, '45', 'Failed or invalid saves restore confirmed GPIOs');
    assert.equal(elements['lighting-gpios-password'].value, '', 'Failed and malformed replies also clear the password');
    assertLightingControls(elements, false);
  }
  const uncertain = deferred();
  queue.push(uncertain.promise);
  elements['lighting-gpios'].value = '46';
  elements['lighting-gpios-password'].value = 'attempt-password';
  const failedSave = elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
  uncertain.reject(new Error('Connection interrupted'));
  await failedSave;
  assert.equal(elements['lighting-gpios'].value, '45');
  assert.match(elements['lighting-gpios-status'].textContent, /Could not confirm.*Reload/);
  assert.equal(elements['lighting-gpios-password'].value, '', 'Network failure clears the attempted password');

  const changing = deferred();
  queue.push(changing.promise);
  const change = elements['change-password'].listeners.submit({ preventDefault() {} });
  assertLightingControls(elements, true);
  const before = calls.length;
  await elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
  assert.equal(calls.length, before, 'Firmware operations block GPIO writes');
  changing.resolve(textResponse('Password changed.'));
  await change;
  assertLightingControls(elements, false);

  for (const invalid of [textResponse('Cannot read GPIOs.', false), lightingResponse([]),
    lightingResponse([45], []), lightingResponse([49], [49]), lightingResponse([45], [46]),
    lightingResponse([45], [46, 45]), lightingResponse(['45'])]) {
    const failed = page(true, response(true), animationResponse(), textResponse(''), invalid);
    await settled();
    assertLightingControls(failed.elements, true);
    assert.equal(failed.elements.animation.disabled, false, 'Other settings load independently');
    assert.match(failed.elements['lighting-gpios-status'].textContent, /Reload/);
    const before = failed.calls.length;
    await failed.elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
    assert.equal(failed.calls.length, before, 'Failed loading must keep GPIO saving disabled');
  }

  const disabled = page(false, response(true));
  await settled();
  assertLightingControls(disabled.elements, true);
  assert.equal(disabled.elements['lighting-gpios'].value, '45,46', 'GPIO GET remains public');
  assert.equal(disabled.elements['lighting-gpios-status'].textContent,
    'Password initialization failed. GPIO changes are disabled.');
  disabled.elements['lighting-gpios-password'].value = 'ignored-password';
  const disabledCount = disabled.calls.length;
  await disabled.elements['lighting-gpios-form'].listeners.submit({ preventDefault() {} });
  assert.equal(disabled.calls.length, disabledCount, 'Missing initialized password keeps GPIO writes disabled');

  const late = deferred();
  const raced = page(true, response(true), animationResponse(), textResponse(''), late.promise);
  await settled();
  enterPassword(raced.elements, 'replacement-password');
  const passwordChange = deferred();
  raced.queue.push(passwordChange.promise);
  const pending = raced.elements['change-password'].listeners.submit({ preventDefault() {} });
  late.resolve(lightingResponse([45]));
  await settled();
  assertLightingControls(raced.elements, true);
  assertSettingsControls(raced.elements, true);
  passwordChange.resolve(textResponse('Password changed.'));
  await pending;
  assertLightingControls(raced.elements, false);
  assert.equal(raced.elements['lighting-gpios'].value, '45');
}

async function test() {
  await testLightingGpios();
  await testAnimationPreferences();
  await testAnimationStatus();
  const loading = deferred();
  const controls = page(false, loading.promise);
  const { elements, queue, calls } = controls;
  assert(elements.animation.disabled);
  assert(elements.submit.disabled && elements.firmware.disabled && elements.password.disabled);
  assert.equal(elements.status.textContent, 'Password initialization failed. Uploads are disabled.');
  assertFirmwareControls(elements, true);
  assert.equal(calls[0].url, '/settings');
  assert.equal(calls[0].options.cache, 'no-store');
  loading.resolve(response(true));
  await settled();
  assert.equal(elements.animation.checked, true);
  assert.equal(elements['stick-figure'].checked, true);
  assert.equal(elements.animation.disabled, false);
  assert.equal(elements['stick-figure'].disabled, false);
  assert.equal(elements['settings-status'].textContent, '');
  assert.equal(elements['price-url-status'].textContent,
    'Password initialization failed. URL changes are disabled.');
  assert(elements.submit.disabled, 'Settings loading must not enable disabled firmware uploads');
  assertPriceTestControls(elements, false);

  // Every catalog entry is selectable without an upgrade password.
  assert.equal(calls[1].url, '/api/animations');
  assert.equal(calls[1].options.cache, 'no-store');
  assertAnimationTestControls(elements, false);
  assert.equal(elements['animation-choice'].children.length, ANIMATION_COUNT);
  assert.equal(elements['animation-choice'].value, '1');
  for (let index = 0; index < ANIMATION_COUNT; ++index) {
    assert.equal(elements['animation-choice'].children[index].value, String(index + 1));
    assert.equal(elements['animation-choice'].children[index].textContent, 'Animation ' + (index + 1));
  }
  elements['animation-choice'].value = '33';
  const running = deferred();
  queue.push(running.promise);
  const pendingAnimation = elements['run-animation'].listeners.click();
  assertAnimationTestControls(elements, true);
  assert.match(elements['animation-test-status'].textContent, /Starting/);
  const animationCall = calls.at(-1);
  assert.equal(animationCall.url, '/api/animation');
  assert.equal(animationCall.options.method, 'POST');
  assert.equal(animationCall.options.body, '33');
  assert.equal(animationCall.options.headers['Content-Type'], 'text/plain');
  assert.equal(animationCall.options.headers['X-Animation-Test'], '1');
  assert.equal(animationCall.options.headers['X-OTA-Password'], undefined);
  const beforeDuplicate = calls.length;
  await elements['run-animation'].listeners.click();
  assert.equal(calls.length, beforeDuplicate);
  running.resolve({ ok: true, async text() { return 'Dig for treasure'; } });
  await pendingAnimation;
  assertAnimationTestControls(elements, false);
  assert.equal(elements['animation-test-status'].textContent, 'Starting: Dig for treasure.');
  for (const message of ['The display is busy. Please try the animation again.', '']) {
    queue.push({ ok: false, async text() { return message; } });
    await elements['run-animation'].listeners.click();
    assert.equal(elements['animation-test-status'].textContent,
      message || 'The display could not start the animation. Try again.');
    assertAnimationTestControls(elements, false);
  }
  queue.push(Promise.reject(new Error('connection lost')));
  await elements['run-animation'].listeners.click();
  assert.match(elements['animation-test-status'].textContent, /Could not confirm.*try again/);
  assertAnimationTestControls(elements, false);
  queue.push({ ok: true, async text() { return ' '; } });
  await elements['run-animation'].listeners.click();
  assert.match(elements['animation-test-status'].textContent, /Could not confirm/);
  assertAnimationTestControls(elements, false);

  // Scene count comes from the loaded catalog, including its status boundary.
  for (const count of [2, ANIMATION_COUNT + 1]) {
    const resizedCatalog = page(false, response(true), animationResponse(count));
    await settled();
    assertAnimationTestControls(resizedCatalog.elements, false);
    assert.equal(resizedCatalog.elements['animation-choice'].children.length, count);
    resizedCatalog.queue.push(statusResponse(count));
    await resizedCatalog.poll();
    assert.equal(resizedCatalog.elements['animation-test-status'].textContent, 'Playing: Animation ' + count + '.');
    resizedCatalog.queue.push(statusResponse(count + 1));
    await resizedCatalog.poll();
    assert.match(resizedCatalog.elements['animation-test-status'].textContent, /Could not check.*Retrying/);
  }

  for (const badCatalog of [{ ok: false }, { ok: true, async json() { return []; } },
    { ok: true, async json() { return [null]; } },
    { ok: true, async json() { return Array.from({ length: ANIMATION_COUNT }, () => ({ id: 1, name: 'Duplicate' })); } }]) {
    const failedCatalog = page(false, response(false), badCatalog);
    await settled();
    assertAnimationTestControls(failedCatalog.elements, true);
    assert.match(failedCatalog.elements['animation-test-status'].textContent, /Reload/);
    const beforeFailedCatalog = failedCatalog.calls.length;
    await failedCatalog.elements['run-animation'].listeners.click();
    assert.equal(failedCatalog.calls.length, beforeFailedCatalog);
    assertPriceTestControls(failedCatalog.elements, false);
  }

  // Price tests work without an upgrade password and serialize both directions.
  const increasing = deferred();
  queue.push(increasing.promise);
  const pricePending = elements['test-price-increase'].listeners.click();
  assertPriceTestControls(elements, true);
  assert.match(elements['price-test-status'].textContent, /Generating/);
  const priceCall = calls.at(-1);
  assert.equal(priceCall.url, '/test-price');
  assert.equal(priceCall.options.method, 'POST');
  assert.equal(priceCall.options.body, '+');
  assert.equal(priceCall.options.headers['Content-Type'], 'text/plain');
  assert.equal(priceCall.options.headers['X-Price-Test'], '1');
  assert.equal(priceCall.options.headers['X-OTA-Password'], undefined);
  assert.equal(priceCall.options.headers['X-Display-Settings'], undefined);
  const priceCalls = calls.length;
  await elements['test-price-increase'].listeners.click();
  await elements['test-price-decrease'].listeners.click();
  assert.equal(calls.length, priceCalls, 'Pending price tests must prevent duplicate requests');
  increasing.resolve({ ok: true, async text() { return '$4,123.45'; } });
  await pricePending;
  assertPriceTestControls(elements, false);
  assertFirmwareControls(elements, true);
  assert.match(elements['price-test-status'].textContent, /\$4,123\.45.*60 seconds/);

  queue.push({ ok: true, async text() { return '$3,876.54'; } });
  await elements['test-price-decrease'].listeners.click();
  assert.equal(calls.at(-1).options.body, '-');
  assert.match(elements['price-test-status'].textContent, /\$3,876\.54/);
  assertPriceTestControls(elements, false);

  for (const message of ['The display is busy. Please try again.', 'The display could not generate a test price.']) {
    queue.push({ ok: false, async text() { return message; } });
    await elements['test-price-increase'].listeners.click();
    assert.equal(elements['price-test-status'].textContent, message);
    assertPriceTestControls(elements, false);
  }
  queue.push({ ok: false, async text() { return ''; } });
  await elements['test-price-decrease'].listeners.click();
  assert.match(elements['price-test-status'].textContent, /could not generate/);
  const lostPrice = deferred();
  queue.push(lostPrice.promise);
  const interruptedPrice = elements['test-price-increase'].listeners.click();
  lostPrice.reject(new Error('connection lost'));
  await interruptedPrice;
  assert.match(elements['price-test-status'].textContent, /Could not confirm.*try again/);
  assertPriceTestControls(elements, false);
  queue.push({ ok: true, async text() { return '   '; } });
  await elements['test-price-decrease'].listeners.click();
  assert.match(elements['price-test-status'].textContent, /Could not confirm/);
  assertPriceTestControls(elements, false);

  const saving = deferred();
  queue.push(saving.promise);
  elements.animation.checked = true;
  const pending = elements.animation.listeners.change();
  assert(elements.animation.disabled);
  assert.equal(elements['settings-status'].textContent, 'Saving…');
  const post = calls.at(-1);
  assert.equal(post.url, '/settings');
  assert.equal(post.options.method, 'POST');
  assert.equal(post.options.body, '1');
  assert.equal(post.options.headers['Content-Type'], 'text/plain');
  assert.equal(post.options.headers['X-Display-Settings'], '1');
  assert.equal(post.options.headers['X-OTA-Password'], undefined);
  saving.resolve(response(true));
  await pending;
  assert.equal(elements.animation.checked, true);
  assert.equal(elements.animation.disabled, false);
  assert.equal(elements['settings-status'].textContent, 'Saved.');

  queue.push(response(false));
  elements.animation.checked = false;
  await elements.animation.listeners.change();
  assert.equal(calls.at(-1).options.body, '0');
  assert.equal(elements.animation.checked, false);
  assert.equal(elements['settings-status'].textContent, 'Saved.');

  queue.push({ ok: false, async text() { return 'The device could not save this setting.'; } });
  elements.animation.checked = true;
  await elements.animation.listeners.change();
  assert.equal(elements.animation.checked, false, 'A failed commit must restore the last saved setting');
  assert.equal(elements.animation.disabled, false);
  assert.equal(elements['settings-status'].textContent, 'The device could not save this setting.');

  const disconnected = deferred();
  queue.push(disconnected.promise);
  elements.animation.checked = true;
  const lostSave = elements.animation.listeners.change();
  disconnected.reject(new Error('connection lost'));
  await lostSave;
  assert.equal(elements.animation.checked, false);
  assert.equal(elements.animation.disabled, false);
  assert.match(elements['settings-status'].textContent, /Could not confirm.*Reload/);

  queue.push(response('invalid'));
  elements.animation.checked = true;
  await elements.animation.listeners.change();
  assert.equal(elements.animation.checked, false);
  assert.match(elements['settings-status'].textContent, /Could not confirm/);

  const before = calls.length;
  await elements.upload.listeners.submit({ preventDefault() {} });
  await elements['change-password'].listeners.submit({ preventDefault() {} });
  assert.equal(calls.length, before, 'Disabled firmware upload must stay disabled');
  assert.equal(elements.animation.disabled, false, 'Settings must remain usable without an OTA password');

  const failedLoad = page(false, { ok: false });
  await settled();
  assert(failedLoad.elements.animation.disabled);
  assert.match(failedLoad.elements['settings-status'].textContent, /Reload/);
  assertPriceTestControls(failedLoad.elements, false);
  failedLoad.queue.push({ ok: true, async text() { return '$4,123.45'; } });
  await failedLoad.elements['test-price-increase'].listeners.click();
  assert.match(failedLoad.elements['price-test-status'].textContent, /\$4,123\.45/);
  assert(failedLoad.elements.animation.disabled, 'Price tests must not alter failed settings loading');
  const invalidLoad = page(true, response('invalid'));
  await settled();
  assert(invalidLoad.elements.animation.disabled);
  assert.match(invalidLoad.elements['settings-status'].textContent, /Reload/);

  const mismatch = page(true, response(true));
  await settled();
  enterPassword(mismatch.elements, 'new-test-password', 'different-password');
  const mismatchCalls = mismatch.calls.length;
  await mismatch.elements['change-password'].listeners.submit({ preventDefault() {} });
  assert.equal(mismatch.calls.length, mismatchCalls, 'Mismatched confirmation must not make a request');
  assert(mismatch.elements['password-status'].textContent);
  assert.equal(mismatch.elements['new-password'].value, 'new-test-password');
  assertFirmwareControls(mismatch.elements, false);

  for (const invalid of ['', 'x'.repeat(129), 'new\npassword', 'new\0password',
    'new\x7fpassword', 'new£password', 'new😀password', ' ', ' newpassword', 'newpassword ']) {
    const invalidPassword = page(true, response(false));
    await settled();
    enterPassword(invalidPassword.elements, invalid);
    const count = invalidPassword.calls.length;
    await invalidPassword.elements['change-password'].listeners.submit({ preventDefault() {} });
    assert.equal(invalidPassword.calls.length, count, 'Invalid password must be rejected before a request');
    assert(invalidPassword.elements['password-status'].textContent);
    assertFirmwareControls(invalidPassword.elements, false);
  }

  const passwordChange = page(true, response(true));
  await settled();
  enterPassword(passwordChange.elements, 'new-test-password');
  passwordChange.elements.password.value = 'old-upload-password';
  passwordChange.elements['lighting-gpios-password'].value = 'old-gpio-password';
  passwordChange.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  const changing = deferred();
  passwordChange.queue.push(changing.promise);
  const changePending = passwordChange.elements['change-password'].listeners.submit({ preventDefault() {} });
  assertFirmwareControls(passwordChange.elements, true);
  assertPriceTestControls(passwordChange.elements, true);
  assertAnimationTestControls(passwordChange.elements, true);
  assert.equal(passwordChange.elements.animation.disabled, true);
  assert.equal(passwordChange.elements['stick-figure'].disabled, true);
  const changeCall = passwordChange.calls.at(-1);
  assert.equal(changeCall.url, '/password');
  assert.equal(changeCall.options.method, 'POST');
  assert.equal(changeCall.options.body, 'new-test-password');
  assert.equal(changeCall.options.headers['Content-Type'], 'text/plain');
  assert.equal(changeCall.options.headers['X-OTA-Password'], 'current-test-password');
  assert.equal(changeCall.options.headers['X-Display-Settings'], undefined);
  const pendingCalls = passwordChange.calls.length;
  await passwordChange.elements.upload.listeners.submit({ preventDefault() {} });
  await passwordChange.elements['change-password'].listeners.submit({ preventDefault() {} });
  await passwordChange.elements['test-price-increase'].listeners.click();
  await passwordChange.elements['test-price-decrease'].listeners.click();
  await passwordChange.elements['run-animation'].listeners.click();
  assert.equal(passwordChange.calls.length, pendingCalls, 'Password changes must serialize both forms');
  changing.resolve({ ok: true, async text() { return 'Password changed.'; } });
  await changePending;
  assert.equal(passwordChange.elements['password-status'].textContent, 'Password changed.');
  for (const id of ['current-password', 'new-password', 'confirm-password', 'password', 'price-url-password', 'lighting-gpios-password']) {
    assert.equal(passwordChange.elements[id].value, '', `${id} must be cleared after success`);
  }
  assertFirmwareControls(passwordChange.elements, false);
  assertPriceTestControls(passwordChange.elements, false);
  assertAnimationTestControls(passwordChange.elements, false);

  for (const valid of ['!', '~'.repeat(128), 'new password']) {
    const validPassword = page(true, response(true));
    await settled();
    enterPassword(validPassword.elements, valid);
    validPassword.queue.push({ ok: true, async text() { return 'Password changed.'; } });
    await validPassword.elements['change-password'].listeners.submit({ preventDefault() {} });
    assert.equal(validPassword.calls.at(-1).url, '/password');
    assert.equal(validPassword.calls.at(-1).options.body, valid);
    assertFirmwareControls(validPassword.elements, false);
  }

  const passwordFailure = page(true, response(true));
  await settled();
  enterPassword(passwordFailure.elements, 'new-test-password');
  passwordFailure.elements.password.value = 'old-upload-password';
  for (const message of ['Incorrect current password.', 'Could not save the new password.']) {
    passwordFailure.queue.push({ ok: false, async text() { return message; } });
    await passwordFailure.elements['change-password'].listeners.submit({ preventDefault() {} });
    assert.equal(passwordFailure.elements['password-status'].textContent, message);
    assert.equal(passwordFailure.elements['current-password'].value, 'current-test-password');
    assert.equal(passwordFailure.elements['new-password'].value, 'new-test-password');
    assert.equal(passwordFailure.elements['confirm-password'].value, 'new-test-password');
    assert.equal(passwordFailure.elements.password.value, 'old-upload-password');
    assertFirmwareControls(passwordFailure.elements, false);
  }
  const lostChange = deferred();
  passwordFailure.queue.push(lostChange.promise);
  const interruptedChange = passwordFailure.elements['change-password'].listeners.submit({ preventDefault() {} });
  assertFirmwareControls(passwordFailure.elements, true);
  lostChange.reject(new Error('network lost'));
  await interruptedChange;
  assert(passwordFailure.elements['password-status'].textContent);
  assert.equal(passwordFailure.elements['new-password'].value, 'new-test-password');
  assert.equal(passwordFailure.elements['current-password'].value, 'current-test-password');
  assert.equal(passwordFailure.elements.password.value, 'old-upload-password');
  assertFirmwareControls(passwordFailure.elements, false);

  const uploadFailure = page(true, response(true));
  await settled();
  enterPassword(uploadFailure.elements, 'new-test-password');
  uploadFailure.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  uploadFailure.elements.password.value = 'upload-test-password';
  uploadFailure.queue.push({ ok: false, async text() { return 'Firmware rejected.'; } });
  await uploadFailure.elements.upload.listeners.submit({ preventDefault() {} });
  assertFirmwareControls(uploadFailure.elements, false);
  assertPriceTestControls(uploadFailure.elements, false);
  assert.equal(uploadFailure.elements['new-password'].value, 'new-test-password');
  assert.equal(uploadFailure.elements.password.value, 'upload-test-password');
  const lostUpload = deferred();
  uploadFailure.queue.push(lostUpload.promise);
  const failedUpload = uploadFailure.elements.upload.listeners.submit({ preventDefault() {} });
  assertFirmwareControls(uploadFailure.elements, true);
  lostUpload.reject(new Error('network lost'));
  await failedUpload;
  assertFirmwareControls(uploadFailure.elements, false);
  assertPriceTestControls(uploadFailure.elements, false);
  assert.equal(uploadFailure.elements['new-password'].value, 'new-test-password');
  assert.equal(uploadFailure.elements.password.value, 'upload-test-password');

  const firmware = page(true, response(true));
  await settled();
  assert.equal(firmware.elements.animation.checked, true);
  assert.equal(firmware.elements.submit.disabled, false);
  firmware.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  firmware.elements.password.value = 'test-password';
  enterPassword(firmware.elements, 'new-test-password');
  const uploading = deferred();
  firmware.queue.push(uploading.promise);
  const uploadPending = firmware.elements.upload.listeners.submit({ preventDefault() {} });
  assertFirmwareControls(firmware.elements, true);
  assertPriceTestControls(firmware.elements, true);
  assertAnimationTestControls(firmware.elements, true);
  const uploadingCalls = firmware.calls.length;
  await firmware.elements['change-password'].listeners.submit({ preventDefault() {} });
  await firmware.elements.upload.listeners.submit({ preventDefault() {} });
  await firmware.elements['test-price-increase'].listeners.click();
  await firmware.elements['test-price-decrease'].listeners.click();
  await firmware.elements['run-animation'].listeners.click();
  assert.equal(firmware.calls.length, uploadingCalls, 'Firmware uploads must serialize both forms');
  uploading.resolve({ ok: true, async text() { return 'Firmware verified. Restarting.'; } });
  await uploadPending;
  const upload = firmware.calls.at(-1);
  assert.equal(upload.url, '/update');
  assert.equal(upload.options.headers['X-OTA-Password'], 'test-password');
  assert.equal(upload.options.headers['Content-Type'], 'application/octet-stream');
  assert.equal(upload.options.body, firmware.elements.firmware.files[0]);
  assert(firmware.elements.submit.disabled);
  assert.equal(firmware.elements.submit.textContent, 'Upgrade complete');
  assert.equal(firmware.elements.password.value, '');
  assert.equal(firmware.elements.status.textContent, 'Firmware verified. Restarting.');
  assert.equal(firmware.elements.animation.disabled, true);
  assert.equal(firmware.elements['stick-figure'].disabled, true);
  assertFirmwareControls(firmware.elements, true);
  assertPriceTestControls(firmware.elements, true);
  assertAnimationTestControls(firmware.elements, true);

  // A completed price request must not re-enable its controls during an upload.
  const overlapping = page(true, response(true));
  await settled();
  overlapping.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  overlapping.elements.password.value = 'test-password';
  const priceWhileUploading = deferred(), concurrentUpload = deferred();
  overlapping.queue.push(priceWhileUploading.promise);
  const pendingPrice = overlapping.elements['test-price-increase'].listeners.click();
  overlapping.queue.push(concurrentUpload.promise);
  const pendingUpload = overlapping.elements.upload.listeners.submit({ preventDefault() {} });
  assertPriceTestControls(overlapping.elements, true);
  priceWhileUploading.resolve({ ok: true, async text() { return '$4,123.45'; } });
  await pendingPrice;
  assertPriceTestControls(overlapping.elements, true);
  concurrentUpload.resolve({ ok: false, async text() { return 'Firmware rejected.'; } });
  await pendingUpload;
  assertPriceTestControls(overlapping.elements, false);
  assertFirmwareControls(overlapping.elements, false);

  // Animation completion cannot re-enable controls while firmware is busy.
  const animationOverlap = page(true, response(true));
  await settled();
  animationOverlap.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  animationOverlap.elements.password.value = 'test-password';
  const queuedAnimation = deferred(), animationUpload = deferred();
  animationOverlap.queue.push(queuedAnimation.promise);
  const overlappingAnimation = animationOverlap.elements['run-animation'].listeners.click();
  animationOverlap.queue.push(animationUpload.promise);
  const overlappingUpload = animationOverlap.elements.upload.listeners.submit({ preventDefault() {} });
  assertAnimationTestControls(animationOverlap.elements, true);
  queuedAnimation.resolve({ ok: true, async text() { return 'Clean the screen'; } });
  await overlappingAnimation;
  assertAnimationTestControls(animationOverlap.elements, true);
  animationUpload.resolve({ ok: false, async text() { return 'Firmware rejected.'; } });
  await overlappingUpload;
  assertAnimationTestControls(animationOverlap.elements, false);

  // A catalog arriving during upload follows the existing firmware busy state.
  const slowCatalog = deferred();
  const delayedCatalog = page(true, response(true), slowCatalog.promise);
  await settled();
  delayedCatalog.elements.firmware.files = [{ name: 'native-display.bin', size: 2048 }];
  const delayedUpload = deferred();
  delayedCatalog.queue.push(delayedUpload.promise);
  const uploadBeforeCatalog = delayedCatalog.elements.upload.listeners.submit({ preventDefault() {} });
  slowCatalog.resolve(animationResponse());
  await settled();
  assertAnimationTestControls(delayedCatalog.elements, true);
  assert.equal(delayedCatalog.elements['animation-choice'].children.length, ANIMATION_COUNT);
  delayedUpload.resolve({ ok: false, async text() { return 'Firmware rejected.'; } });
  await uploadBeforeCatalog;
  assertAnimationTestControls(delayedCatalog.elements, false);

  await testPriceUrl();
  await testShowPassword();
  console.log('Browser script checks passed: settings/password/OTA, authenticated URL saving, screen-only password requests, price tests, 33 animation options, catalog/status errors, authenticated GPIO configuration, password clearing, and firmware busy races.');
}

test().catch(error => { console.error(error); process.exitCode = 1; });
