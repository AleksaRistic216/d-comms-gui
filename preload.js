'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('dcomms', {
    // Chat management
    listChats:   ()                          => ipcRenderer.invoke('chat:list'),
    openChat:    (name)                      => ipcRenderer.invoke('chat:open',   { name }),
    createChat:  (name)                      => ipcRenderer.invoke('chat:create', { name }),
    joinChat:    (name, userKey, secretId)   => ipcRenderer.invoke('chat:join',   { name, userKey, secretId }),
    getMessages: (chatId)                    => ipcRenderer.invoke('chat:messages', { chatId }),
    getChatInfo: (chatId)                    => ipcRenderer.invoke('chat:info',     { chatId }),
    sendMessage: (chatId, text)              => ipcRenderer.invoke('chat:send',     { chatId, text }),
    deleteChat:  (name, chatId)              => ipcRenderer.invoke('chat:delete',   { name, chatId }),

    // Sync
    addPeer:  (host, port) => ipcRenderer.invoke('sync:add-peer', { host, port }),
    syncNow:  ()           => ipcRenderer.invoke('sync:now'),

    // App
    getStatus:   ()  => ipcRenderer.invoke('app:status'),
    openDataDir: ()  => ipcRenderer.invoke('app:open-data-dir'),

    // Push events from main process
    onReady:          (cb) => ipcRenderer.on('app:ready',        (_e, d) => cb(d)),
    onError:          (cb) => ipcRenderer.on('app:error',        (_e, d) => cb(d)),
    onMessagesUpdate: (cb) => ipcRenderer.on('messages:update',  (_e, d) => cb(d)),
    onSyncStart:      (cb) => ipcRenderer.on('sync:start',       (_e, d) => cb(d)),
    onSyncDone:       (cb) => ipcRenderer.on('sync:done',        (_e, d) => cb(d)),
    onPeersUpdate:    (cb) => ipcRenderer.on('peers:update',     (_e, d) => cb(d)),

    // Clipboard helper (renderer-side, no IPC needed)
    copyToClipboard: (text) => navigator.clipboard.writeText(text),
});
