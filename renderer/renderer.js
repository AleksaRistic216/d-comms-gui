'use strict';

// ── State ────────────────────────────────────────────────────────────────────

const state = {
    chats:          [],          // [{ name, loaded, chatId, userKey, secretId, isInitiator }]
    currentName:    null,
    currentChatId:  null,
    messages:       {},          // chatId → message[]
    chatState:      {},          // chatId → 0|1|2  (proto state)
    unreadCounts:   {},          // name  → number
    appReady:       false,
    syncing:        false,
    syncPort:       -1,
    peers:          [],          // [{ host, port, live, lastSeen }]
};

// ── DOM refs ─────────────────────────────────────────────────────────────────

const $ = id => document.getElementById(id);
const el = {
    chatList:       $('chatList'),
    emptyState:     $('emptyState'),
    emptyTitle:     $('emptyTitle'),
    emptySubtitle:  $('emptySubtitle'),
    globalSpinner:  $('globalSpinner'),
    chatView:       $('chatView'),
    chatTitle:      $('chatTitle'),
    chatLoading:    $('chatLoading'),
    messagesArea:   $('messagesArea'),
    turnBar:        $('turnBar'),
    turnBarText:    $('turnBarText'),
    messageInput:   $('messageInput'),
    btnSend:        $('btnSend'),
    syncIndicator:  $('syncIndicator'),
    peerBadge:      $('peerBadge'),
    peerTooltip:    $('peerTooltip'),
    // modals / buttons
    btnAddChat:       $('btnAddChat'),
    btnOpenDataDir:   $('btnOpenDataDir'),
    btnSyncNow:       $('btnSyncNow'),
    btnChatInfo:      $('btnChatInfo'),
    btnDeleteChat:    $('btnDeleteChat'),
    // add-chat modal
    modalAddChat:     $('modalAddChat'),
    createFields:     $('createFields'),
    inputChatName:    $('inputChatName'),
    joinFields:       $('joinFields'),
    inputManualName:  $('inputManualName'),
    inputInviteCode:  $('inputInviteCode'),
    addHint:          $('addHint'),
    btnAddSubmit:     $('btnAddSubmit'),
    stepChoose:       $('stepChoose'),
    stepLoading:      $('stepLoading'),
    stepCredentials:  $('stepCredentials'),
    credInviteCode:   $('credInviteCode'),
    btnCopyInviteCode: $('btnCopyInviteCode'),
    btnCredDone:      $('btnCredDone'),
    // info modal
    modalChatInfo:    $('modalChatInfo'),
    infoChatName:     $('infoChatName'),
    infoChatRole:     $('infoChatRole'),
    infoUserKey:      $('infoUserKey'),
    infoSecretId:     $('infoSecretId'),
    btnInfoCopyUserKey:  $('btnInfoCopyUserKey'),
    btnInfoCopySecretId: $('btnInfoCopySecretId'),
    infoSyncPort:     $('infoSyncPort'),
    // add-peer modal
    // delete confirm
    modalDeleteConfirm:  $('modalDeleteConfirm'),
    deleteConfirmName:   $('deleteConfirmName'),
    btnConfirmDelete:    $('btnConfirmDelete'),
};

// ── Utilities ─────────────────────────────────────────────────────────────────

function formatTime(ts) {
    return new Date(ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function formatDate(ts) {
    const d = new Date(ts);
    const today = new Date();
    if (d.toDateString() === today.toDateString()) return 'Today';
    const yesterday = new Date(today); yesterday.setDate(today.getDate() - 1);
    if (d.toDateString() === yesterday.toDateString()) return 'Yesterday';
    return d.toLocaleDateString([], { weekday: 'long', month: 'long', day: 'numeric' });
}

/** Consistent colour from a short string (for peer avatar) */
function stringColor(s) {
    let h = 0;
    for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0;
    const hue = h % 360;
    return `hsl(${hue},55%,45%)`;
}


function showModal(id)  { $(id).style.display = 'flex'; }
function closeModal(id) { $(id).style.display = 'none'; }

function setHint(text, isError = false) {
    el.addHint.textContent = text;
    el.addHint.className   = 'field-hint' + (isError ? ' error' : '');
}

// ── Render ────────────────────────────────────────────────────────────────────

function renderSidebar() {
    el.chatList.innerHTML = '';
    if (state.chats.length === 0 && state.appReady) {
        const p = document.createElement('p');
        p.style.cssText = 'color:var(--text-muted);font-size:12px;padding:8px 10px;';
        p.textContent = 'No chats yet.';
        el.chatList.appendChild(p);
        return;
    }
    for (const chat of state.chats) {
        const div = document.createElement('div');
        div.className = 'chat-item' + (chat.name === state.currentName ? ' active' : '');
        div.dataset.name = chat.name;

        const unread = state.unreadCounts[chat.name] || 0;
        div.innerHTML = `
            <span class="chat-item-hash">#</span>
            <span class="chat-item-name">${escHtml(chat.name)}</span>
            ${!chat.loaded ? '<span class="chat-item-lock">🔒</span>' : ''}
            ${unread > 0 && chat.name !== state.currentName
                ? `<span class="chat-item-badge">${unread}</span>` : ''}
        `;
        div.addEventListener('click', () => openChat(chat.name));
        el.chatList.appendChild(div);
    }
}

function escHtml(s) {
    return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

function renderMessages() {
    const chatId = state.currentChatId;
    if (chatId == null) return;

    const msgs = state.messages[chatId] || [];
    el.messagesArea.innerHTML = '';

    if (msgs.length === 0) {
        el.messagesArea.innerHTML = `
            <div class="messages-empty">
                <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.2">
                    <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
                </svg>
                <p>No messages yet.<br>Be the first to say something!</p>
            </div>`;
        return;
    }

    // Group messages by sender runs, track day separators
    let lastDate   = null;
    let lastSender = null;

    for (const msg of msgs) {
        const now       = Date.now();
        const msgDate   = formatDate(now);   // d-comms has no timestamps; use "Today"
        const isMe      = msg.isMe;
        const senderKey = isMe ? 'me' : (msg.entityId || 'peer');
        const isCont    = senderKey === lastSender;

        // Day separator
        if (msgDate !== lastDate) {
            const sep = document.createElement('div');
            sep.className   = 'day-separator';
            sep.textContent = msgDate;
            el.messagesArea.appendChild(sep);
            lastDate   = msgDate;
            lastSender = null;
        }

        if (!isCont || lastSender !== senderKey) {
            // New message group
            const group = document.createElement('div');
            group.className = 'message-group';

            const authorColor = isMe ? '#5865f2' : stringColor(msg.entityId || 'peer');

            group.innerHTML = `
                <div class="message-avatar" style="background:${authorColor}" title="${msg.entityId || ''}"></div>
                <div class="message-body">
                    <div class="message-meta">
                        <span class="message-ts">${formatTime(now)}</span>
                    </div>
                    <div class="message-text">${escHtml(msg.text)}</div>
                </div>`;
            el.messagesArea.appendChild(group);
        } else {
            // Continuation (same sender)
            const cont = document.createElement('div');
            cont.className = 'message-continuation';
            cont.innerHTML = `<div class="message-text">${escHtml(msg.text)}</div>`;
            el.messagesArea.appendChild(cont);
        }

        lastSender = senderKey;
    }

    // Scroll to bottom
    el.messagesArea.scrollTop = el.messagesArea.scrollHeight;
}

function updateTurnBar() {
    const chatId = state.currentChatId;
    if (chatId == null) { el.turnBar.style.display = 'none'; return; }

    const s = state.chatState[chatId];
    if (s === 2) {
        // need_list → waiting for peer
        el.turnBar.style.display = 'flex';
        el.turnBarText.textContent = 'Waiting for peer to respond…';
        el.btnSend.disabled = true;
    } else {
        el.turnBar.style.display = 'none';
        el.btnSend.disabled = false;
    }
}

function updateSyncIndicator() {
    el.syncIndicator.className = 'sync-indicator' + (state.syncing ? ' syncing' : ' connected');
}

function relativeTime(ts) {
    if (!ts) return 'never';
    const diff = Math.floor((Date.now() - ts) / 1000);
    if (diff < 5)    return 'just now';
    if (diff < 60)   return `${diff}s ago`;
    if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
    return `${Math.floor(diff / 3600)}h ago`;
}

function updatePeerBadge(count, peers) {
    el.peerBadge.textContent = count === 1 ? '1 peer' : `${count} peers`;
    el.peerBadge.className   = 'peer-badge' + (count > 0 ? ' has-peers' : '');
    if (peers) state.peers = peers;
}

function buildPeerTooltip() {
    const peers = state.peers.filter(p => p.live);
    if (peers.length === 0) {
        el.peerTooltip.innerHTML = '<div class="peer-tooltip-empty">No active peers</div>';
        return;
    }
    el.peerTooltip.innerHTML = peers.map(p => `
        <div class="peer-tooltip-row">
            <span class="peer-tooltip-dot live"></span>
            <span class="peer-tooltip-addr">${escHtml(p.host)}:${p.port}</span>
            <span class="peer-tooltip-time">${relativeTime(p.lastSeen)}</span>
        </div>`).join('');
}

el.peerBadge.addEventListener('mouseenter', () => {
    buildPeerTooltip();
    const rect = el.peerBadge.getBoundingClientRect();
    el.peerTooltip.style.top  = (rect.bottom + 6) + 'px';
    el.peerTooltip.style.left = Math.min(rect.left, window.innerWidth - 230) + 'px';
    el.peerTooltip.classList.add('visible');
});
el.peerBadge.addEventListener('mouseleave', () => {
    el.peerTooltip.classList.remove('visible');
});


function showChatView() {
    el.emptyState.style.display   = 'none';
    el.chatView.style.display     = 'flex';
    el.chatLoading.style.display  = 'none';
}

function showChatLoading() {
    el.emptyState.style.display   = 'none';
    el.chatView.style.display     = 'flex';
    el.chatLoading.style.display  = 'flex';
    el.messagesArea.innerHTML     = '';
    el.turnBar.style.display      = 'none';
}

function showEmptyState(title, subtitle, spinner = false) {
    el.emptyState.style.display  = 'flex';
    el.chatView.style.display    = 'none';
    el.emptyTitle.textContent    = title;
    el.emptySubtitle.textContent = subtitle;
    el.globalSpinner.style.display = spinner ? 'block' : 'none';
}

// ── openChat ──────────────────────────────────────────────────────────────────

async function openChat(name) {
    if (state.currentName === name && state.currentChatId != null) return;

    state.currentName   = name;
    state.currentChatId = null;
    state.unreadCounts[name] = 0;

    el.chatTitle.textContent = name;
    renderSidebar();
    showChatLoading();

    const chat = state.chats.find(c => c.name === name);
    if (!chat) return;

    // Already loaded?
    if (chat.loaded && chat.chatId != null) {
        state.currentChatId = chat.chatId;
        await loadMessagesAndShow(chat.chatId);
        return;
    }

    // Need to load from disk (5-second KDF)
    const result = await window.dcomms.openChat(name);
    if (!result.success) {
        showEmptyState('Failed to open chat', result.error || 'Unknown error');
        return;
    }

    // Update chat entry in state
    const idx = state.chats.findIndex(c => c.name === name);
    if (idx >= 0) {
        Object.assign(state.chats[idx], {
            loaded:      true,
            chatId:      result.chatId,
            userKey:     result.userKey,
            secretId:    result.secretId,
            isInitiator: result.isInitiator,
        });
    }

    state.currentChatId = result.chatId;
    await loadMessagesAndShow(result.chatId);
    renderSidebar();
}

async function loadMessagesAndShow(chatId) {
    const msgs = await window.dcomms.getMessages(chatId);
    state.messages[chatId] = msgs;

    const info = await window.dcomms.getChatInfo(chatId);
    if (info) state.chatState[chatId] = info.state;

    showChatView();
    renderMessages();
    updateTurnBar();
    el.messageInput.focus();
}

// ── Send message ─────────────────────────────────────────────────────────────

async function sendMessage() {
    const chatId = state.currentChatId;
    if (chatId == null) return;

    const text = el.messageInput.value.trim();
    if (!text) return;

    el.messageInput.value = '';
    autoResizeTextarea();

    const result = await window.dcomms.sendMessage(chatId, text);
    if (!result.success) {
        if (result.error === 'Not your turn') {
            el.turnBar.style.display = 'flex';
            el.turnBarText.textContent = 'Not your turn — sync to check for peer messages.';
        }
        // Put the text back
        el.messageInput.value = text;
        autoResizeTextarea();
        return;
    }

    // Refresh messages immediately
    const msgs = await window.dcomms.getMessages(chatId);
    state.messages[chatId] = msgs;
    const info = await window.dcomms.getChatInfo(chatId);
    if (info) state.chatState[chatId] = info.state;
    renderMessages();
    updateTurnBar();
}

// ── Add chat modal ────────────────────────────────────────────────────────────

function openAddChatModal() {
    el.stepChoose.style.display      = '';
    el.stepLoading.style.display     = 'none';
    el.stepCredentials.style.display = 'none';
    el.inputChatName.value   = '';
    el.inputManualName.value = '';
    el.inputInviteCode.value = '';
    el.createFields.style.display = '';
    el.joinFields.style.display   = 'none';
    el.btnAddSubmit.style.display = '';
    el.btnAddSubmit.textContent   = 'Create';
    document.querySelector('input[name="chatAction"][value="create"]').checked = true;
    setHint('');
    showModal('modalAddChat');
    el.inputChatName.focus();
}

document.querySelectorAll('input[name="chatAction"]').forEach(r => {
    r.addEventListener('change', () => {
        const isJoin = r.value === 'join';
        el.createFields.style.display = isJoin ? 'none' : '';
        el.joinFields.style.display   = isJoin ? '' : 'none';
        el.btnAddSubmit.textContent   = isJoin ? 'Join' : 'Create';
    });
});

el.btnAddSubmit.addEventListener('click', async () => {
    const action = document.querySelector('input[name="chatAction"]:checked').value;

    if (action === 'create') {
        const name = el.inputChatName.value.trim();
        if (!name) { setHint('Please enter a chat name.', true); return; }
        if (state.chats.some(c => c.name === name)) {
            setHint('A chat with that name already exists.', true); return;
        }

        el.stepChoose.style.display  = 'none';
        el.stepLoading.style.display = '';

        const result = await window.dcomms.createChat(name);
        el.stepLoading.style.display = 'none';

        if (!result.success) {
            el.stepChoose.style.display = '';
            setHint(result.error || 'Failed to create chat.', true);
            return;
        }

        el.credInviteCode.textContent    = result.userKey + result.secretId;
        el.stepCredentials.style.display = '';
        state.chats.push({ name, loaded: true, chatId: result.chatId,
                           userKey: result.userKey, secretId: result.secretId, isInitiator: true });
        renderSidebar();

    } else {
        const name = el.inputManualName.value.trim();
        const code = el.inputInviteCode.value.trim();
        if (!name) { setHint('Enter a chat name.', true); return; }
        if (code.length !== 64) { setHint('Invite code must be 64 characters.', true); return; }

        el.stepChoose.style.display  = 'none';
        el.stepLoading.style.display = '';

        const result = await window.dcomms.joinChat(name, code.slice(0, 32), code.slice(32));
        el.stepLoading.style.display = 'none';

        if (!result.success) {
            el.stepChoose.style.display = '';
            setHint(result.error || 'Failed to join.', true);
            return;
        }

        state.chats.push({ name, loaded: true, chatId: result.chatId,
                           userKey: result.userKey, secretId: result.secretId, isInitiator: false });
        renderSidebar();
        closeModal('modalAddChat');
        openChat(name);
    }
});

el.btnCredDone.addEventListener('click', () => {
    closeModal('modalAddChat');
    const newChat = state.chats[state.chats.length - 1];
    if (newChat) openChat(newChat.name);
});

el.btnCopyInviteCode.addEventListener('click', () => window.dcomms.copyToClipboard(el.credInviteCode.textContent));

// ── Chat info modal ───────────────────────────────────────────────────────────

el.btnChatInfo.addEventListener('click', async () => {
    const chatId = state.currentChatId;
    if (chatId == null) return;
    const info = await window.dcomms.getChatInfo(chatId);
    if (!info) return;
    const status = await window.dcomms.getStatus();

    el.infoChatName.textContent  = state.currentName;
    el.infoChatRole.textContent  = info.isInitiator ? 'Initiator (you started this chat)' : 'Responder (you joined this chat)';
    el.infoUserKey.textContent   = info.userKey;
    el.infoSecretId.textContent  = info.secretId;
    el.infoSyncPort.textContent  = status.syncPort > 0 ? `${status.syncPort}` : 'Not running';
    showModal('modalChatInfo');
});

el.btnInfoCopyUserKey.addEventListener('click',  () => window.dcomms.copyToClipboard(el.infoUserKey.textContent));
el.btnInfoCopySecretId.addEventListener('click', () => window.dcomms.copyToClipboard(el.infoSecretId.textContent));

// ── Delete chat ───────────────────────────────────────────────────────────────

el.btnDeleteChat.addEventListener('click', () => {
    if (!state.currentName) return;
    el.deleteConfirmName.textContent = state.currentName;
    showModal('modalDeleteConfirm');
});

el.btnConfirmDelete.addEventListener('click', async () => {
    const name   = state.currentName;
    const chatId = state.currentChatId;
    closeModal('modalDeleteConfirm');
    await window.dcomms.deleteChat(name, chatId);
    state.chats = state.chats.filter(c => c.name !== name);
    if (state.messages[chatId]) delete state.messages[chatId];
    if (state.chatState[chatId]) delete state.chatState[chatId];
    state.currentName   = null;
    state.currentChatId = null;
    renderSidebar();
    showEmptyState('Select a chat', 'Choose a chat from the sidebar or create a new one.');
});

// ── Sync now ──────────────────────────────────────────────────────────────────

el.btnSyncNow.addEventListener('click', async () => {
    state.syncing = true;
    updateSyncIndicator();
    const result = await window.dcomms.syncNow();
    state.syncing = false;
    updateSyncIndicator();
    if (result.added > 0 && state.currentChatId != null) {
        const msgs = await window.dcomms.getMessages(state.currentChatId);
        state.messages[state.currentChatId] = msgs;
        const info = await window.dcomms.getChatInfo(state.currentChatId);
        if (info) state.chatState[state.currentChatId] = info.state;
        renderMessages();
        updateTurnBar();
    }
});

// ── Add peer (from info modal header, via right-click / context) ─────────────
// Trigger via sidebar footer icon (right-click) or keyboard shortcut
el.btnOpenDataDir.addEventListener('click', () => window.dcomms.openDataDir());

// ── Modal close buttons ───────────────────────────────────────────────────────

document.querySelectorAll('[data-close]').forEach(btn => {
    btn.addEventListener('click', () => closeModal(btn.dataset.close));
});
document.querySelectorAll('.modal-backdrop').forEach(backdrop => {
    backdrop.addEventListener('click', e => {
        if (e.target === backdrop) closeModal(backdrop.id);
    });
});

// ── Input handling ────────────────────────────────────────────────────────────

el.btnAddChat.addEventListener('click', openAddChatModal);

el.btnSend.addEventListener('click', sendMessage);

el.messageInput.addEventListener('keydown', e => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendMessage();
    }
});

function autoResizeTextarea() {
    const t = el.messageInput;
    t.style.height = 'auto';
    t.style.height = Math.min(t.scrollHeight, 160) + 'px';
}
el.messageInput.addEventListener('input', autoResizeTextarea);

// ── Push events from main process ─────────────────────────────────────────────

// Show "Starting up…" only if the main process takes longer than 300 ms to
// respond. Fast startups skip this screen entirely.
const startupTimer = setTimeout(() => {
    if (!state.appReady) {
        el.emptyState.style.display   = 'flex';
        el.emptyTitle.textContent     = 'Starting up…';
        el.emptySubtitle.textContent  = 'Initialising the d-comms library';
        el.globalSpinner.style.display = 'block';
    }
}, 300);

window.dcomms.onReady(data => {
    clearTimeout(startupTimer);
    state.appReady = true;
    state.syncPort = data.syncPort;
    updatePeerBadge(data.peerCount || 0);

    if (data.savedChats && data.savedChats.length > 0) {
        state.chats = data.savedChats.map(name => ({ name, loaded: false, chatId: null }));
        showEmptyState('Select a chat', 'Choose a chat from the sidebar or create a new one.');
    } else {
        showEmptyState(
            'Welcome to D-Comms',
            'Create or join a chat to get started with encrypted P2P messaging.',
        );
    }
    renderSidebar();
    updateSyncIndicator();
});

window.dcomms.onError(data => {
    showEmptyState('Error', data.message || 'An error occurred.');
});

window.dcomms.onMessagesUpdate(data => {
    const { chatId, name, messages, state: chatStateVal } = data;

    state.messages[chatId]   = messages;
    state.chatState[chatId]  = chatStateVal;

    if (chatId === state.currentChatId) {
        renderMessages();
        updateTurnBar();
    } else {
        // Track unread count
        const prev = (state.messages[chatId] || []).length;
        state.unreadCounts[name] = (state.unreadCounts[name] || 0) + Math.max(0, messages.length - prev);
        renderSidebar();
    }
});

window.dcomms.onPeersUpdate(data => {
    updatePeerBadge(data.count || 0, data.peers || []);
});

window.dcomms.onSyncStart(() => {
    state.syncing = true;
    updateSyncIndicator();
});

window.dcomms.onSyncDone(() => {
    state.syncing = false;
    updateSyncIndicator();
});

// ── Keyboard shortcuts ────────────────────────────────────────────────────────

document.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
        document.querySelectorAll('.modal-backdrop').forEach(m => {
            if (m.style.display !== 'none') closeModal(m.id);
        });
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'n') {
        e.preventDefault();
        openAddChatModal();
    }
});
