'use strict';

const { app, BrowserWindow, ipcMain, shell, Menu } = require('electron');
const path = require('path');
const fs   = require('fs');
const net  = require('net');

// ── State ────────────────────────────────────────────────────────────────────

let mainWindow  = null;
let addon       = null;
let syncPort    = -1;

// name → { chatId, userKey, secretId, isInitiator }
const loadedChats   = new Map();
// chatId → message count (for change detection)
const messageCounts = new Map();
// Names of .chat files found on disk (not yet loaded)
const savedChatNames = new Set();

let syncInterval    = null;
let pollInterval    = null;
let syncInProgress  = false;

// "host:port" → timestamp (ms) of last successful TCP probe
const peerLastSeen = new Map();

// ── Helpers ──────────────────────────────────────────────────────────────────

function userData() {
    return app.getPath('userData');
}

function chatsDir() {
    return path.join(userData(), 'chats');
}

function scanSavedChats() {
    const dir = chatsDir();
    if (!fs.existsSync(dir)) return;
    for (const f of fs.readdirSync(dir)) {
        if (f.endsWith('.chat')) {
            savedChatNames.add(f.slice(0, -5));
        }
    }
}

function pushToRenderer(channel, data) {
    if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send(channel, data);
    }
}

function checkNewMessages() {
    if (!addon) return;
    for (const [name, info] of loadedChats) {
        try {
            const msgs = addon.getMessages(info.chatId);
            const prev = messageCounts.get(info.chatId) ?? 0;
            if (msgs.length !== prev) {
                messageCounts.set(info.chatId, msgs.length);
                // Also refresh state so the UI knows if it's the user's turn
                const chatInfo = addon.getChatInfo(info.chatId);
                pushToRenderer('messages:update', {
                    chatId:   info.chatId,
                    name,
                    messages: msgs,
                    state:    chatInfo ? chatInfo.state : 0,
                });
            }
        } catch (_) {}
    }
}

function probePeer(host, port) {
    return new Promise(resolve => {
        const sock = net.connect({ host, port, timeout: 300 });
        sock.on('connect', () => { sock.destroy(); resolve(true);  });
        sock.on('error',   () => resolve(false));
        sock.on('timeout', () => { sock.destroy(); resolve(false); });
    });
}

async function getLivePeers() {
    const regPath = path.join(userData(), 'registry.db');
    if (!fs.existsSync(regPath)) return { count: 0, peers: [] };
    try {
        // Collect unique entries that are not our own sync port
        const seen  = new Set();
        const peers = [];
        for (const raw of fs.readFileSync(regPath, 'utf8').split('\n')) {
            const l = raw.trim();
            if (!l || seen.has(l)) continue;
            seen.add(l);
            const i = l.lastIndexOf(':');
            if (i < 0) continue;
            const host = l.slice(0, i);
            const port = parseInt(l.slice(i + 1), 10);
            if (!isNaN(port) && port !== syncPort) peers.push({ host, port });
        }
        const results = await Promise.all(peers.map(async p => {
            const key  = `${p.host}:${p.port}`;
            const live = await probePeer(p.host, p.port);
            if (live) peerLastSeen.set(key, Date.now());
            return { host: p.host, port: p.port, live, lastSeen: peerLastSeen.get(key) || null };
        }));
        return { count: results.filter(r => r.live).length, peers: results };
    } catch (_) { return { count: 0, peers: [] }; }
}

function startLoops() {
    // Sync with peers every 500ms; skip if a sync is already running
    syncInterval = setInterval(async () => {
        if (!addon || syncInProgress) return;
        syncInProgress = true;
        pushToRenderer('sync:start', {});
        try {
            const added = await addon.syncWithPeers();
            pushToRenderer('sync:done', { added });
            if (added > 0) checkNewMessages();
            const { count, peers } = await getLivePeers();
            pushToRenderer('peers:update', { count, peers });
        } catch (_) {
        } finally {
            syncInProgress = false;
        }
    }, 500);

    // Poll for local message count changes every 1 second
    pollInterval = setInterval(checkNewMessages, 1000);
}

// ── App initialisation ───────────────────────────────────────────────────────

async function initAddon() {
    // IMPORTANT: set CWD to userData so d-comms writes messages.db there
    const ud = userData();
    fs.mkdirSync(ud, { recursive: true });
    process.chdir(ud);

    try {
        addon = require('bindings')('dcomms_addon');
    } catch (err) {
        console.error('Failed to load native addon:', err.message);
        console.error('Did you run `npm run build` first?');
        pushToRenderer('app:error', { message: 'Native addon not built. Run `npm run build` first.' });
        return;
    }

    // Start sync server
    syncPort = addon.startServer();
    if (syncPort > 0) {
        addon.register(syncPort);
        addon.startDHT(syncPort, ud);
    }

    // Scan saved chats (just names, no KDF yet)
    scanSavedChats();

    startLoops();

    pushToRenderer('app:ready', {
        syncPort,
        savedChats: [...savedChatNames],
    });
}

// ── IPC Handlers ─────────────────────────────────────────────────────────────

// Returns list of all known chats (saved + loaded)
ipcMain.handle('chat:list', () => {
    const all = new Set([...savedChatNames, ...loadedChats.keys()]);
    return [...all].map(name => {
        const info = loadedChats.get(name);
        return {
            name,
            loaded:      !!info,
            chatId:      info?.chatId      ?? null,
            userKey:     info?.userKey     ?? null,
            secretId:    info?.secretId    ?? null,
            isInitiator: info?.isInitiator ?? null,
        };
    });
});

// Open (load) a saved chat — triggers 5-second KDF
ipcMain.handle('chat:open', async (_, { name }) => {
    if (loadedChats.has(name)) {
        const info = loadedChats.get(name);
        return { success: true, ...info };
    }
    if (!savedChatNames.has(name)) {
        return { success: false, error: 'Chat not found on disk' };
    }
    if (!addon) return { success: false, error: 'Addon not loaded' };
    try {
        const result = await addon.loadChat(name, userData());
        const info = {
            chatId:      result.chatId,
            userKey:     result.userKey,
            secretId:    result.secretId,
            isInitiator: result.isInitiator,
        };
        loadedChats.set(name, info);
        messageCounts.set(result.chatId, 0);
        addon.addDHTChat(result.secretId);
        return { success: true, name, ...info };
    } catch (err) {
        return { success: false, error: err.message };
    }
});

// Create a new chat — triggers 5-second KDF
ipcMain.handle('chat:create', async (_, { name }) => {
    if (!addon) return { success: false, error: 'Addon not loaded' };
    try {
        const result = await addon.createChat(name);
        const info = {
            chatId:      result.chatId,
            userKey:     result.userKey,
            secretId:    result.secretId,
            isInitiator: true,
        };
        loadedChats.set(name, info);
        savedChatNames.add(name);
        messageCounts.set(result.chatId, 0);
        addon.saveChat(result.chatId, name, userData());
        addon.addDHTChat(result.secretId);

        return { success: true, name, ...info };
    } catch (err) {
        return { success: false, error: err.message };
    }
});

// Join an existing chat — triggers 5-second KDF
ipcMain.handle('chat:join', async (_, { name, userKey, secretId }) => {
    if (!addon) return { success: false, error: 'Addon not loaded' };
    try {
        const result = await addon.joinChat(name, userKey, secretId);
        const info = {
            chatId:      result.chatId,
            userKey:     result.userKey,
            secretId:    result.secretId,
            isInitiator: false,
        };
        loadedChats.set(name, info);
        savedChatNames.add(name);
        messageCounts.set(result.chatId, 0);
        addon.saveChat(result.chatId, name, userData());
        addon.addDHTChat(result.secretId);
        return { success: true, name, ...info };
    } catch (err) {
        return { success: false, error: err.message };
    }
});

// Retrieve messages for a loaded chat
ipcMain.handle('chat:messages', (_, { chatId }) => {
    if (!addon) return [];
    try {
        return addon.getMessages(chatId);
    } catch (_) { return []; }
});

// Retrieve per-chat metadata (keys, state, role)
ipcMain.handle('chat:info', (_, { chatId }) => {
    if (!addon) return null;
    try { return addon.getChatInfo(chatId); } catch (_) { return null; }
});

// Send a message
ipcMain.handle('chat:send', (_, { chatId, text }) => {
    if (!addon) return { success: false, error: 'Addon not loaded' };
    try { return addon.sendMessage(chatId, text); } catch (err) {
        return { success: false, error: err.message };
    }
});

// Delete a chat (unload + remove .chat file)
ipcMain.handle('chat:delete', (_, { name, chatId }) => {
    try {
        if (addon && chatId != null) addon.destroyChat(chatId);
        loadedChats.delete(name);
        messageCounts.delete(chatId);
        savedChatNames.delete(name);
        const f = path.join(chatsDir(), `${name}.chat`);
        if (fs.existsSync(f)) fs.unlinkSync(f);
        return { success: true };
    } catch (err) {
        return { success: false, error: err.message };
    }
});

// Manually add a sync peer
ipcMain.handle('sync:add-peer', (_, { host, port }) => {
    if (addon) addon.addPeer(host, port);
});

// Trigger an immediate sync
ipcMain.handle('sync:now', async () => {
    if (!addon) return { added: 0 };
    try {
        const added = await addon.syncWithPeers();
        if (added > 0) checkNewMessages();
        return { added };
    } catch (_) { return { added: 0 }; }
});

// App status
ipcMain.handle('app:status', () => {
    const status = addon ? addon.getStatus() : {};
    return { ...status, syncPort, ready: !!addon };
});

// Open userData folder in system file manager
ipcMain.handle('app:open-data-dir', () => shell.openPath(userData()));

// ── Window & lifecycle ───────────────────────────────────────────────────────

function createWindow() {
    mainWindow = new BrowserWindow({
        width:           1100,
        height:          720,
        minWidth:        700,
        minHeight:       480,
        backgroundColor: '#202225',
        webPreferences: {
            preload:          path.join(__dirname, 'preload.js'),
            contextIsolation: true,
            nodeIntegration:  false,
        },
    });

    mainWindow.loadFile(path.join(__dirname, 'renderer', 'index.html'));

    mainWindow.on('closed', () => { mainWindow = null; });

    // Wait for the renderer to finish loading before initialising the addon,
    // so the app:ready push event is never sent before the listener is registered.
    // If the window is reopened after a close, the addon is already running —
    // just push the current state instead of re-initialising.
    mainWindow.webContents.once('did-finish-load', () => {
        if (!addon) {
            initAddon().catch(err => console.error('initAddon error:', err));
        } else {
            pushToRenderer('app:ready', {
                syncPort,
                savedChats: [...savedChatNames],
            });
        }
    });
}

app.whenReady().then(() => {
    Menu.setApplicationMenu(null);
    createWindow();

    // macOS: recreate the window when the dock icon is clicked and no windows
    // are open (standard macOS app behaviour).
    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('before-quit', () => {
    clearInterval(syncInterval);
    clearInterval(pollInterval);
    // Schedule a hard exit as a fallback in case native threads (DHT select
    // loop, UPnP cleanup) stall the normal Electron shutdown on macOS.
    // .unref() ensures this timer doesn't prevent a clean exit if it isn't needed.
    setTimeout(() => process.exit(0), 2000).unref();
});

app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
});
