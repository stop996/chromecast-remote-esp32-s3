#pragma once

#include <pgmspace.h>

const char setupPage[] PROGMEM = R"html(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light">
<title>Chromecast 遥控器设置</title>
<style>
  :root { font-family: system-ui, sans-serif; color: #172334; background: #f2f5f8; }
  * { box-sizing: border-box; }
  body { margin: 0; }
  main { width: min(100% - 32px, 750px); margin: 28px auto 36px; }
  h1 { font-size: clamp(24px, 5vw, 34px); margin: 0 0 18px; }
  h2 { font-size: 19px; margin: 0; }
  p { margin: 8px 0 12px; color: #47576a; line-height: 1.55; }
  section { background: #fff; border: 1px solid #d7e1e9; border-radius: 12px; padding: 18px; margin-bottom: 14px; }
  .heading, .controls, .save-bar { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
  .heading { justify-content: space-between; }
  #connection { color: #224c78; background: #e9f2fc; padding: 5px 9px; border-radius: 6px; font-size: 14px; }
  #last-key { min-height: 1.55em; margin-bottom: 0; }
  select { flex: 1 1 220px; min-width: 0; }
  button, input, select { font: inherit; min-height: 42px; border: 1px solid #aabac8; border-radius: 7px; padding: 7px 11px; background: #fff; color: inherit; }
  button { font-weight: 600; cursor: pointer; }
  button:disabled { opacity: .48; cursor: default; }
  :focus-visible { outline: 2px solid #0757bd; outline-offset: 2px; }
  .keys { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
  .key { border: 1px solid #d7e1e9; border-radius: 8px; padding: 8px; display: grid; grid-template-columns: minmax(80px, .7fr) minmax(0, 1fr); align-items: center; gap: 6px; }
  .key input { min-width: 0; width: 100%; font-family: ui-monospace, monospace; font-size: 14px; }
  .keys.shortcuts { grid-template-columns: 1fr; }
  .shortcuts .key { grid-template-columns: minmax(5em, .65fr) repeat(2, minmax(0, 1fr)); }
  .key label { min-width: 0; }
  .key label span { display: block; color: #47576a; margin-bottom: 2px; }
  .chord-field { display: grid; grid-template-columns: minmax(0, 1fr) auto; align-items: end; gap: 5px; min-width: 0; }
  .record { padding-inline: 7px; font-size: 14px; }
  .record[aria-pressed="true"] { color: #0757bd; border-color: #0757bd; }
  .record-note { grid-column: 1 / -1; color: #224c78; font-size: 13px; }
  .mic-setting { margin: 12px 0; }
  .mic-setting select { margin-left: 6px; }
  .key.detected { background: #e9f2fc; border-color: #3a79bf; }
  .key span { font-size: 14px; }
  .save-bar { justify-content: space-between; margin-top: 16px; }
  #save { background: #0a4ea1; border-color: #0a4ea1; color: #fff; }
  #message { margin: 4px 0 0; min-height: 1.55em; }
  #message.error { color: #a32323; }
  @media (max-width: 590px) { main { margin-top: 18px; } .keys { grid-template-columns: 1fr; } }
  @media (max-width: 460px) {
    .shortcuts .key { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    .shortcuts .key > span { grid-column: 1 / -1; font-weight: 600; }
  }
</style>
</head>
<body>
<main>
  <h1>Chromecast 遥控器设置</h1>
  <section aria-labelledby="remote-title">
    <div class="heading"><h2 id="remote-title">连接遥控器</h2><span id="connection">读取中</span></div>
    <p>长按遥控器的返回和 Home，指示灯闪烁后点扫描。</p>
    <div class="controls">
      <button type="button" id="scan" disabled>扫描</button>
      <select id="devices" aria-label="扫描到的蓝牙设备" disabled><option value="">请选择遥控器</option></select>
      <button type="button" id="connect" disabled>连接</button>
      <button type="button" id="disconnect" disabled>断开</button>
    </div>
    <p id="last-key" aria-live="polite">连接后按键试一下。</p>
  </section>
  <section aria-labelledby="map-title">
    <div class="heading"><h2 id="map-title">按键映射</h2><button type="button" id="defaults" disabled>恢复默认串口名</button></div>
    <p>串口事件名留空会关闭该键的命名串口事件，原始报告仍会输出。</p>
    <p id="shortcut-help" hidden>点「录制」后在电脑键盘上按下组合键，再松开。也可以输入 <code>Ctrl+Shift+S</code>，留空可停用该键的 USB 输出。</p>
    <button type="button" id="restore-shortcuts" hidden disabled>恢复默认键盘映射</button>
    <form id="mapping" autocomplete="off">
      <div id="mic-settings" class="mic-setting" hidden>
        <label for="mic-gain">麦克风增益倍率</label><select id="mic-gain" name="micGainSteps" disabled></select>
      </div>
      <div class="keys" id="keys"></div>
      <div class="save-bar"><button type="submit" id="save" disabled>保存并关闭 Wi-Fi</button></div>
    </form>
    <p id="message" role="status"></p>
  </section>
</main>
<script>
  const $ = id => document.getElementById(id);
  const names = {
    POWER: '电源', PLAY_PAUSE: '播放 / 暂停', UP: '上', DOWN: '下', LEFT: '左', RIGHT: '右',
    OK: '确认', MUTE: '静音', ASSISTANT: '语音键', HOME: '主页', BACK: '返回', VOLUME_UP: '音量加',
    VOLUME_DOWN: '音量减', YOUTUBE: 'YouTube', NETFLIX: 'Netflix', BRIGHTNESS_UP: '亮度加', INPUT: '输入源'
  };
  const states = {
    starting: '蓝牙启动中', idle: '待连接', scanning: '扫描中', connecting: '连接中',
    discovering: '读取遥控器', subscribing: '启用按键接收', ready: '已连接', closing: '断开中'
  };
  const errors = {
    busy: '蓝牙正在处理，请稍后重试。', connected: '请先断开已连接的遥控器。',
    invalid_device: '请重新扫描并选择遥控器。', flash_write: '保存失败，设置网页仍然开启。',
    invalid_mapping: '事件名最多 24 个字符，只能使用英文字母、数字、下划线、连字符、点和加号。',
    invalid_shortcut: '键盘组合无效。请用 + 连接不同的键，最多填写 6 个普通键。',
    invalid_mic_gain: '请选择列表中的麦克风增益倍率。',
    forbidden: '网页会话已变更，请刷新页面。'
  };
  const rows = new Map();
  let token;
  let busy = false;
  let saved = false;
  let remote;
  let deviceList = '';
  let recentKey;
  let timer;
  let shortcutLimit = 0;
  let recording;
  const modifierCodes = {
    ControlLeft: 'Ctrl', ControlRight: 'RCtrl', ShiftLeft: 'Shift', ShiftRight: 'RShift',
    AltLeft: 'Alt', AltRight: 'RAlt', MetaLeft: 'Win', MetaRight: 'RWin'
  };
  const namedCodes = {
    Escape: 'Esc', Space: 'Space', ArrowLeft: 'Left', ArrowRight: 'Right', ArrowUp: 'Up', ArrowDown: 'Down',
    Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\',
    Semicolon: ';', Quote: "'", Backquote: '`', Comma: ',', Period: '.', Slash: '/',
    AudioVolumeUp: 'VolumeUp', AudioVolumeDown: 'VolumeDown', AudioVolumeMute: 'VolumeMute', ContextMenu: 'Menu'
  };

  function message(text, error = false) {
    $('message').textContent = text;
    $('message').classList.toggle('error', error);
  }

  async function request(path, fields) {
    const options = { cache: 'no-store' };
    if (fields) Object.assign(options, {
      method: 'POST', headers: { 'X-Setup-Token': token }, body: new URLSearchParams(fields)
    });
    const response = await fetch(path, options);
    const data = await response.json();
    if (!response.ok) {
      const key = $('keys').children[data.index];
      if (data.error === 'invalid_shortcut' && key) {
        const input = key.querySelector('[name^="h"]');
        input.focus();
        throw new Error(`${key.firstElementChild.textContent}：${errors.invalid_shortcut}`);
      }
      throw new Error(errors[data.error] || '操作失败，请重试。');
    }
    return data;
  }

  function controls() {
    const ready = !busy && !saved && remote;
    const idle = ready && remote.state === 'idle';
    $('scan').disabled = !idle;
    $('devices').disabled = !idle || !remote.devices.length;
    $('connect').disabled = !idle || !$('devices').value;
    $('disconnect').disabled = !ready || remote.state !== 'ready';
    $('defaults').disabled = !!(!token || busy || saved);
    $('save').disabled = !!(!token || busy || saved || recording);
    $('restore-shortcuts').disabled = !!(!token || busy || saved || recording);
  }

  function renderStatus(data) {
    remote = data.remote;
    const nextList = JSON.stringify(remote.devices);
    if (deviceList !== nextList) {
      deviceList = nextList;
      const select = $('devices');
      const selected = select.value;
      const options = remote.devices.map(device => {
        const name = device.name || '未命名设备';
        return new Option(`${name} · ${device.address}${device.hid ? ' · HID' : ''}`, device.id);
      });
      select.replaceChildren(new Option('请选择遥控器', ''), ...options);
      if (options.some(option => option.value === selected)) select.value = selected;
    }
    $('connection').textContent = states[remote.state] || remote.state;
    const key = data.lastKey;
    if (key) {
      if (recentKey) rows.get(recentKey)?.label.classList.remove('detected');
      recentKey = key;
      rows.get(key)?.label.classList.add('detected');
      const row = rows.get(key);
      const alias = row?.input.value || '已关闭';
      const shortcut = row?.shortcut?.value;
      const output = shortcut ? `${alias} / ${shortcut}` : alias;
      if (key === 'ASSISTANT' && data.lastAction === 'up') {
        $('last-key').textContent = `语音键已松开 → ${output}`;
      } else {
        $('last-key').textContent = data.lastAction === 'request'
          ? `语音键请求 → ${output}` : `最近按键 ${names[key] || key} → ${output}`;
      }
    } else if (!recentKey) {
      $('last-key').textContent = remote.state === 'ready' && remote.decoder === 'raw'
        ? '已连接。此遥控器的报告会显示为串口 RAW。'
        : '连接后按键试一下。';
    }
    controls();
  }

  async function poll() {
    if (!busy && !saved) {
      try {
        const data = await request('/api/status');
        if (!busy && !saved) renderStatus(data);
      } catch {
        if (!busy && !saved) $('connection').textContent = '连接中断';
      }
    }
    if (!saved) timer = setTimeout(poll, 850);
  }

  function field(input, caption, keyName) {
    const label = document.createElement('label');
    const title = document.createElement('span');
    title.textContent = caption;
    input.setAttribute('aria-label', `${keyName} ${caption}`);
    label.append(title, input);
    return label;
  }

  function keyboardToken(code) {
    if (/^Key[A-Z]$/.test(code)) return code.slice(3);
    if (/^Digit[0-9]$/.test(code)) return code.slice(5);
    if (/^(F([1-9]|1[0-9]|2[0-4])|Enter|Backspace|Tab|CapsLock|PrintScreen|ScrollLock|Pause|Insert|Home|PageUp|Delete|End|PageDown|NumLock)$/.test(code)) return code;
    return modifierCodes[code] || namedCodes[code];
  }

  function stopRecording(value) {
    if (!recording) return;
    const { input, button, note } = recording;
    if (value !== undefined) input.value = value;
    button.textContent = '录制';
    button.setAttribute('aria-pressed', 'false');
    note.hidden = true;
    recording = undefined;
    controls();
  }

  function recordField(input, keyName, hold) {
    const wrap = document.createElement('div');
    wrap.className = 'chord-field';
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'record';
    button.textContent = '录制';
    button.setAttribute('aria-label', `${keyName} 录制键盘组合`);
    button.setAttribute('aria-pressed', 'false');
    const note = document.createElement('small');
    note.className = 'record-note';
    note.setAttribute('aria-live', 'polite');
    note.hidden = true;
    button.addEventListener('click', () => {
      const cancel = recording?.button === button;
      stopRecording();
      if (cancel || busy || saved) return;
      recording = { input, button, note, held: new Map(), chord: '', count: 0, invalid: false };
      button.textContent = '取消';
      button.setAttribute('aria-pressed', 'true');
      note.hidden = false;
      note.textContent = '按下键盘上的组合键，然后松开。';
      controls();
    });
    wrap.append(field(input, hold ? 'USB 按住快捷键' : 'USB 键盘组合', keyName), button, note);
    return wrap;
  }

  window.addEventListener('keydown', event => {
    if (!recording) return;
    event.preventDefault();
    event.stopPropagation();
    if (event.repeat || event.isComposing) return;
    const { held, note } = recording;
    const token = keyboardToken(event.code);
    held.set(event.code, token);
    const modifiers = [];
    const ordinary = [];
    for (const [code, name] of held) {
      if (!name) continue;
      (modifierCodes[code] ? modifiers : ordinary).push(name);
    }
    const chord = [...modifiers, ...ordinary].join('+');
    const media = ordinary.some(name => name.startsWith('Volume'));
    if (!token || ordinary.length > 6 || (media && held.size > 1) || chord.length > shortcutLimit) {
      recording.invalid = true;
      note.textContent = '此组合无法录制。请松开后重试，或手动输入。';
    } else if (!recording.invalid && held.size >= recording.count) {
      recording.chord = chord;
      recording.count = held.size;
      note.textContent = chord;
    }
  }, true);

  window.addEventListener('keyup', event => {
    if (!recording) return;
    event.preventDefault();
    event.stopPropagation();
    recording.held.delete(event.code);
    if (recording.held.size) return;
    if (!recording.invalid && recording.chord) {
      stopRecording(recording.chord);
    } else {
      recording.invalid = false;
      recording.count = 0;
      recording.chord = '';
    }
  }, true);
  window.addEventListener('blur', () => stopRecording());

  function makeRow({ key, name, shortcut, defaultShortcut }, index) {
    const row = document.createElement(shortcutLimit ? 'div' : 'label');
    row.className = 'key';
    const title = document.createElement('span');
    title.textContent = names[key] || key;
    const input = document.createElement('input');
    input.name = `k${index}`;
    input.value = name;
    input.maxLength = 24;
    input.pattern = '[A-Za-z0-9_.+\\-]{0,24}';
    input.spellcheck = false;
    input.autocapitalize = 'none';
    row.append(title);
    let chord;
    if (shortcutLimit) {
      chord = document.createElement('input');
      chord.name = `h${index}`;
      chord.value = shortcut;
      chord.maxLength = shortcutLimit;
      chord.spellcheck = false;
      chord.autocapitalize = 'none';
      row.append(field(input, '串口事件名', title.textContent),
        recordField(chord, title.textContent, key === 'ASSISTANT'));
    } else {
      row.append(input);
    }
    $('keys').append(row);
    rows.set(key, { label: row, input, shortcut: chord, defaultShortcut });
  }

  async function remoteAction(action, id = '') {
    if (busy || saved) return;
    busy = true;
    controls();
    message('');
    try { renderStatus(await request('/api/remote', { action, id })); }
    catch (error) { message(error.message, true); }
    finally { busy = false; controls(); }
  }

  $('devices').addEventListener('change', controls);
  $('scan').addEventListener('click', () => remoteAction('scan'));
  $('connect').addEventListener('click', () => remoteAction('connect', $('devices').value));
  $('disconnect').addEventListener('click', () => remoteAction('disconnect'));
  $('defaults').addEventListener('click', () => {
    for (const [key, row] of rows) row.input.value = key;
  });
  $('restore-shortcuts').addEventListener('click', () => {
    for (const row of rows.values()) row.shortcut.value = row.defaultShortcut;
  });
  $('mapping').addEventListener('submit', async event => {
    event.preventDefault();
    if (busy || saved || !token || recording) return;
    busy = true;
    controls();
    message('正在保存。');
    try {
      await request('/api/save', new FormData(event.currentTarget));
      saved = true;
      clearTimeout(timer);
      $('connection').textContent = '配置已完成';
      for (const control of $('keys').querySelectorAll('input, button')) control.disabled = true;
      $('mic-gain').disabled = true;
      message('已保存。配置网页和 Wi-Fi 热点即将关闭。');
    } catch (error) { message(error.message, true); }
    finally { busy = false; controls(); }
  });

  request('/api/config').then(config => {
    token = config.token;
    shortcutLimit = config.shortcutLimit || 0;
    $('keys').classList.toggle('shortcuts', !!shortcutLimit);
    $('shortcut-help').hidden = !shortcutLimit;
    $('restore-shortcuts').hidden = !shortcutLimit;
    if (config.micGainOptions?.length) {
      const select = $('mic-gain');
      select.replaceChildren(...config.micGainOptions.map(steps =>
        new Option(`${steps / 2}×（${steps * 50}%）`, steps)));
      select.value = config.micGainSteps;
      select.disabled = false;
      $('mic-settings').hidden = false;
    }
    config.keys.forEach(makeRow);
    controls();
    poll();
  }).catch(() => message('无法读取配置，请确认仍连接 ESP32 的热点并刷新。', true));
</script>
</body>
</html>)html";
