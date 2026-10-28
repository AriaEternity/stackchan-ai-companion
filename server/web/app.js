(() => {
  const loginView = document.querySelector('#login-view');
  const chatView = document.querySelector('#chat-view');
  const loginForm = document.querySelector('#login-form');
  const loginError = document.querySelector('#login-error');
  const chatForm = document.querySelector('#chat-form');
  const input = document.querySelector('#message');
  const sendButton = chatForm.querySelector('button[type="submit"]');
  const conversation = document.querySelector('#conversation');
  const memoryDrawer = document.querySelector('#memory-drawer');
  const memoryList = document.querySelector('#memory-list');
  const deviceStatus = document.querySelector('#device-status');
  const deviceLabel = document.querySelector('#device-label');
  const commandPanel = document.querySelector('#command-panel');
  const commandSummary = document.querySelector('#command-summary');
  const commandHistory = document.querySelector('#command-history');
  const motionLock = document.querySelector('#motion-lock');
  const actionFeedback = document.querySelector('#action-feedback');
  const actionButtons = [...document.querySelectorAll('[data-gesture]')];

  const MESSAGE_POLL_MS = 2000;
  const DEVICE_POLL_MS = 2000;
  const MAX_RENDERED_MESSAGES = 200;
  const retryDelays = [1000, 2000, 5000, 10000, 15000];
  const messageNodes = new Map();
  let messageCursor = 0;
  let messageTimer = null;
  let deviceTimer = null;
  let messageFailures = 0;
  let deviceFailures = 0;
  let chatActive = false;
  let chatSending = false;
  let actionSending = false;
  let deviceOnline = false;
  let lastActionId = '';
  let temporaryMessageId = 0;
  let lastCommandAlertKey = '';

  class RequestError extends Error {
    constructor(message, status) {
      super(message);
      this.status = status;
    }
  }

  async function request(url, options = {}) {
    const response = await fetch(url, { credentials: 'same-origin', headers: { 'Content-Type': 'application/json', ...(options.headers || {}) }, ...options });
    if (!response.ok) throw new RequestError((await response.json().catch(() => ({}))).detail || '请求失败', response.status);
    return response.status === 204 ? null : response.json();
  }

  function nearConversationBottom() {
    return conversation.scrollHeight - conversation.scrollTop - conversation.clientHeight < 80;
  }

  function appendMessage(kind, content, className = '') {
    const node = document.createElement('div');
    node.className = `message ${kind}${className ? ` ${className}` : ''}`;
    node.textContent = content;
    conversation.append(node);
    conversation.scrollTop = conversation.scrollHeight;
    return node;
  }

  function insertPersistedMessage(item) {
    if (!Number.isInteger(item.id) || item.id < 1 || !['user', 'assistant'].includes(item.role) || typeof item.content !== 'string') return;
    if (messageNodes.has(item.id)) return;
    const shouldScroll = nearConversationBottom();
    const node = document.createElement('div');
    node.className = `message ${item.role}`;
    node.textContent = item.content;
    node.dataset.messageId = String(item.id);

    const laterId = [...messageNodes.keys()].filter((id) => id > item.id).sort((left, right) => left - right)[0];
    if (laterId !== undefined) conversation.insertBefore(node, messageNodes.get(laterId));
    else conversation.append(node);
    messageNodes.set(item.id, node);

    while (messageNodes.size > MAX_RENDERED_MESSAGES) {
      const oldestId = Math.min(...messageNodes.keys());
      messageNodes.get(oldestId).remove();
      messageNodes.delete(oldestId);
    }
    if (shouldScroll) conversation.scrollTop = conversation.scrollHeight;
  }

  function stopLiveUpdates() {
    chatActive = false;
    if (messageTimer !== null) clearTimeout(messageTimer);
    if (deviceTimer !== null) clearTimeout(deviceTimer);
    messageTimer = null;
    deviceTimer = null;
  }

  function returnToLogin(message) {
    stopLiveUpdates();
    chatView.classList.add('hidden');
    loginView.classList.remove('hidden');
    loginError.textContent = message;
  }

  function handleRequestError(error) {
    if (error instanceof RequestError && error.status === 401) {
      returnToLogin('登录已过期，请重新进入');
      return true;
    }
    return false;
  }

  function setActionButtonsEnabled(enabled) {
    for (const button of actionButtons) button.disabled = !enabled || actionSending;
  }

  async function syncMessages(initial = false) {
    if (!chatActive) return;
    try {
      const query = initial || messageCursor === 0
        ? '/api/messages?conversation_id=default&limit=100'
        : `/api/messages?conversation_id=default&after_id=${messageCursor}&limit=100`;
      const data = await request(query);
      for (const item of data.items) insertPersistedMessage(item);
      if (Number.isInteger(data.next_after_id) && data.next_after_id >= messageCursor) messageCursor = data.next_after_id;
      messageFailures = 0;
      messageTimer = setTimeout(() => syncMessages(false), data.has_more ? 0 : MESSAGE_POLL_MS);
    } catch (error) {
      if (handleRequestError(error)) return;
      const delay = retryDelays[Math.min(messageFailures, retryDelays.length - 1)];
      messageFailures += 1;
      messageTimer = setTimeout(() => syncMessages(false), delay);
    }
  }

  async function refreshDevice() {
    if (!chatActive) return;
    let nextDelay = DEVICE_POLL_MS;
    try {
      const device = await request('/api/device');
      deviceOnline = device.online === true;
      deviceStatus.classList.toggle('online', device.online);
      deviceLabel.textContent = device.online ? `已连接${device.battery === null ? '' : ` ${Math.round(device.battery)}%`}` : '未连接';
      setActionButtonsEnabled(deviceOnline);
      renderCommandState(device);
      deviceFailures = 0;
    } catch (error) {
      deviceOnline = false;
      setActionButtonsEnabled(false);
      if (handleRequestError(error)) return;
      nextDelay = retryDelays[Math.min(deviceFailures, retryDelays.length - 1)];
      deviceFailures += 1;
    }
    if (chatActive) deviceTimer = setTimeout(refreshDevice, nextDelay);
  }

  const commandNames = {
    show_text: '屏幕文字',
    set_expression: '表情',
    play_gesture: '头部动作',
    set_idle_mode: '待机模式',
  };
  const gestureNames = {
    nod: '点头', shake: '摇头', look_left: '左看', look_right: '右看',
    look_up: '上看', look_down: '下看', nuzzle: '蹭蹭',
  };
  const sourceNames = { web_action: '网页按钮', chat: '聊天', touch: '摸头' };
  const statusNames = {
    pending: '等待发送', sending: '发送中', sent: '已发送', accepted: '设备已接收', completed: '已完成',
    rejected: '未执行', expired: '已过期', busy: '设备忙', timeout: '回执超时',
    disconnected: '连接中断', not_sent: '未发送', send_failed: '发送失败',
  };
  const reasonNames = {
    unsafe: '设备安全策略阻止动作', server_motion_locked: '服务器动作锁阻止请求',
    invalid: '命令无效', unsupported: '设备不支持',
    internal: '设备内部拒绝', ack_timeout: '未在有效期内收到回执',
    device_disconnected: '设备连接已断开', device_reconnected: '设备已重新连接',
    offline: '设备当前离线', protocol_mismatch: '设备协议不匹配',
    send_error: '发送到设备失败',
  };

  function commandTime(item) {
    const value = item.updated_at || item.sent_at;
    if (!value) return '';
    const parsed = new Date(value);
    return Number.isNaN(parsed.getTime()) ? '' : parsed.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  }

  function commandLabel(item) {
    if (item.name !== 'play_gesture') return commandNames[item.name] || item.name || '设备命令';
    const source = sourceNames[item.source] || '';
    const gesture = gestureNames[item.gesture] || item.gesture || commandNames.play_gesture;
    return [source, gesture].filter(Boolean).join(' · ');
  }

  function renderCommandState(device) {
    motionLock.classList.toggle('hidden', !device.server_policy?.motion_locked);
    const history = Array.isArray(device.command_history) ? device.command_history : [];
    commandHistory.replaceChildren();
    if (history.length === 0) {
      const empty = document.createElement('li');
      empty.className = 'command-empty'; empty.textContent = '暂无设备命令'; commandHistory.append(empty);
      commandSummary.textContent = device.online ? '设备在线' : '设备离线';
      commandSummary.className = '';
      return;
    }
    for (const item of history) {
      const row = document.createElement('li'); row.className = 'command-item';
      const name = document.createElement('span'); name.className = 'command-name'; name.textContent = commandLabel(item);
      const result = document.createElement('span'); result.className = 'command-result';
      const status = statusNames[item.status] || item.status || '状态未知';
      const reason = item.reason ? reasonNames[item.reason] || item.reason : '';
      result.textContent = reason ? `${status}：${reason}` : status;
      const meta = document.createElement('span'); meta.className = 'command-meta';
      const shortId = typeof item.id === 'string' ? item.id.slice(0, 8) : '';
      const intensity = Number.isFinite(item.intensity) ? `幅度 ${Math.round(item.intensity * 100)}%` : '';
      meta.textContent = [commandTime(item), intensity, shortId].filter(Boolean).join(' | ');
      if (typeof item.id === 'string') {
        meta.title = [
          `命令 ID：${item.id}`,
          item.issued_at ? `生成：${item.issued_at}` : '',
          item.sent_at ? `发送：${item.sent_at}` : '',
          item.updated_at ? `更新：${item.updated_at}` : '',
        ].filter(Boolean).join('\n');
      }
      row.append(name, result, meta); commandHistory.append(row);
    }
    const latest = history[0];
    const latestStatus = statusNames[latest.status] || latest.status || '状态未知';
    const latestReason = latest.reason ? reasonNames[latest.reason] || latest.reason : '';
    commandSummary.textContent = latestReason ? `${latestStatus}：${latestReason}` : latestStatus;
    commandSummary.className = ['rejected', 'expired', 'busy', 'timeout', 'disconnected', 'not_sent', 'send_failed'].includes(latest.status)
      ? 'command-summary-warn' : latest.status === 'completed' ? 'command-summary-ok' : '';
    const warning = history.find((item) => item.status === 'rejected'
      || item.reason === 'unsafe' || item.reason === 'server_motion_locked');
    const alertKey = warning ? `${warning.id || ''}:${warning.status || ''}:${warning.reason || ''}` : '';
    if (warning && alertKey !== lastCommandAlertKey) {
      const keepBottom = nearConversationBottom();
      commandPanel.open = true;
      if (keepBottom) requestAnimationFrame(() => { conversation.scrollTop = conversation.scrollHeight; });
    }
    lastCommandAlertKey = alertKey;
    if (lastActionId) {
      const action = history.find((item) => item.id === lastActionId);
      if (action) {
        const actionName = commandLabel(action);
        const actionStatus = statusNames[action.status] || action.status || '状态未知';
        const actionReason = action.reason ? reasonNames[action.reason] || action.reason : '';
        actionFeedback.textContent = `${actionName}：${actionReason ? `${actionStatus}：${actionReason}` : actionStatus}`;
        actionFeedback.classList.toggle('warn', Boolean(actionReason) || action.status !== 'completed');
      }
    }
  }

  async function refreshMemories() {
    const data = await request('/api/memories');
    memoryList.replaceChildren();
    for (const item of data.items) {
      const node = document.createElement('article');
      node.className = 'memory-item';
      const text = document.createElement('p'); text.textContent = item.content;
      const footer = document.createElement('footer');
      const meta = document.createElement('span'); meta.textContent = `${item.category} | ${item.status}`;
      const remove = document.createElement('button'); remove.type = 'button'; remove.textContent = '删除';
      remove.addEventListener('click', async () => { await request(`/api/memories/${item.id}`, { method: 'DELETE' }); await refreshMemories(); });
      footer.append(meta, remove); node.append(text, footer); memoryList.append(node);
    }
  }

  async function enterChat() {
    stopLiveUpdates();
    chatActive = true;
    messageCursor = 0;
    messageFailures = 0;
    deviceFailures = 0;
    lastCommandAlertKey = '';
    messageNodes.clear();
    conversation.replaceChildren();
    loginView.classList.add('hidden'); chatView.classList.remove('hidden');
    await Promise.all([syncMessages(true), refreshDevice()]);
    input.focus();
  }

  loginForm.addEventListener('submit', async (event) => {
    event.preventDefault(); loginError.textContent = '';
    try { await request('/api/login', { method: 'POST', body: JSON.stringify({ password: document.querySelector('#password').value }) }); await enterChat(); }
    catch (error) { loginError.textContent = error.message; }
  });

  chatForm.addEventListener('submit', async (event) => {
    event.preventDefault(); const message = input.value.trim(); if (!message || chatSending) return;
    chatSending = true; input.disabled = true; sendButton.disabled = true;
    input.value = ''; input.focus();
    const pending = appendMessage('user', message, 'pending');
    pending.dataset.temporaryId = String(++temporaryMessageId);
    try {
      const reply = await request('/api/chat', { method: 'POST', body: JSON.stringify({ message }) });
      pending.remove();
      if (Array.isArray(reply.messages)) {
        for (const item of reply.messages) insertPersistedMessage(item);
      } else {
        appendMessage('assistant', reply.reply);
      }
    } catch (error) {
      if (handleRequestError(error)) return;
      pending.classList.remove('pending'); pending.classList.add('failed');
      appendMessage('note', error.message);
    } finally {
      chatSending = false; input.disabled = false; sendButton.disabled = false;
      if (chatActive) input.focus();
    }
  });

  actionButtons.forEach((button) => {
    button.addEventListener('click', async () => {
      if (actionSending || !deviceOnline) return;
      const gesture = button.dataset.gesture;
      if (!gesture) return;
      actionSending = true;
      setActionButtonsEnabled(false);
      actionFeedback.textContent = '正在发送动作请求…';
      actionFeedback.classList.remove('warn');
      try {
        const result = await request('/api/action', {
          method: 'POST',
          body: JSON.stringify({ gesture }),
        });
        lastActionId = result.command?.id || '';
        const actionName = `网页按钮 · ${gestureNames[result.gesture] || result.gesture || '头部动作'}`;
        const status = statusNames[result.command?.status] || result.command?.status || '状态未知';
        const reason = result.command?.reason ? reasonNames[result.command.reason] || result.command.reason : '';
        actionFeedback.textContent = `${actionName}：${reason ? `${status}：${reason}` : status}`;
        actionFeedback.classList.toggle('warn', Boolean(reason) || result.command?.status !== 'completed');
        await refreshDevice();
      } catch (error) {
        if (handleRequestError(error)) return;
        actionFeedback.textContent = error.message;
        actionFeedback.classList.add('warn');
      } finally {
        actionSending = false;
        setActionButtonsEnabled(deviceOnline);
      }
    });
  });

  document.querySelector('#memory-button').addEventListener('click', async () => { memoryDrawer.classList.remove('hidden'); await refreshMemories(); });
  document.querySelector('#close-memory').addEventListener('click', () => memoryDrawer.classList.add('hidden'));
  document.querySelector('#memory-form').addEventListener('submit', async (event) => {
    event.preventDefault(); const field = document.querySelector('#memory-content'); const content = field.value.trim(); if (!content) return;
    await request('/api/memories', { method: 'POST', body: JSON.stringify({ content }) }); field.value = ''; await refreshMemories();
  });
})();
